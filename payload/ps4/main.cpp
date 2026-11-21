/* PS4Upload Payload - PS4-Pendant zu PS5Upload, dritter Funktionsstand.
 *
 * Laeuft als ELF-Payload (GoldHEN-BinLoader 9090) im Kernel-Kontext und
 * liefert auf Port 9180 einen HTTP-Dienst:
 *
 *   GET    /status                    JSON (Phase, Prozent, Uptime, Platz)
 *   POST   /upload?name=...           PKG-Body nach /data/ps4upload/<name>
 *   POST   /upload?name=...&offset=N  an eine .part-Datei anhaengen (Resume)
 *   HEAD   /upload?name=...           bereits empfangene Bytes (X-Upload-Offset)
 *   DELETE /upload?name=...           Datei und .part-Rest entfernen
 *   GET    /files                     Dateien mit Groesse, Quote, freier Platz
 *   GET    /diagnostics               PIN-geschuetzte Selbstauskunft (Platz,
 *                                     Schreibtest, PIN, AppInstUtil, BGFT)
 *   GET    /                          kurzer Text
 *
 * Eine Quote kommt aus /data/ps4upload/config.txt ("quota_bytes=N", 0 oder
 * fehlend = unbegrenzt); ohne Quote ist der Platz die einzige Grenze.
 *
 * Jeder Upload traegt den Header X-Upload-Pin. Vor dem Umbenennen auf den
 * Zielnamen prueft pkg.h aus ps4app/ die PKG (Signatur, Groesse, param.sfo,
 * Content-Type); ungueltige Pakete werden geloescht. Bewusst klein: ein
 * Client zur Zeit, feste Puffer, kein BGFT (Installation ist der naechste
 * Schritt).
 */

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* Die Payload-SDK liefert nur die FreeBSD-libc-Header, keine orbis-Header.
   Fuer /diagnostics werden drei Systemfunktionen gebraucht; ihre Prototypen
   stehen deshalb hier (dieselben Aufrufe, die ps4app/install_bgft.h macht). */
extern "C" {
int sceAppInstUtilInitialize(void);
uint32_t sceKernelLoadStartModule(const char *path, size_t args,
                                  const void *argp, uint32_t flags,
                                  const void *opt, uint32_t *pRes);
int sceKernelDlsym(int32_t handle, const char *symbol, void **address);
}

#include "bgft_symbols.h"
#include "pkg.h"
#include "pin.h"
#include "ps4u_diag.h"
#include "ps4u_files.h"
#include "ps4u_http.h"
#include "ps4u_status.h"
#include "upload.h"

#define PAYLOAD_VERSION "0.6.2"
#define LISTEN_PORT 9180
#define RESPONSE_MAX 1024
/* Obergrenze fuer GET /files: mehr Eintraege passen nicht sinnvoll in eine
   Antwort, und die Quote zaehlt ohnehin alle Dateien im Verzeichnis. */
#define FILES_MAX 64
#define FILES_BODY_MAX 4096
#define BACKLOG 4

static const char UPLOAD_DIR[] = "/data/ps4upload";
static const char CONFIG_PATH[] = "/data/ps4upload/config.txt";
static const char PIN_PATH[] = "/data/ps4upload/pin.txt";
static const char LOG_PATH[] = "/data/ps4upload/payload.log";
static const char HEARTBEAT_PATH[] = "/data/ps4upload/heartbeat";
static const char ROOT_BODY[] =
    "PS4Upload Payload " PAYLOAD_VERSION "\n"
    "GET /status\n"
    "POST /upload?name=datei.pkg[&offset=N]\n"
    "HEAD /upload?name=datei.pkg\n"
    "DELETE /upload?name=datei.pkg\n"
    "GET /files\n"
    "GET /diagnostics        (Header X-Upload-Pin)\n";

static PinState g_pin;
static PinClientState g_clients[PIN_CLIENT_CAPACITY];
static unsigned g_uploading;
static int g_percent;
/* Quote in Bytes aus config.txt; 0 heisst unbegrenzt. */
static unsigned long long g_quota;
static time_t g_started;

/* ---------------------------------------------------------------- Diagnose */

/* Ohne TV ist eine Datei die einzige Spur: sie ueberlebt einen Absturz und
   ist per FTP lesbar. Bewusst klein und ohne Rotation. */
