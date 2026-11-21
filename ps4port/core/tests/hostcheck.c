#include "bgft_symbols.h"
#include "ps4u_diag.h"
#include "ps4u_files.h"
#include "ps4u_http.h"
#include "ps4u_status.h"
#include "upload.h"

#include <stdio.h>
#include <string.h>

static int checks;
static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                    \
    } while (0)

/* Baut aus einer Anfragezeile denselben Kopfblock, den der Payload aus dem
   Socket schneidet. So laufen die Tests ueber ps4u_parse_head(), also ueber
   den Pfad, den der Payload tatsaechlich benutzt. */
static int parse_line(const char *line, ps4u_request *out)
{
    char block[512];
    int len = snprintf(block, sizeof(block), "%s\r\n\r\n", line);

    if (len < 0 || (size_t)len >= sizeof(block)) {
        return -1;
    }
    return ps4u_parse_head(block, (size_t)len, out);
}

static void test_request_line(void)
{
    ps4u_request request;
    char long_name[135];
    char query[256];

    CHECK(parse_line("GET /status HTTP/1.1", &request) == 0);
    CHECK(request.method == PS4U_METHOD_GET);
    CHECK(request.route == PS4U_ROUTE_STATUS);
    CHECK(request.name[0] == '\0');

    CHECK(parse_line("POST /upload?name=My%20Game.pkg HTTP/1.1", &request) == 0);
    CHECK(request.method == PS4U_METHOD_POST);
    CHECK(request.route == PS4U_ROUTE_UPLOAD);
    CHECK(strcmp(request.name, "My Game.pkg") == 0);

    CHECK(parse_line("POST /upload?name=a+b.pkg HTTP/1.1", &request) == 0);
    CHECK(strcmp(request.name, "a b.pkg") == 0);

    CHECK(parse_line("GET / HTTP/1.0", &request) == 0);
    CHECK(request.route == PS4U_ROUTE_ROOT);

    CHECK(parse_line("GET /missing HTTP/1.1", &request) == 0);
    CHECK(request.route == PS4U_ROUTE_UNKNOWN);

    CHECK(parse_line("PATCH /status HTTP/1.1", &request) == 0);
    CHECK(request.method == PS4U_METHOD_OTHER);
    CHECK(request.route == PS4U_ROUTE_STATUS);

    CHECK(parse_line("GET", &request) == -1);
    CHECK(parse_line("GET /status", &request) == -1);
    CHECK(parse_line("GET status HTTP/1.1", &request) == -1);
    CHECK(parse_line("GET /upload?name=bad%zz.pkg HTTP/1.1", &request) == -1);
    CHECK(parse_line("GET /upload?name=a%00.pkg HTTP/1.1", &request) == -1);

    /* 134 Zeichen Name passen nicht in ps4u_request.name (128). */
    memset(long_name, 'a', 130);
    memcpy(long_name + 130, ".pkg", 5);
    snprintf(query, sizeof(query), "POST /upload?name=%s HTTP/1.1", long_name);
    CHECK(parse_line(query, &request) == -1);
}

static void test_headers(void)
{
    const char *headers =
        "Host: 192.168.0.50\r\n"
        "X-PS4U-Token:  abc123 \r\n"
        "X-Upload-Pin:\t1234\t\r\n"
        "\r\n";
    char value[32];

    CHECK(ps4u_header_value(headers, "x-ps4u-token", value, sizeof(value)) == 0);
    CHECK(strcmp(value, "abc123") == 0);

    CHECK(ps4u_header_value(headers, "X-Upload-Pin", value, sizeof(value)) == 0);
    CHECK(strcmp(value, "1234") == 0);

    CHECK(ps4u_header_value(headers, "X-Missing", value, sizeof(value)) == -1);
    CHECK(ps4u_header_value(headers, "X-PS4U-Token", value, 4) == -1);
    CHECK(ps4u_header_value(NULL, "Host", value, sizeof(value)) == -1);
}

