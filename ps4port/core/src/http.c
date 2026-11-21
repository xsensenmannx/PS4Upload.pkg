#include "ps4u_http.h"

#include <stdio.h>
#include <string.h>

/* Kein <ctype.h>: FreeBSD bildet tolower() auf die Thread-lokale Locale-Tabelle
   ab, und die fehlt im PS4-Payload-Link (__emutls_v._ThreadRuneLocale).
   Fuer HTTP-Header und .pkg-Endungen genuegt ASCII. */
static unsigned char ascii_lower(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c - 'A' + 'a') : c;
}

static int ascii_alnum(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z');
}

static int ascii_space(char c)
{
    return c == ' ' || c == '\t';
}

static int ci_equal_n(const char *a, const char *b, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (ascii_lower((unsigned char)a[i]) != ascii_lower((unsigned char)b[i])) {
            return 0;
        }
    }
    return 1;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int url_decode(const char *in, char *out, size_t cap)
{
    size_t len = 0;
    const char *p;

    if (in == NULL || out == NULL || cap == 0) {
        return -1;
    }

    for (p = in; *p != '\0';) {
        unsigned char c;

        if (*p == '%') {
            int hi;
            int lo;

            if (p[1] == '\0' || p[2] == '\0') {
                return -1;
            }
            hi = hex_value(p[1]);
            lo = hex_value(p[2]);
            if (hi < 0 || lo < 0) {
                return -1;
            }
            c = (unsigned char)((hi << 4) | lo);
            p += 3;
        } else if (*p == '+') {
            c = (unsigned char)' ';
            p++;
        } else {
            c = (unsigned char)*p;
            p++;
        }

        if (c < 0x20 || c == 0x7f) {
            return -1;
        }
        if (len + 1 >= cap) {
            return -1;
        }
        out[len++] = (char)c;
    }

    out[len] = '\0';
    return (int)len;
}

static int parse_request_line(const char *line, ps4u_request *out)
{
    const char *p;
    const char *method;
    size_t method_len;
    const char *target;
    size_t target_len;
    const char *query;
    size_t path_len;
    size_t i;

    if (line == NULL || out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    p = line;
    while (ascii_space(*p)) {
        p++;
    }
    method = p;
    while (*p != '\0' && !ascii_space(*p)) {
        p++;
    }
    method_len = (size_t)(p - method);
    if (method_len == 0) {
        return -1;
    }

    if (method_len == 3 && memcmp(method, "GET", 3) == 0) {
        out->method = PS4U_METHOD_GET;
    } else if (method_len == 4 && memcmp(method, "HEAD", 4) == 0) {
        out->method = PS4U_METHOD_HEAD;
    } else if (method_len == 4 && memcmp(method, "POST", 4) == 0) {
        out->method = PS4U_METHOD_POST;
    } else if (method_len == 6 && memcmp(method, "DELETE", 6) == 0) {
        out->method = PS4U_METHOD_DELETE;
    } else {
        out->method = PS4U_METHOD_OTHER;
    }

    while (ascii_space(*p)) {
        p++;
    }
    target = p;
    while (*p != '\0' && !ascii_space(*p)) {
        p++;
    }
    target_len = (size_t)(p - target);
    if (target_len == 0) {
        return -1;
    }

    /* Eine HTTP-Version gehört dazu; ihr Inhalt wird nicht ausgewertet. */
    while (ascii_space(*p)) {
        p++;
    }
    if (*p == '\0') {
        return -1;
    }

    query = memchr(target, '?', target_len);
    path_len = query != NULL ? (size_t)(query - target) : target_len;
    if (path_len == 0 || target[0] != '/') {
        return -1;
    }
    for (i = 0; i < path_len; i++) {
        unsigned char c = (unsigned char)target[i];

        if (c < 0x21 || c == 0x7f) {
            return -1;
        }
    }

    if (path_len == 1) {
        out->route = PS4U_ROUTE_ROOT;
    } else if (path_len == 7 && memcmp(target, "/status", 7) == 0) {
        out->route = PS4U_ROUTE_STATUS;
    } else if (path_len == 7 && memcmp(target, "/upload", 7) == 0) {
        out->route = PS4U_ROUTE_UPLOAD;
    } else if (path_len == 6 && memcmp(target, "/files", 6) == 0) {
        out->route = PS4U_ROUTE_FILES;
    } else if (path_len == 12 && memcmp(target, "/diagnostics", 12) == 0) {
        out->route = PS4U_ROUTE_DIAGNOSTICS;
    } else {
        out->route = PS4U_ROUTE_UNKNOWN;
    }

    if (out->route == PS4U_ROUTE_UPLOAD && query != NULL) {
        const char *cursor = query + 1;
        const char *end = target + target_len;

        while (cursor < end) {
            const char *amp = memchr(cursor, '&', (size_t)(end - cursor));
            const char *token_end = amp != NULL ? amp : end;
            const char *eq = memchr(cursor, '=', (size_t)(token_end - cursor));

            if (eq != NULL && (size_t)(eq - cursor) == 4 &&
                memcmp(cursor, "name", 4) == 0) {
                char raw[128];
                size_t raw_len = (size_t)(token_end - eq - 1);
                int decoded;

                if (raw_len >= sizeof(raw)) {
                    return -1;
                }
                memcpy(raw, eq + 1, raw_len);
                raw[raw_len] = '\0';
                decoded = url_decode(raw, out->name, sizeof(out->name));
                if (decoded <= 0) {
                    return -1;
                }
            } else if (eq != NULL && (size_t)(eq - cursor) == 6 &&
                       memcmp(cursor, "offset", 6) == 0) {
                const char *digit = eq + 1;
                unsigned long long value = 0;

                if (digit == token_end) {
                    return -1;
                }
                for (; digit < token_end; digit++) {
                    if (*digit < '0' || *digit > '9') {
                        return -1;
                    }
                    value = value * 10ull + (unsigned long long)(*digit - '0');
                    if (value > PS4U_UPLOAD_MAX_BYTES) {
                        return -1;
                    }
                }
                out->offset = value;
            }
            if (amp == NULL) {
                break;
            }
            cursor = amp + 1;
        }
    }

    return 0;
}

size_t ps4u_header_block_length(const char *data, size_t len)
{
    size_t i;

    if (data == NULL || len < 4) {
        return 0;
    }
    for (i = 0; i + 3 < len; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' &&
            data[i + 3] == '\n') {
            return i + 4;
        }
    }
    return 0;
}