static void plog(const char *line)
{
    FILE *file = fopen(LOG_PATH, "a");

    if (file == NULL) {
        return;
    }
    fprintf(file, "%ld %s\n", (long)(time(NULL) - g_started), line);
    fclose(file);
}

static void log_start(void)
{
    FILE *file = fopen(LOG_PATH, "w");

    if (file != NULL) {
        fprintf(file, "0 payload %s start\n", PAYLOAD_VERSION);
        fclose(file);
    }
}

/* Getrennt vom Log: eine Zeile, die ein Client ohne Netzzugriff pruefen kann. */
static void heartbeat(void)
{
    FILE *file = fopen(HEARTBEAT_PATH, "w");

    if (file == NULL) {
        return;
    }
    fprintf(file, "{\"version\":\"%s\",\"uptime_s\":%ld}\n", PAYLOAD_VERSION,
            (long)(time(NULL) - g_started));
    fclose(file);
}

/* ------------------------------------------------------------------ Antworten */

static int send_all(int fd, const char *data, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        ssize_t written = send(fd, data + sent, len - sent, 0);

        if (written <= 0) {
            return -1;
        }
        sent += (size_t)written;
    }
    return 0;
}

/* Statuszeile und Headerblock kommen aus ps4port/core und sind dort
   host-getestet; hier bleibt nur das Senden. */
static void send_http_extra(int fd, int status, const char *content_type,
                            const char *extra, const char *body, size_t body_len)
{
    char head[320];
    size_t head_len = ps4u_response_head_extra(status, content_type,
                                               (unsigned long long)body_len,
                                               extra, head, sizeof(head));

    if (head_len == 0 || head_len >= sizeof(head)) {
        return;
    }
    if (send_all(fd, head, head_len) != 0) {
        return;
    }
    if (body_len > 0) {
        send_all(fd, body, body_len);
    }
}

static void send_http(int fd, int status, const char *content_type,
                      const char *body, size_t body_len)
{
    send_http_extra(fd, status, content_type, NULL, body, body_len);
}

static void send_json(int fd, int status, const char *json)
{
    send_http(fd, status, "application/json; charset=utf-8", json, strlen(json));
}

static void send_json_extra(int fd, int status, const char *extra, const char *json)
{
    send_http_extra(fd, status, "application/json; charset=utf-8", extra, json,
                    strlen(json));
}

static void send_error(int fd, int status)
{
    char response[256];
    size_t len = ps4u_error_response(status, response, sizeof(response));

    send_all(fd, response, len);
}

static unsigned long long free_bytes_at(const char *path)
{
    struct statvfs info;

    if (statvfs(path, &info) != 0) {
        return 0;
    }
    return (unsigned long long)info.f_bavail * (unsigned long long)info.f_frsize;
}

/* Quote aus config.txt, nur beim Start gelesen: eine Zeile "quota_bytes=N",
   '#' ist Kommentar. Fehlt die Datei oder der Schluessel, gilt keine Quote. */
static void config_load(void)
{
    char buffer[512];
    size_t read_len = 0;
    FILE *file = fopen(CONFIG_PATH, "rb");

    g_quota = 0;
    if (file == NULL) {
        return;
    }
    read_len = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[read_len] = '\0';
    g_quota = ps4u_config_quota(buffer);
    if (g_quota > 0) {
        plog("main: Quote aus config.txt uebernommen");
    }
}

/* Summiert die Bytes aller Pakete im Upload-Verzeichnis. Mit entries != NULL
   werden die ersten cap zusaetzlich fuer GET /files gemerkt; die Namen leben
   in einem statischen Puffer, weil der Verzeichnis-Handle nur Zeiger liefert.
   Die Quote zaehlt auch Dateien jenseits von FILES_MAX, deshalb die Summe
   ueber alle Treffer und nicht nur ueber die gemerkten. */