static void test_pkg_names(void)
{
    CHECK(ps4u_safe_pkg_name("Game.pkg") == 0);
    CHECK(ps4u_safe_pkg_name("My Game.PKG") == 0);
    CHECK(ps4u_safe_pkg_name("../Game.pkg") == -1);
    CHECK(ps4u_safe_pkg_name("a/Game.pkg") == -1);
    CHECK(ps4u_safe_pkg_name("Game.sfo") == -1);
    CHECK(ps4u_safe_pkg_name(".hidden.pkg") == -1);
    CHECK(ps4u_safe_pkg_name("Game.pkg ") == -1);
    CHECK(ps4u_safe_pkg_name("") == -1);
    CHECK(ps4u_safe_pkg_name(NULL) == -1);
}

static void test_routes(void)
{
    CHECK(ps4u_route_status(PS4U_ROUTE_ROOT, PS4U_METHOD_GET) == 200);
    CHECK(ps4u_route_status(PS4U_ROUTE_ROOT, PS4U_METHOD_POST) == 405);
    CHECK(ps4u_route_status(PS4U_ROUTE_STATUS, PS4U_METHOD_GET) == 200);
    CHECK(ps4u_route_status(PS4U_ROUTE_STATUS, PS4U_METHOD_POST) == 405);
    CHECK(ps4u_route_status(PS4U_ROUTE_UPLOAD, PS4U_METHOD_POST) == 200);
    CHECK(ps4u_route_status(PS4U_ROUTE_UPLOAD, PS4U_METHOD_GET) == 405);
    CHECK(ps4u_route_status(PS4U_ROUTE_FILES, PS4U_METHOD_GET) == 200);
    CHECK(ps4u_route_status(PS4U_ROUTE_FILES, PS4U_METHOD_POST) == 405);
    CHECK(ps4u_route_status(PS4U_ROUTE_DIAGNOSTICS, PS4U_METHOD_GET) == 200);
    CHECK(ps4u_route_status(PS4U_ROUTE_DIAGNOSTICS, PS4U_METHOD_HEAD) == 405);
    CHECK(ps4u_route_status(PS4U_ROUTE_DIAGNOSTICS, PS4U_METHOD_DELETE) == 405);
    CHECK(ps4u_route_status(PS4U_ROUTE_UNKNOWN, PS4U_METHOD_GET) == 404);

    {
        ps4u_request request;

        CHECK(parse_line("GET /diagnostics HTTP/1.1", &request) == 0);
        CHECK(request.route == PS4U_ROUTE_DIAGNOSTICS);
        CHECK(request.method == PS4U_METHOD_GET);
        CHECK(parse_line("POST /diagnostics HTTP/1.1", &request) == 0);
        CHECK(ps4u_route_status(request.route, request.method) == 405);
    }
}

static void test_status_json(void)
{
    ps4u_status status;
    char out[512];
    char small[24];
    size_t needed;

    memset(&status, 0, sizeof(status));
    status.version = "0.1.0";
    status.phase = "idle";
    status.uptime_s = 12;
    status.clients = 2;
    status.queue = 1;
    status.percent = 0;
    status.free_bytes = 1024;

    needed = ps4u_status_json(&status, out, sizeof(out));
    CHECK(needed == strlen(out));
    CHECK(strcmp(out,
                 "{\"ok\":true,\"version\":\"0.1.0\",\"uptime_s\":12,"
                 "\"clients\":2,\"queue\":1,\"free_bytes\":1024,"
                 "\"install\":{\"phase\":\"idle\",\"percent\":0}}") == 0);

    status.phase = "error";
    status.title = "Game \"One\"";
    status.error = "line\nbreak";
    status.percent = 150;
    ps4u_status_json(&status, out, sizeof(out));
    CHECK(strstr(out, "\"percent\":100") != NULL);
    CHECK(strstr(out, "\"title\":\"Game \\\"One\\\"\"") != NULL);
    CHECK(strstr(out, "\"error\":\"line\\nbreak\"") != NULL);

    status.percent = -5;
    ps4u_status_json(&status, out, sizeof(out));
    CHECK(strstr(out, "\"percent\":0") != NULL);

    status.percent = 0;
    status.title = "a\x01" "b";
    ps4u_status_json(&status, out, sizeof(out));
    CHECK(strstr(out, "\"title\":\"a\\u0001b\"") != NULL);

    memset(small, 0x5a, sizeof(small));
    needed = ps4u_status_json(&status, small, 23);
    CHECK(needed > 23);
    CHECK((unsigned char)small[23] == 0x5a);
    CHECK(strlen(small) == 22);
}