int ps4u_parse_head(const char *data, size_t block_len, ps4u_request *out)
{
    char head[PS4U_HEAD_MAX];

    if (data == NULL || out == NULL || block_len < 4 ||
        block_len >= sizeof(head) ||
        memcmp(data + block_len - 4, "\r\n\r\n", 4) != 0) {
        return -1;
    }
    memcpy(head, data, block_len);
    head[block_len] = '\0';

    /* Der Block bleibt vollstaendig: die Anfragezeile endet am Leerzeichen
       vor "HTTP/1.x", die Header stehen fuer ps4u_header_value dahinter. */
    return parse_request_line(head, out);
}

int ps4u_content_length(const char *head, unsigned long long *out)
{
    char value[32];
    unsigned long long parsed = 0;
    const char *p;

    if (head == NULL || out == NULL) {
        return -1;
    }
    if (ps4u_header_value(head, "Content-Length", value, sizeof(value)) != 0) {
        return -1;
    }
    if (value[0] == '\0') {
        return -1;
    }
    for (p = value; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return -1;
        }
        parsed = parsed * 10ull + (unsigned long long)(*p - '0');
        if (parsed > PS4U_UPLOAD_MAX_BYTES) {
            return -1;
        }
    }
    *out = parsed;
    return 0;
}

int ps4u_header_value(const char *headers, const char *name, char *out, size_t cap)
{
    const char *p;
    size_t name_len;

    if (headers == NULL || name == NULL || out == NULL || cap == 0) {
        return -1;
    }
    name_len = strlen(name);

    p = headers;
    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        const char *line_end = eol != NULL ? eol : p + strlen(p);
        const char *colon;

        if (line_end > p && line_end[-1] == '\r') {
            line_end--;
        }
        if (line_end == p) {
            break;
        }

        colon = memchr(p, ':', (size_t)(line_end - p));
        if (colon != NULL && (size_t)(colon - p) == name_len &&
            ci_equal_n(p, name, name_len)) {
            const char *value = colon + 1;
            const char *value_end = line_end;
            size_t value_len;

            while (value < line_end && ascii_space(*value)) {
                value++;
            }
            while (value_end > value && ascii_space(value_end[-1])) {
                value_end--;
            }
            value_len = (size_t)(value_end - value);
            if (value_len + 1 > cap) {
                return -1;
            }
            memcpy(out, value, value_len);
            out[value_len] = '\0';
            return 0;
        }

        if (eol == NULL) {
            break;
        }
        p = eol + 1;
    }

    return -1;
}

int ps4u_safe_pkg_name(const char *name)
{
    size_t len;
    size_t i;
    const char *ext;

    if (name == NULL) {
        return -1;
    }
    len = strlen(name);
    if (len < 5 || len > 127) {
        return -1;
    }
    if (name[0] == '.' || name[0] == ' ') {
        return -1;
    }

    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];

        if (!(ascii_alnum(c) || c == '.' || c == '_' || c == '-' || c == ' ')) {
            return -1;
        }
    }

    /* ext zeigt auf die letzten vier Zeichen, also auf ".pkg". */
    ext = name + len - 4;
    if (ext[0] != '.' ||
        ascii_lower((unsigned char)ext[1]) != 'p' ||
        ascii_lower((unsigned char)ext[2]) != 'k' ||
        ascii_lower((unsigned char)ext[3]) != 'g') {
        return -1;
    }
    return 0;
}