static unsigned long long scan_files(ps4u_file_entry *entries, size_t cap,
                                     size_t *count)
{
    static char names[FILES_MAX][128];
    unsigned long long used = 0;
    size_t found = 0;
    struct dirent *item;
    struct stat info;
    char path[256];
    DIR *dir;

    if (count != NULL) {
        *count = 0;
    }
    if (entries != NULL && cap > FILES_MAX) {
        cap = FILES_MAX;
    }

    dir = opendir(UPLOAD_DIR);
    if (dir == NULL) {
        return 0;
    }

    while ((item = readdir(dir)) != NULL) {
        if (!ps4u_listable_file(item->d_name)) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", UPLOAD_DIR, item->d_name);
        if (stat(path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0) {
            continue;
        }
        used += (unsigned long long)info.st_size;
        if (entries != NULL && found < cap) {
            snprintf(names[found], sizeof(names[found]), "%s", item->d_name);
            entries[found].name = names[found];
            entries[found].bytes = (unsigned long long)info.st_size;
            found++;
        }
    }
    closedir(dir);

    if (count != NULL) {
        *count = found;
    }
    return used;
}

/* GET /files: Name und Groesse je Paket, dazu Quote und freier Platz. */
static void handle_files(int fd)
{
    static ps4u_file_entry entries[FILES_MAX];
    char body[FILES_BODY_MAX];
    size_t count = 0;
    unsigned long long used = scan_files(entries, FILES_MAX, &count);
    size_t len = ps4u_files_json(entries, count, used, g_quota,
                                 free_bytes_at(UPLOAD_DIR), body, sizeof(body));

    if (len >= sizeof(body)) {
        plog("files: Liste zu lang fuer den Puffer");
        send_json(fd, 500, "{\"ok\":false,\"error\":\"files\"}");
        return;
    }
    send_http(fd, 200, "application/json; charset=utf-8", body, len);
}

static void send_status(int fd)
{
    char json[RESPONSE_MAX];
    ps4u_status status;
    size_t len;

    memset(&status, 0, sizeof(status));
    status.version = PAYLOAD_VERSION;
    status.phase = g_uploading ? "upload" : "idle";
    status.uptime_s = (unsigned)(time(NULL) - g_started);
    status.clients = 1;
    status.queue = 0;
    status.percent = g_percent;
    status.free_bytes = free_bytes_at(UPLOAD_DIR);

    len = ps4u_status_json(&status, json, sizeof(json));
    if (len >= sizeof(json)) {
        len = sizeof(json) - 1;
    }
    send_http(fd, 200, "application/json; charset=utf-8", json, len);
}

/* ----------------------------------------------------------------------- PIN */

static void pin_load_or_create(void)
{
    char buffer[64];
    char parsed[PIN_DIGITS + 1];
    size_t read_len = 0;
    FILE *file = fopen(PIN_PATH, "rb");

    if (file != NULL) {
        read_len = fread(buffer, 1, sizeof(buffer) - 1, file);
        fclose(file);
    }
    buffer[read_len] = '\0';

    if (read_len > 0 && pin_parse_value(buffer, parsed, sizeof(parsed))) {
        memcpy(g_pin.value, parsed, sizeof(parsed));
        return;
    }

    /* Zeit plus Adresse: keine Krypto, die Sperre ist die eigentliche Bremse. */
    pin_generate(g_pin, (unsigned int)time(NULL) ^ (unsigned int)(size_t)&g_pin);

    file = fopen(PIN_PATH, "wb");
    if (file != NULL) {
        fwrite(g_pin.value, 1, PIN_DIGITS, file);
        fputc('\n', file);
        fclose(file);
    }
}

/* Prueft den PIN-Header samt Sperre; head ist der ganze Anfragepuffer, aus dem
   ps4u_header_value den Header liest. 0 = in Ordnung, sonst HTTP-Status; error
   zeigt dabei auf den passenden JSON-Fehlertext. */
static int pin_gate(const char *head, const char *client,
                    unsigned long long now_ms, PinClientState **state,
                    const char **error)
{
    static const char denied[] = "{\"ok\":false,\"error\":\"pin\"}";
    static const char locked[] = "{\"ok\":false,\"error\":\"locked\"}";
    char pin_value[32];
    char given[PIN_DIGITS + 1];
    PinClientState *found = pin_client_state(g_clients, PIN_CLIENT_CAPACITY, client);

    if (found == NULL) {
        *error = "{\"ok\":false,\"error\":\"clients\"}";
        return 429;
    }
    if (pin_locked(*found, now_ms)) {
        *error = locked;
        return 403;
    }
    if (ps4u_header_value(head, PIN_HEADER_NAME, pin_value, sizeof(pin_value)) != 0 ||
        !pin_parse_value(pin_value, given, sizeof(given)) ||
        !pin_equal(g_pin.value, given)) {
        pin_fail(*found, now_ms);
        *error = denied;
        return 403;
    }

    *state = found;
    *error = NULL;
    return 0;
}

/* --------------------------------------------------------------------- Upload */

static void part_path(char *out, size_t cap, const char *name)
{
    snprintf(out, cap, "%s/%s.part", UPLOAD_DIR, name);
}

static void dest_path(char *out, size_t cap, const char *name)
{
    snprintf(out, cap, "%s/%s", UPLOAD_DIR, name);
}

/* Groesse einer Datei; 0, wenn sie fehlt oder leer ist. */
static unsigned long long file_bytes(const char *path)
{
    struct stat info;

    if (stat(path, &info) != 0 || info.st_size <= 0) {
        return 0;
    }
    return (unsigned long long)info.st_size;
}

/* Merkt sich die ersten vier Bytes, damit ein falscher Body nicht erst nach
   Gigabytes auffaellt. */
static void magic_feed(unsigned char *magic, size_t *len, const char *data,
                       size_t count)
{
    while (*len < 4 && count > 0) {
        magic[(*len)++] = (unsigned char)*data++;
        count--;
    }
}

static int magic_is_pkg(const unsigned char *magic, size_t len)
{
    return len == 4 && magic[0] == 0x7f && magic[1] == 'C' && magic[2] == 'N' &&
           magic[3] == 'T';
}

static void handle_upload(int fd, const char *head, const char *name,
                          const char *client, const char *body, size_t body_len,
                          unsigned long long offset)
{
    static const char nospace[] = "{\"ok\":false,\"error\":\"no_space\"}";
    static const char partial[] = "{\"ok\":false,\"error\":\"incomplete\"}";
    static const char not_pkg[] = "{\"ok\":false,\"error\":\"not_pkg\"}";
    char dest[256];
    char part[256];
    char answer[512];
    char extra[64];
    char line[176];
    unsigned char magic[4] = {0};
    size_t magic_len = 0;
    unsigned long long length = 0;
    unsigned long long total;
    unsigned long long written = 0;
    unsigned long long on_disk = 0;
    unsigned long long now_ms = (unsigned long long)time(NULL) * 1000ull;
    unsigned long long have = 0;
    unsigned long long done = 0;
    ps4u_resume_plan plan;
    const char *error = NULL;
    PinClientState *state = NULL;
    PkgInfo pkg;
    FILE *out;
    char chunk[16384];
    int bad_magic = 0;
    /* Nur ein frischer Upload beginnt mit dem PKG-Kopf; beim Anhaengen waeren
       die ersten vier Bytes des Rests kein "7f434e54". */
    int watch_magic = offset == 0;
    int status;

    if (ps4u_safe_pkg_name(name) != 0) {
        send_json(fd, 400, "{\"ok\":false,\"error\":\"name\"}");
        return;
    }
    if (ps4u_content_length(head, &length) != 0) {
        send_json(fd, 411, "{\"ok\":false,\"error\":\"content_length\"}");
        return;
    }
    /* Laenge 0 ist nur beim Anhaengen erlaubt: dann fehlt nichts mehr, es bleibt
       nur noch Pruefen und Umbenennen (z. B. nach einem Abbruch vor der Antwort). */
    if (length == 0 && offset == 0) {
        send_json(fd, 411, "{\"ok\":false,\"error\":\"content_length\"}");
        return;
    }

    status = pin_gate(head, client, now_ms, &state, &error);
    if (status != 0) {
        send_json(fd, status, error);
        return;
    }

    part_path(part, sizeof(part), name);
    dest_path(dest, sizeof(dest), name);

    /* Ein Offset ungleich 0 setzt einen passenden .part-Rest voraus. Liegt
       die fertige Datei schon mit genau offset Bytes vor, ist ein erneutes
       Abschliessen (Content-Length 0) kein Fehler: die Antwort von vorher
       ging verloren, der Upload ist durch. */
    have = file_bytes(part);
    done = file_bytes(dest);
    plan = ps4u_resume_plan_for(offset, length, have, done);
    if (plan == PS4U_RESUME_ALREADY) {
        snprintf(answer, sizeof(answer),
                 "{\"ok\":true,\"name\":\"%s\",\"bytes\":%llu,\"already\":true}",
                 name, done);
        snprintf(line, sizeof(line), "upload: bereits fertig %s", name);
        plog(line);
        send_json(fd, 200, answer);
        return;
    }
    if (plan == PS4U_RESUME_CONFLICT) {
        snprintf(answer, sizeof(answer),
                 "{\"ok\":false,\"error\":\"offset\",\"offset\":%llu}", have);
        send_json(fd, 409, answer);
        return;
    }
    /* Bei offset 0 wird die .part-Datei abgeschnitten; ihre alten Bytes sind
       dann wieder frei. */
    if (offset == 0) {
        have = 0;
    }
    total = offset + length;

    /* Quote aus config.txt: der .part-Rest zaehlt schon zu used, deshalb ist
       length und nicht total der Zuwachs. */
    if (g_quota > 0) {
        unsigned long long used = scan_files(NULL, 0, NULL);

        if (!ps4u_quota_ok(used, length, g_quota)) {
            snprintf(answer, sizeof(answer),
                     "{\"ok\":false,\"error\":\"quota\",\"used\":%llu,"
                     "\"quota\":%llu}",
                     used, g_quota);
            send_json(fd, 507, answer);
            return;
        }
    }

    /* BGFT legt beim Installieren eine zweite Kopie an. Ohne Body wird nichts
       geschrieben, dann darf eine volle Platte das Umbenennen nicht blockieren.
       Beim Resume liegen die Bytes der .part-Datei schon auf der Platte und
       fehlen in free_bytes; upload_bgft_space_ok_resume rechnet sie dazu. */
    if (length > 0 &&
        !upload_bgft_space_ok_resume(total, free_bytes_at(UPLOAD_DIR), have,
                                     UPLOAD_SPACE_RESERVE_BYTES)) {
        send_json(fd, 507, nospace);
        return;
    }

    out = fopen(part, offset > 0 ? "ab" : "wb");
    if (out == NULL) {
        plog("upload: .part nicht schreibbar");
        send_json(fd, 500, "{\"ok\":false,\"error\":\"open\"}");
        return;
    }

    pin_succeeded(*state);
    g_uploading = 1;
    g_percent = total > 0 ? (int)((offset * 100ull) / total) : 0;

    if (body_len > 0) {
        size_t take = body_len;
        size_t done;

        if (take > length) {
            take = (size_t)length;
        }
        /* fwrite kann weniger schreiben als verlangt; nur das Gezählte zählt. */
        done = fwrite(body, 1, take, out);
        if (watch_magic) {
            magic_feed(magic, &magic_len, body, done);
        }
        written = (unsigned long long)done;
    }

    while (written < length) {
        ssize_t got = recv(fd, chunk, sizeof(chunk), 0);
        size_t done;

        if (got <= 0) {
            break;
        }
        done = fwrite(chunk, 1, (size_t)got, out);
        if (watch_magic) {
            magic_feed(magic, &magic_len, chunk, done);
        }
        written += (unsigned long long)done;
        g_percent = (int)(((offset + written) * 100ull) / total);
        if (done != (size_t)got) {
            break;
        }
        if (watch_magic && magic_len == 4 && !magic_is_pkg(magic, magic_len)) {
            bad_magic = 1;
            break;
        }
    }

    fclose(out);
    g_uploading = 0;
    g_percent = 0;

    /* Die Datei auf dem Datentraeger ist die Wahrheit: fwrite kann mitten im
       Puffer abbrechen, und ein Pufferrest landet erst beim fclose. Nur so
       passt der gemeldete Offset zum naechsten HEAD desselben Namens. */
    on_disk = file_bytes(part);
    if (on_disk == 0) {
        on_disk = offset + written;
    }

    if (bad_magic) {
        remove(part);
        plog("upload: keine PKG-Signatur, abgebrochen");
        send_json(fd, 400, not_pkg);
        return;
    }

    /* Abbruch: der .part-Rest bleibt liegen, damit derselbe Offset weitergeht.
       on_disk != total faengt auch einen fehlgeschlagenen fclose-Puffer ab
       (z. B. Platte voll), damit pkg_probe den Rest nicht wegwirft. */
    if (written != length || on_disk != total) {
        char offset_header[64];

        snprintf(offset_header, sizeof(offset_header), "X-Upload-Offset: %llu\r\n",
                 on_disk);
        snprintf(line, sizeof(line),
                 "upload: unvollstaendig bei %llu Bytes, Rest bleibt fuer Resume",
                 on_disk);
        plog(line);
        send_json_extra(fd, 400, offset_header, partial);
        return;
    }

    if (!pkg_probe(part, &pkg)) {
        char detail[80];
        size_t i;

        for (i = 0; pkg.error[i] != '\0' && i + 1 < sizeof(detail); i++) {
            detail[i] = pkg.error[i];
        }
        detail[i] = '\0';
        remove(part);
        snprintf(answer, sizeof(answer),
                 "{\"ok\":false,\"error\":\"pkg\",\"detail\":\"%s\"}", detail);
        plog("upload: PKG-Pruefung fehlgeschlagen");
        send_json(fd, 400, answer);
        return;
    }

    if (rename(part, dest) != 0) {
        plog("upload: Umbenennen fehlgeschlagen");
        send_json(fd, 500, "{\"ok\":false,\"error\":\"rename\"}");
        return;
    }

    snprintf(answer, sizeof(answer),
             "{\"ok\":true,\"name\":\"%s\",\"bytes\":%llu,\"title_id\":\"%s\","
             "\"content_id\":\"%s\"}",
             name, total, pkg.title_id, pkg.content_id);
    snprintf(line, sizeof(line), "upload: fertig %s (%llu Bytes)", name, total);
    plog(line);
    heartbeat();
    send_json(fd, 200, answer);
}

/* BGFT einmal pro Prozess laden: jeder sceKernelLoadStartModule-Aufruf erhoeht
   den Referenzzaehler des Moduls, /diagnostics darf das nicht bei jedem Abruf
   tun. Scheitert der Versuch, bleibt das Ergebnis stehen (es aendert sich
   ohne Neustart nicht). */
static int32_t g_bgft_handle;
static int32_t g_bgft_load_result;
static const char *g_bgft_path;
static int g_bgft_tried;

static void diagnostics_bgft_load(void)
{
    static const char *const candidates[] = {
        "/system/common/lib/libSceBgft.sprx",
        "libSceBgft.sprx",
    };
    size_t i;

    if (g_bgft_tried) {
        return;
    }
    g_bgft_tried = 1;
    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        int32_t handle = (int32_t)sceKernelLoadStartModule(candidates[i], 0, 0,
                                                          0, 0, 0);

        g_bgft_load_result = handle;
        if (handle < 0) {
            continue;
        }
        g_bgft_handle = handle;
        g_bgft_path = candidates[i];
        return;
    }
}