static void test_responses(void)
{
    char buffer[512];
    char tiny[16];
    size_t len;

    len = ps4u_response_head_extra(200, "application/json", 7, NULL, buffer,
                                   sizeof(buffer));
    CHECK(strcmp(buffer,
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: 7\r\n"
                 "Cache-Control: no-store\r\n"
                 "Connection: close\r\n\r\n") == 0);
    CHECK(len == strlen(buffer));

    len = ps4u_error_response(404, buffer, sizeof(buffer));
    CHECK(len == strlen(buffer));
    CHECK(strncmp(buffer, "HTTP/1.1 404 Not Found\r\n", 24) == 0);
    CHECK(strstr(buffer, "Content-Length: 10\r\n") != NULL);
    CHECK(strstr(buffer, "\r\n\r\nnot found\n") != NULL);

    len = ps4u_error_response(405, buffer, sizeof(buffer));
    CHECK(len == strlen(buffer));
    CHECK(strncmp(buffer, "HTTP/1.1 405 Method Not Allowed\r\n", 33) == 0);
    CHECK(strstr(buffer, "\r\n\r\nmethod not allowed\n") != NULL);

    /* Zu kleiner Puffer: kein Ueberlauf, volle Laenge gemeldet. */
    memset(tiny, 0x5a, sizeof(tiny));
    len = ps4u_error_response(400, tiny, sizeof(tiny));
    CHECK(len > sizeof(tiny));
    CHECK(tiny[sizeof(tiny) - 1] == '\0');
}

static void test_resume(void)
{
    ps4u_request request;
    char buffer[512];

    CHECK(parse_line("HEAD /upload?name=a.pkg HTTP/1.1", &request) == 0);
    CHECK(request.method == PS4U_METHOD_HEAD);
    CHECK(request.route == PS4U_ROUTE_UPLOAD);

    CHECK(parse_line("DELETE /upload?name=a.pkg HTTP/1.1", &request) == 0);
    CHECK(request.method == PS4U_METHOD_DELETE);

    CHECK(parse_line("POST /upload?name=a.pkg&offset=1234 HTTP/1.1",
                     &request) == 0);
    CHECK(request.offset == 1234);
    CHECK(strcmp(request.name, "a.pkg") == 0);

    CHECK(parse_line("POST /upload?offset=12&name=b.pkg HTTP/1.1",
                     &request) == 0);
    CHECK(request.offset == 12);
    CHECK(strcmp(request.name, "b.pkg") == 0);

    CHECK(parse_line("POST /upload?offset=0&name=a.pkg HTTP/1.0",
                     &request) == 0);
    CHECK(request.offset == 0);

    /* Ungueltige Offsets machen die ganze Anfrage ungueltig. */
    CHECK(parse_line("POST /upload?offset=&name=a.pkg HTTP/1.0",
                     &request) != 0);
    CHECK(parse_line("POST /upload?offset=12a&name=a.pkg HTTP/1.0",
                     &request) != 0);
    CHECK(parse_line("POST /upload?offset=-1&name=a.pkg HTTP/1.0",
                     &request) != 0);
    CHECK(parse_line("POST /upload?offset=999999999999&name=a.pkg HTTP/1.0",
                     &request) != 0);

    CHECK(ps4u_response_head_extra(200, "text/plain", 0,
                                   "X-Upload-Offset: 42\r\n", buffer,
                                   sizeof(buffer)) == strlen(buffer));
    CHECK(strstr(buffer, "X-Upload-Offset: 42\r\n") != NULL);
    CHECK(strstr(buffer, "Content-Length: 0\r\n") != NULL);
    CHECK(strstr(buffer, "\r\n\r\n") != NULL);
}

/* Zerlegen von Puffer -> Kopfblock -> Anfragezeile: der Fall, der im Payload
   vorher am ersten CRLF abgeschnitten wurde. */
