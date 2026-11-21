#include "ps4u_files.h"

#include <stdio.h>
#include <string.h>

#include "json_out.h"
#include "ps4u_status.h"

#define QUOTA_KEY "quota_bytes"

/* Reine Ziffernfolge zwischen begin und end; alles andere ist unbrauchbar. */
static int parse_uint(const char *begin, const char *end,
                      unsigned long long *out)
{
    unsigned long long value = 0;
    const char *p = begin;

    while (p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }
    if (p == end) {
        return -1;
    }
    for (; p < end; p++) {
        unsigned digit;

        if (*p < '0' || *p > '9') {
            return -1;
        }
        digit = (unsigned)(*p - '0');
        if (value > (~(unsigned long long)0 - digit) / 10ull) {
            return -1;
        }
        value = value * 10ull + digit;
    }
    *out = value;
    return 0;
}

unsigned long long ps4u_config_quota(const char *text)
{
    unsigned long long quota = 0;
    const char *line;

    if (text == NULL) {
        return 0;
    }

    for (line = text; *line != '\0';) {
        const char *eol = strchr(line, '\n');
        const char *end = eol != NULL ? eol : line + strlen(line);
        const char *start = line;
        const char *key_end;
        const char *eq;
        unsigned long long value;

        while (start < end && (*start == ' ' || *start == '\t')) {
            start++;
        }
        while (end > start &&
               (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) {
            end--;
        }

        if (start < end && *start != '#') {
            eq = memchr(start, '=', (size_t)(end - start));
            if (eq != NULL) {
                key_end = eq;
                while (key_end > start &&
                       (key_end[-1] == ' ' || key_end[-1] == '\t')) {
                    key_end--;
                }
                if ((size_t)(key_end - start) == sizeof(QUOTA_KEY) - 1 &&
                    memcmp(start, QUOTA_KEY, sizeof(QUOTA_KEY) - 1) == 0 &&
                    parse_uint(eq + 1, end, &value) == 0) {
                    quota = value;
                }
            }
        }

        if (eol == NULL) {
            break;
        }
        line = eol + 1;
    }

    return quota;
}

int ps4u_quota_ok(unsigned long long used, unsigned long long add,
                  unsigned long long quota)
{
    if (quota == 0) {
        return 1;
    }
    if (used > quota) {
        return 0;
    }
    return add <= quota - used;
}

size_t ps4u_files_json(const ps4u_file_entry *files, size_t count,
                       unsigned long long used, unsigned long long quota,
                       unsigned long long free_bytes, char *out, size_t cap)
{
    ps4u_out s;
    char name[512];
    size_t i;
    int first = 1;

    s.buf = out;
    s.cap = cap;
    s.len = 0;
    if (out != NULL && cap > 0) {
        out[0] = '\0';
    }

    ps4u_out_text(&s, "{\"ok\":true");
    ps4u_out_unsigned(&s, ",\"used\":", used);
    ps4u_out_unsigned(&s, ",\"quota\":", quota);
    ps4u_out_unsigned(&s, ",\"free_bytes\":", free_bytes);
    ps4u_out_text(&s, ",\"files\":[");

    for (i = 0; i < count; i++) {
        size_t escaped;

        if (files == NULL || files[i].name == NULL) {
            continue;
        }
        escaped = ps4u_json_escape(files[i].name, name, sizeof(name));
        if (escaped >= sizeof(name)) {
            continue;
        }
        if (!first) {
            ps4u_out_text(&s, ",");
        }
        first = 0;
        ps4u_out_text(&s, "{\"name\":\"");
        ps4u_out_text(&s, name);
        ps4u_out_unsigned(&s, "\",\"bytes\":", files[i].bytes);
        ps4u_out_text(&s, "}");
    }
    ps4u_out_text(&s, "]}");

    if (out != NULL && cap > 0) {
        out[s.len < cap ? s.len : cap - 1] = '\0';
    }
    return s.len;
}