/* GET /diagnostics: Momentaufnahme fuer die Fehlersuche auf einer Konsole
   ohne Shell. Alles wird beim Abruf gemessen (Platz, Quote, Schreib-/Loesch-
   test, PIN-Datei, AppInstUtil, BGFT-Modul und jedes dlsym-Ergebnis); das
   JSON und der Schreibtest liegen in ps4port/core und sind dort host-getestet. */
static void handle_diagnostics(int fd, const char *head, const char *client)
{
    ps4u_diag_symbol symbols[PS4U_BGFT_SYMBOL_COUNT];
    ps4u_diag diag;
    char body[PS4U_DIAG_BODY_MAX];
    char pin_buffer[64];
    char parsed_pin[PIN_DIGITS + 1];
    char line[176];
    const char *error = NULL;
    PinClientState *state = NULL;
    unsigned long long now_ms = (unsigned long long)time(NULL) * 1000ull;
    struct stat info;
    size_t i;
    size_t len;
    int status;

    status = pin_gate(head, client, now_ms, &state, &error);
    if (status != 0) {
        send_json(fd, status, error);
        return;
    }

    memset(&diag, 0, sizeof(diag));
    memset(symbols, 0, sizeof(symbols));
    diag.version = PAYLOAD_VERSION;

    /* Dieselben Quellen wie /files und /status. */
    diag.dir_exists = stat(UPLOAD_DIR, &info) == 0 && S_ISDIR(info.st_mode);
    diag.free_bytes = free_bytes_at(UPLOAD_DIR);
    diag.used_bytes = scan_files(NULL, 0, NULL);
    diag.quota = g_quota;

    ps4u_diag_write_test(UPLOAD_DIR, &diag.write_test);

    /* PIN-Datei: existiert sie, und steht ein brauchbarer PIN darin? */
    {
        FILE *file = fopen(PIN_PATH, "rb");

        if (file != NULL) {
            size_t got = fread(pin_buffer, 1, sizeof(pin_buffer) - 1, file);

            fclose(file);
            pin_buffer[got] = '\0';
            diag.pin_file = got > 0;
            diag.pin_valid = diag.pin_file &&
                             pin_parse_value(pin_buffer, parsed_pin,
                                             sizeof(parsed_pin));
        }
    }

    diag.app_inst_result = (int32_t)sceAppInstUtilInitialize();
    diag.app_inst_ok = diag.app_inst_result == 0;

    /* BGFT wie install_bgft_open: erst der Systempfad, dann der Soname-Stub.
       Jedes dlsym-Ergebnis wird einzeln gemeldet, damit ein fehlendes Symbol
       nicht als Ladefehler durchgeht. */
    diag.symbols = symbols;
    diag.symbol_count = PS4U_BGFT_SYMBOL_COUNT;
    for (i = 0; i < PS4U_BGFT_SYMBOL_COUNT; i++) {
        symbols[i].name = ps4u_bgft_symbols[i];
        symbols[i].result = -1; /* nicht abgefragt */
    }
    diagnostics_bgft_load();
    diag.bgft_loaded = g_bgft_handle > 0;
    diag.bgft_path = g_bgft_path;
    diag.bgft_result = g_bgft_load_result;
    if (diag.bgft_loaded) {
        for (i = 0; i < PS4U_BGFT_SYMBOL_COUNT; i++) {
            void *address = NULL;

            symbols[i].result =
                (int32_t)sceKernelDlsym(g_bgft_handle, ps4u_bgft_symbols[i],
                                        &address);
        }
    }

    len = ps4u_diag_json(&diag, body, sizeof(body));
    if (len >= sizeof(body)) {
        plog("diagnostics: JSON zu lang fuer den Puffer");
        send_json(fd, 500, "{\"ok\":false,\"error\":\"diag\"}");
        return;
    }
    snprintf(line, sizeof(line),
             "diagnostics: write=%d delete=%d appinst=%d bgft=%d",
             diag.write_test.write_ok, diag.write_test.delete_ok,
             (int)diag.app_inst_result, (int)diag.bgft_result);
    plog(line);
    send_http(fd, 200, "application/json; charset=utf-8", body, len);
}