static void test_request_block(void)
{
    const char *post =
        "POST /upload?name=My%20Game.pkg&offset=120 HTTP/1.1\r\n"
        "Host: 192.168.0.50\r\n"
        "Content-Length: 4194304\r\n"
        "X-Upload-Pin: 4711\r\n"
        "\r\n";
    const size_t post_len = strlen(post);
    char combined[512];
    char head[256];
    char big[PS4U_HEAD_MAX + 64];
    ps4u_request request;
    unsigned long long length = 0;

    memcpy(combined, post, post_len);
    memcpy(combined + post_len, "\x7f" "CNT payload", 12);

    /* Body im selben Puffer aendert die Blocklaenge nicht. */
    CHECK(ps4u_header_block_length(combined, post_len + 12) == post_len);
    CHECK(ps4u_header_block_length(post, post_len) == post_len);
    CHECK(ps4u_header_block_length(post, post_len - 2) == 0);
    CHECK(ps4u_header_block_length(post, 3) == 0);
    CHECK(ps4u_header_block_length(NULL, post_len) == 0);

    /* Der Parser bekommt genau die Blocklaenge, die der Finder geliefert hat. */
    CHECK(ps4u_parse_head(combined, post_len, &request) == 0);
    CHECK(request.method == PS4U_METHOD_POST);
    CHECK(request.route == PS4U_ROUTE_UPLOAD);
    CHECK(strcmp(request.name, "My Game.pkg") == 0);
    CHECK(request.offset == 120);

    /* Nur die Anfragezeile, kein Header: geht auch. */
    CHECK(ps4u_parse_head("GET /status HTTP/1.1\r\n\r\n", 24, &request) == 0);
    CHECK(request.route == PS4U_ROUTE_STATUS);

    /* Ohne vollstaendigen Block oder ohne HTTP-Version: unbrauchbar. */
    CHECK(ps4u_parse_head(post, post_len - 2, &request) == -1);
    CHECK(ps4u_parse_head(post, post_len - 4, &request) == -1);
    CHECK(ps4u_parse_head("GET /status\r\n\r\n", 16, &request) == -1);
    CHECK(ps4u_parse_head(NULL, 64, &request) == -1);
    CHECK(ps4u_parse_head(combined, 0, &request) == -1);

    memset(big, 'A', sizeof(big));
    memcpy(big + sizeof(big) - 4, "\r\n\r\n", 4);
    CHECK(ps4u_header_block_length(big, sizeof(big)) == sizeof(big));
    CHECK(ps4u_parse_head(big, sizeof(big), &request) == -1);

    strcpy(head, post);
    CHECK(ps4u_content_length(head, &length) == 0);
    CHECK(length == 4194304ull);

    strcpy(head, "POST /upload HTTP/1.1\r\nContent-length: 0\r\n\r\n");
    CHECK(ps4u_content_length(head, &length) == 0);
    CHECK(length == 0);

    strcpy(head, "POST /upload HTTP/1.1\r\nContent-Length: 12a\r\n\r\n");
    CHECK(ps4u_content_length(head, &length) == -1);

    strcpy(head, "POST /upload HTTP/1.1\r\nContent-Length:\r\n\r\n");
    CHECK(ps4u_content_length(head, &length) == -1);

    strcpy(head, "POST /upload HTTP/1.1\r\nHost: ps4\r\n\r\n");
    CHECK(ps4u_content_length(head, &length) == -1);

    strcpy(head, "POST /upload HTTP/1.1\r\nContent-Length: 68719476737\r\n\r\n");
    CHECK(ps4u_content_length(head, &length) == -1);

    CHECK(ps4u_content_length(NULL, &length) == -1);
}