int ps4u_listable_file(const char *name)
{
    char base[128];
    size_t len;

    if (name == NULL) {
        return 0;
    }
    if (ps4u_safe_pkg_name(name) == 0) {
        return 1;
    }

    /* "<name>.pkg.part" ist der Rest eines abgebrochenen Uploads. */
    len = strlen(name);
    if (len <= 5 || len - 5 >= sizeof(base) ||
        memcmp(name + len - 5, ".part", 5) != 0) {
        return 0;
    }
    memcpy(base, name, len - 5);
    base[len - 5] = '\0';
    return ps4u_safe_pkg_name(base) == 0;
}

ps4u_resume_plan ps4u_resume_plan_for(unsigned long long offset,
                                      unsigned long long length,
                                      unsigned long long part_bytes,
                                      unsigned long long dest_bytes)
{
    /* Ein frischer Upload schreibt "wb" und schneidet einen alten Rest ab. */
    if (offset == 0) {
        return PS4U_RESUME_APPEND;
    }
    if (part_bytes == offset) {
        return length == 0 ? PS4U_RESUME_FINALIZE : PS4U_RESUME_APPEND;
    }
    /* Antwort auf ein frueheres Finalisieren verloren: nichts mehr zu tun. */
    if (length == 0 && dest_bytes == offset) {
        return PS4U_RESUME_ALREADY;
    }
    return PS4U_RESUME_CONFLICT;
}

unsigned long long ps4u_resume_offset(unsigned long long part_bytes,
                                      unsigned long long dest_bytes,
                                      int *complete)
{
    if (complete != NULL) {
        *complete = 0;
    }
    if (part_bytes > 0) {
        return part_bytes;
    }
    if (dest_bytes > 0) {
        if (complete != NULL) {
            *complete = 1;
        }
        return dest_bytes;
    }
    return 0;
}

int ps4u_route_status(ps4u_route route, ps4u_method method)
{
    switch (route) {
    case PS4U_ROUTE_ROOT:
        return method == PS4U_METHOD_GET ? 200 : 405;
    case PS4U_ROUTE_STATUS:
        return method == PS4U_METHOD_GET ? 200 : 405;
    case PS4U_ROUTE_UPLOAD:
        return method == PS4U_METHOD_POST || method == PS4U_METHOD_HEAD ||
                       method == PS4U_METHOD_DELETE
                   ? 200
                   : 405;
    case PS4U_ROUTE_FILES:
        return method == PS4U_METHOD_GET ? 200 : 405;
    case PS4U_ROUTE_DIAGNOSTICS:
        return method == PS4U_METHOD_GET ? 200 : 405;
    case PS4U_ROUTE_UNKNOWN:
    default:
        return 404;
    }
}

static const char *reason_phrase(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 409:
        return "Conflict";
    case 411:
        return "Length Required";
    case 429:
        return "Too Many Requests";
    case 500:
        return "Internal Server Error";
    case 507:
        return "Insufficient Storage";
    default:
        return "Error";
    }
}

size_t ps4u_response_head_extra(int status, const char *content_type,
                                unsigned long long body_len, const char *extra,
                                char *out, size_t cap)
{
    int needed;

    if (out == NULL || cap == 0) {
        return 0;
    }
    if (content_type == NULL) {
        content_type = "text/plain; charset=utf-8";
    }

    needed = snprintf(out, cap,
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %llu\r\n"
                      "%s"
                      "Cache-Control: no-store\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      status, reason_phrase(status), content_type,
                      body_len, extra != NULL ? extra : "");
    if (needed < 0) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)needed;
}

static size_t response_build(int status, const char *content_type,
                             const char *body, size_t body_len,
                             char *out, size_t cap)
{
    size_t head_len;
    size_t room;
    size_t take;

    if (out == NULL || cap == 0) {
        return 0;
    }

    head_len = ps4u_response_head_extra(status, content_type,
                                        (unsigned long long)body_len, NULL, out,
                                        cap);
    if (head_len >= cap) {
        out[cap - 1] = '\0';
        return head_len + body_len;
    }

    room = cap - head_len - 1;
    take = body_len < room ? body_len : room;
    if (take > 0 && body != NULL) {
        memcpy(out + head_len, body, take);
    }
    out[head_len + take] = '\0';
    return head_len + body_len;
}

size_t ps4u_error_response(int status, char *out, size_t cap)
{
    const char *body;

    switch (status) {
    case 400:
        body = "bad request\n";
        break;
    case 404:
        body = "not found\n";
        break;
    case 405:
        body = "method not allowed\n";
        break;
    default:
        body = "error\n";
        break;
    }

    return response_build(status, "text/plain; charset=utf-8", body,
                          strlen(body), out, cap);
}