/* Offen fuer Resume: wie viele Bytes liegen schon vor? Bei einem fertigen
   Paket steht seine Groesse im Offset und X-Upload-Complete sagt, dass nichts
   mehr fehlt. */
static void handle_probe(int fd, const char *name)
{
    char dest[256];
    char part[256];
    char extra[96];
    unsigned long long offset;
    int complete = 0;

    if (ps4u_safe_pkg_name(name) != 0) {
        send_json(fd, 400, "{\"ok\":false,\"error\":\"name\"}");
        return;
    }

    part_path(part, sizeof(part), name);
    dest_path(dest, sizeof(dest), name);
    offset = ps4u_resume_offset(file_bytes(part), file_bytes(dest), &complete);
    if (complete) {
        snprintf(extra, sizeof(extra),
                 "X-Upload-Offset: %llu\r\nX-Upload-Complete: 1\r\n", offset);
    } else {
        snprintf(extra, sizeof(extra), "X-Upload-Offset: %llu\r\n", offset);
    }
    send_http_extra(fd, 200, "text/plain; charset=utf-8", extra, NULL, 0);
}

static void handle_delete(int fd, const char *head, const char *name,
                          const char *client)
{
    char dest[256];
    char part[256];
    char answer[320];
    char line[176];
    const char *error = NULL;
    PinClientState *state = NULL;
    unsigned long long now_ms = (unsigned long long)time(NULL) * 1000ull;
    int deleted;
    int status;

    if (ps4u_safe_pkg_name(name) != 0) {
        send_json(fd, 400, "{\"ok\":false,\"error\":\"name\"}");
        return;
    }

    status = pin_gate(head, client, now_ms, &state, &error);
    if (status != 0) {
        send_json(fd, status, error);
        return;
    }

    dest_path(dest, sizeof(dest), name);
    part_path(part, sizeof(part), name);
    deleted = remove(dest) == 0;
    remove(part);

    snprintf(answer, sizeof(answer),
             "{\"ok\":true,\"name\":\"%s\",\"deleted\":%s}", name,
             deleted ? "true" : "false");
    snprintf(line, sizeof(line), "upload: geloescht %s", name);
    plog(line);
    send_json(fd, 200, answer);
}