/* Quote aus config.txt und die Platzpruefung fuer GET /files. */
static void test_files(void)
{
    ps4u_file_entry entries[2];
    char json[256];
    char small[16];

    CHECK(ps4u_route_status(PS4U_ROUTE_FILES, PS4U_METHOD_GET) == 200);
    CHECK(ps4u_route_status(PS4U_ROUTE_FILES, PS4U_METHOD_POST) == 405);

    CHECK(ps4u_listable_file("game.pkg") == 1);
    CHECK(ps4u_listable_file("My Game.PKG") == 1);
    CHECK(ps4u_listable_file("game.pkg.part") == 1);
    CHECK(ps4u_listable_file("game.part") == 0);
    CHECK(ps4u_listable_file("config.txt") == 0);
    CHECK(ps4u_listable_file("pin.txt") == 0);
    CHECK(ps4u_listable_file(".pkg") == 0);
    CHECK(ps4u_listable_file(NULL) == 0);

    CHECK(ps4u_config_quota("quota_bytes=1073741824\n") == 1073741824ull);
    CHECK(ps4u_config_quota("quota_bytes = 2048\r\n") == 2048);
    CHECK(ps4u_config_quota("  quota_bytes=4096  \n") == 4096);
    CHECK(ps4u_config_quota("port=9180\nquota_bytes=1024\n") == 1024);
    CHECK(ps4u_config_quota("quota_bytes=1024\nquota_bytes=2048") == 2048);
    CHECK(ps4u_config_quota("# quota_bytes=99\n") == 0);
    CHECK(ps4u_config_quota("quota_bytes=0\n") == 0);
    CHECK(ps4u_config_quota("quota_bytes=\n") == 0);
    CHECK(ps4u_config_quota("quota_bytes=12 MB\n") == 0);
    CHECK(ps4u_config_quota("quota=5\n") == 0);
    CHECK(ps4u_config_quota("quota_bytes=99999999999999999999999\n") == 0);
    CHECK(ps4u_config_quota("") == 0);
    CHECK(ps4u_config_quota(NULL) == 0);

    CHECK(ps4u_quota_ok(0, 10, 0) == 1);
    CHECK(ps4u_quota_ok(0, 1, 1) == 1);
    CHECK(ps4u_quota_ok(95, 5, 100) == 1);
    CHECK(ps4u_quota_ok(96, 5, 100) == 0);
    CHECK(ps4u_quota_ok(100, 0, 100) == 1);
    CHECK(ps4u_quota_ok(200, 0, 100) == 0);

    entries[0].name = "a.pkg";
    entries[0].bytes = 12;
    entries[1].name = "b\"q\".pkg";
    entries[1].bytes = 3456;

    CHECK(ps4u_files_json(entries, 2, 1000, 5000, 77, json, sizeof(json)) ==
          strlen(json));
    CHECK(strcmp(json,
                 "{\"ok\":true,\"used\":1000,\"quota\":5000,"
                 "\"free_bytes\":77,\"files\":["
                 "{\"name\":\"a.pkg\",\"bytes\":12},"
                 "{\"name\":\"b\\\"q\\\".pkg\",\"bytes\":3456}]}") == 0);

    CHECK(ps4u_files_json(NULL, 0, 0, 0, 0, json, sizeof(json)) == strlen(json));
    CHECK(strcmp(json,
                 "{\"ok\":true,\"used\":0,\"quota\":0,\"free_bytes\":0,"
                 "\"files\":[]}") == 0);

    /* Zu kleiner Puffer: es wird nur abgeschnitten, die Laenge stimmt trotzdem. */
    CHECK(ps4u_files_json(entries, 2, 1000, 5000, 77, small, sizeof(small)) >
          sizeof(small) - 1);
    CHECK(strlen(small) == sizeof(small) - 1);
}

/* BGFT-Reserve: ohne die Bytes der .part-Datei scheitert ein Resume, obwohl
   nach dem Umbenennen genauso viel Platz frei ist. */