/* -------------------------------------------------------------------- Client */

static void handle_client(int fd, const char *client)
{
    char request[PS4U_HEAD_MAX];
    size_t used = 0;
    size_t header_len = 0;
    ps4u_request parsed;
    int code;

    while (used + 1 < sizeof(request)) {
        ssize_t got = recv(fd, request + used, sizeof(request) - 1 - used, 0);

        if (got <= 0) {
            break;
        }
        used += (size_t)got;
        request[used] = '\0';
        /* Der Kern sucht den Block: Header, die im selben Puffer liegen wie
           der Body, zaehlen nicht mit. */
        header_len = ps4u_header_block_length(request, used);
        if (header_len != 0) {
            break;
        }
    }
    if (used == 0) {
        return;
    }
    if (header_len == 0) {
        send_error(fd, 400);
        return;
    }

    /* request ist NUL-terminiert, und ps4u_header_value endet an der
       Leerzeile des Blocks: PIN und Content-Length kommen direkt aus dem
       Puffer, der Body dahinter bleibt fuer den Upload liegen. */
    if (ps4u_parse_head(request, header_len, &parsed) != 0) {
        send_error(fd, 400);
        return;
    }

    code = ps4u_route_status(parsed.route, parsed.method);
    if (code == 405) {
        send_error(fd, 405);
        return;
    }
    if (code != 200) {
        send_error(fd, 404);
        return;
    }

    switch (parsed.route) {
    case PS4U_ROUTE_ROOT:
        send_http(fd, 200, "text/plain; charset=utf-8", ROOT_BODY,
                  sizeof(ROOT_BODY) - 1);
        break;
    case PS4U_ROUTE_STATUS:
        send_status(fd);
        break;
    case PS4U_ROUTE_FILES:
        handle_files(fd);
        break;
    case PS4U_ROUTE_DIAGNOSTICS:
        handle_diagnostics(fd, request, client);
        break;
    case PS4U_ROUTE_UPLOAD:
        if (parsed.method == PS4U_METHOD_HEAD) {
            handle_probe(fd, parsed.name);
        } else if (parsed.method == PS4U_METHOD_DELETE) {
            handle_delete(fd, request, parsed.name, client);
        } else {
            handle_upload(fd, request, parsed.name, client, request + header_len,
                          used - header_len, parsed.offset);
        }
        break;
    default:
        send_error(fd, 404);
        break;
    }
}