static void test_upload_space(void)
{
    const unsigned long long mib = 1024ull * 1024ull;

    CHECK(upload_bgft_space_ok(10 * mib, 100 * mib, 64 * mib) == 1);
    CHECK(upload_bgft_space_ok(80 * mib, 200 * mib, 64 * mib) == 0);

    CHECK(upload_bgft_space_ok_resume(80 * mib, 200 * mib, 40 * mib, 64 * mib) == 1);
    CHECK(upload_bgft_space_ok_resume(80 * mib, 200 * mib, 0, 64 * mib) == 0);
    CHECK(upload_bgft_space_ok_resume(0, 0, 0, 64 * mib) == 0);
    CHECK(upload_bgft_space_ok_resume(1, ~(unsigned long long)0, 5, 0) == 1);

    /* Dieselbe Header-Datei bringt Timeout und die einfache Platzpruefung mit. */
    CHECK(upload_idle_expired(100000, 0, 30000) == 0);
    CHECK(upload_idle_expired(100000, 70000, 30000) == 1);
    CHECK(upload_idle_expired(5000, 10000, 30000) == 0);
    CHECK(upload_space_ok(100, 200, 64) == 1);
    CHECK(upload_space_ok(140, 200, 64) == 0);
    CHECK(upload_space_ok(300, 200, 64) == 0);
}

/* Resume-Entscheidung: schon fertig, abschliessen, anhaengen oder 409. */
static void test_resume_plan(void)
{
    int complete = 0;

    CHECK(ps4u_resume_plan_for(0, 100, 0, 0) == PS4U_RESUME_APPEND);
    CHECK(ps4u_resume_plan_for(0, 100, 50, 0) == PS4U_RESUME_APPEND);
    CHECK(ps4u_resume_plan_for(100, 5, 100, 0) == PS4U_RESUME_APPEND);
    CHECK(ps4u_resume_plan_for(100, 0, 100, 0) == PS4U_RESUME_FINALIZE);
    CHECK(ps4u_resume_plan_for(100, 0, 100, 100) == PS4U_RESUME_FINALIZE);

    /* Fertige Datei, .part weg: der wiederholte Abschluss ist kein Fehler. */
    CHECK(ps4u_resume_plan_for(100, 0, 0, 100) == PS4U_RESUME_ALREADY);
    CHECK(ps4u_resume_plan_for(100, 0, 0, 99) == PS4U_RESUME_CONFLICT);
    CHECK(ps4u_resume_plan_for(100, 0, 0, 101) == PS4U_RESUME_CONFLICT);
    CHECK(ps4u_resume_plan_for(100, 0, 50, 0) == PS4U_RESUME_CONFLICT);
    CHECK(ps4u_resume_plan_for(100, 5, 0, 100) == PS4U_RESUME_CONFLICT);
    CHECK(ps4u_resume_plan_for(100, 5, 99, 0) == PS4U_RESUME_CONFLICT);

    CHECK(ps4u_resume_offset(0, 0, &complete) == 0);
    CHECK(complete == 0);
    CHECK(ps4u_resume_offset(50, 0, &complete) == 50);
    CHECK(complete == 0);
    CHECK(ps4u_resume_offset(0, 300, &complete) == 300);
    CHECK(complete == 1);
    /* Ein laufender Rest hat Vorrang vor einer aelteren fertigen Datei. */
    CHECK(ps4u_resume_offset(50, 300, &complete) == 50);
    CHECK(complete == 0);
    CHECK(ps4u_resume_offset(0, 300, NULL) == 300);
}

/* Die gemeinsame BGFT-Liste: Reihenfolge, Namen und Eindeutigkeit. Aendert
   sie sich, bricht hier der Hosttest, nicht erst /diagnostics auf der Konsole. */
static void test_bgft_symbols(void)
{
    size_t i;
    size_t k;

    CHECK(PS4U_BGFT_SYMBOL_COUNT == 6);
    CHECK(strcmp(ps4u_bgft_symbols[PS4U_BGFT_SYMBOL_INIT],
                 "sceBgftServiceIntInit") == 0);
    CHECK(strcmp(ps4u_bgft_symbols[PS4U_BGFT_SYMBOL_REGISTER_STORAGE],
                 "sceBgftServiceIntDownloadRegisterTaskByStorageEx") == 0);
    CHECK(strcmp(ps4u_bgft_symbols[PS4U_BGFT_SYMBOL_START],
                 "sceBgftServiceIntDownloadStartTask") == 0);
    CHECK(strcmp(ps4u_bgft_symbols[PS4U_BGFT_SYMBOL_PROGRESS],
                 "sceBgftServiceIntDownloadGetProgress") == 0);
    CHECK(strcmp(ps4u_bgft_symbols[PS4U_BGFT_SYMBOL_STOP],
                 "sceBgftServiceIntDownloadStopTask") == 0);
    CHECK(strcmp(ps4u_bgft_symbols[PS4U_BGFT_SYMBOL_UNREGISTER],
                 "sceBgftServiceIntDownloadUnregisterTask") == 0);

    for (i = 0; i < PS4U_BGFT_SYMBOL_COUNT; i++) {
        CHECK(ps4u_bgft_symbols[i] != NULL);
        CHECK(strncmp(ps4u_bgft_symbols[i], "sceBgftServiceInt", 17) == 0);
        for (k = i + 1; k < PS4U_BGFT_SYMBOL_COUNT; k++) {
            CHECK(strcmp(ps4u_bgft_symbols[i], ps4u_bgft_symbols[k]) != 0);
        }
    }
}

static void test_diag_json(void)
{
    ps4u_diag_symbol symbols[PS4U_BGFT_SYMBOL_COUNT] = {
        {"sceBgftServiceIntInit", 0},
        {"sceBgftServiceIntDownloadStartTask", -1},
    };
    ps4u_diag diag;
    char json[1024];
    char small[64];
    size_t i;
    size_t needed;

    memset(&diag, 0, sizeof(diag));
    diag.version = "0.6.0";
    diag.dir_exists = 1;
    diag.free_bytes = 123456789012ull;
    diag.used_bytes = 42;
    diag.quota = 4294967296ull;
    diag.write_test.write_ok = 1;
    diag.write_test.delete_ok = 1;
    diag.pin_file = 1;
    diag.pin_valid = 1;
    diag.app_inst_ok = 1;
    diag.bgft_loaded = 1;
    diag.bgft_path = "libSceBgft.sprx";
    diag.bgft_result = 12;
    diag.symbols = symbols;
    diag.symbol_count = 2;

    needed = ps4u_diag_json(&diag, json, sizeof(json));
    CHECK(needed == strlen(json));
    CHECK(strstr(json, "\"ok\":true") != NULL);
    CHECK(strstr(json, "\"version\":\"0.6.0\"") != NULL);
    CHECK(strstr(json, "\"dir\":{\"exists\":true,\"free_bytes\":123456789012,"
                       "\"used_bytes\":42,\"quota\":4294967296}") != NULL);
    CHECK(strstr(json, "\"write_test\":{\"write\":true,\"delete\":true,"
                       "\"error\":null}") != NULL);
    CHECK(strstr(json, "\"pin\":{\"file\":true,\"valid\":true}") != NULL);
    CHECK(strstr(json, "\"app_inst\":{\"ok\":true,\"result\":0}") != NULL);
    CHECK(strstr(json, "\"bgft\":{\"loaded\":true,\"path\":\"libSceBgft.sprx\","
                       "\"result\":12,\"symbols\":[") != NULL);
    CHECK(strstr(json, "{\"name\":\"sceBgftServiceIntInit\",\"result\":0,"
                       "\"found\":true}") != NULL);
    CHECK(strstr(json, "{\"name\":\"sceBgftServiceIntDownloadStartTask\","
                       "\"result\":-1,\"found\":false}") != NULL);
    CHECK(strchr(json, '\n') == NULL);

    /* Alles schief: Datei nicht schreibbar, kein PIN, AppInstUtil und BGFT
       mit Fehlercode, Modul nicht geladen. */
    memset(&diag, 0, sizeof(diag));
    diag.version = "0.6.0";
    diag.write_test.error = "open";
    diag.app_inst_result = -404;
    diag.bgft_result = -2143021056;
    diag.symbols = symbols;
    diag.symbol_count = 1;
    ps4u_diag_json(&diag, json, sizeof(json));
    CHECK(strstr(json, "\"dir\":{\"exists\":false,\"free_bytes\":0,"
                       "\"used_bytes\":0,\"quota\":0}") != NULL);
    CHECK(strstr(json, "\"write\":false,\"delete\":false,\"error\":\"open\"") !=
          NULL);
    CHECK(strstr(json, "\"pin\":{\"file\":false,\"valid\":false}") != NULL);
    CHECK(strstr(json, "\"app_inst\":{\"ok\":false,\"result\":-404}") != NULL);
    CHECK(strstr(json, "\"bgft\":{\"loaded\":false,\"path\":null,"
                       "\"result\":-2143021056,") != NULL);

    /* Fehlertext wird maskiert, Nullfelder bleiben null. */
    diag.write_test.error = "a\"b\n";
    ps4u_diag_json(&diag, json, sizeof(json));
    CHECK(strstr(json, "\"error\":\"a\\\"b\\n\"") != NULL);

    /* Zu kleiner Puffer: volle Laenge, aber sauber terminiert. */
    needed = ps4u_diag_json(&diag, small, sizeof(small));
    CHECK(needed > sizeof(small) - 1);
    CHECK(strlen(small) == sizeof(small) - 1);
    CHECK(ps4u_diag_json(NULL, json, sizeof(json)) == 0);

    /* Groesster sinnvoller Fall muss in den Payload-Puffer passen. */
    memset(&diag, 0, sizeof(diag));
    diag.version = "0.6.2-\"lang\"";
    diag.free_bytes = ~(unsigned long long)0;
    diag.used_bytes = ~(unsigned long long)0;
    diag.quota = ~(unsigned long long)0;
    diag.write_test.write_ok = 1;
    diag.write_test.delete_ok = 1;
    diag.write_test.error = "remove";
    diag.pin_file = 1;
    diag.pin_valid = 1;
    diag.app_inst_result = -2147483647 - 1;
    diag.bgft_loaded = 1;
    diag.bgft_path = "/system/common/lib/libSceBgft.sprx";
    diag.bgft_result = 2147483647;
    diag.symbols = symbols;
    diag.symbol_count = PS4U_BGFT_SYMBOL_COUNT;
    for (i = 0; i < PS4U_BGFT_SYMBOL_COUNT; i++) {
        symbols[i].name = ps4u_bgft_symbols[i];
        symbols[i].result = -2147483647 - 1;
    }
    needed = ps4u_diag_json(&diag, json, sizeof(json));
    CHECK(needed < PS4U_DIAG_BODY_MAX);

    /* Ohne Symbolliste darf der Builder nicht dereferenzieren. */
    memset(&diag, 0, sizeof(diag));
    diag.version = "0.6.2";
    diag.symbol_count = 3;
    ps4u_diag_json(&diag, json, sizeof(json));
    CHECK(strstr(json, "\"symbols\":[]") != NULL);
}