int main(void)
{
    struct sockaddr_in address;
    struct timeval idle_timeout;
    int server;
    int reuse = 1;
    int on = 1;

    g_started = time(NULL);
    mkdir(UPLOAD_DIR, 0777);
    log_start();
    plog("main: start");
    config_load();
    pin_load_or_create();
    heartbeat();

    idle_timeout.tv_sec = (long)(UPLOAD_IDLE_TIMEOUT_MS / 1000ull);
    idle_timeout.tv_usec = (long)((UPLOAD_IDLE_TIMEOUT_MS % 1000ull) * 1000ull);

    server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        plog("main: socket fehlgeschlagen");
        return 1;
    }
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(LISTEN_PORT);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server, (struct sockaddr *)&address, sizeof(address)) != 0) {
        plog("main: bind auf 9180 fehlgeschlagen");
        close(server);
        return 1;
    }
    if (listen(server, BACKLOG) != 0) {
        plog("main: listen fehlgeschlagen");
        close(server);
        return 1;
    }
    plog("main: hoert auf 9180");

    for (;;) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int client = accept(server, (struct sockaddr *)&peer, &peer_len);

        if (client < 0) {
            continue;
        }
        /* Ein Body, der so lange keine Bytes liefert, bricht ab. */
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &idle_timeout,
                   sizeof(idle_timeout));
        /* Ein Client, der frueh schliesst, darf kein SIGPIPE ausloesen. */
        setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
        handle_client(client, inet_ntoa(peer.sin_addr));
        close(client);
    }
}