/* Schreib-/Loeschtest gegen ein echtes Verzeichnis und gegen einen fehlenden
   Ordner. Der fehlende Ordner ist der Fall, in dem fruehere Fassungen den
   Fehler mit "remove" ueberschrieben haben. */
static void test_diag_write_test(void)
{
    ps4u_diag_write_result result;
    FILE *file;

    ps4u_diag_write_test(".", &result);
    CHECK(result.write_ok == 1);
    CHECK(result.delete_ok == 1);
    CHECK(result.error == NULL);
    file = fopen(".diagnostics.tmp", "rb");
    CHECK(file == NULL);
    if (file != NULL) {
        fclose(file);
    }

    ps4u_diag_write_test("ps4u-diag-missing-dir", &result);
    CHECK(result.write_ok == 0);
    CHECK(result.delete_ok == 0);
    CHECK(result.error != NULL && strcmp(result.error, "open") == 0);

    ps4u_diag_write_test(NULL, &result);
    CHECK(result.write_ok == 0);
    CHECK(result.error != NULL && strcmp(result.error, "path") == 0);

    ps4u_diag_write_test(".", NULL); /* darf nicht abstuerzen */
}

int main(void)
{
    test_request_line();
    test_request_block();
    test_files();
    test_resume_plan();
    test_bgft_symbols();
    test_diag_json();
    test_diag_write_test();
    test_upload_space();
    test_headers();
    test_pkg_names();
    test_routes();
    test_responses();
    test_resume();
    test_status_json();

    printf("%d Checks, %d Fehler\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
