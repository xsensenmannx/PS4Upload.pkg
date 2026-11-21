#include "ps4u_status.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} writer;

static void w_raw(writer *w, const char *text, size_t len)
{
    if (w->buf != NULL && w->cap > 1 && w->len < w->cap - 1) {
        size_t room = w->cap - 1 - w->len;
        size_t take = len < room ? len : room;

        memcpy(w->buf + w->len, text, take);
    }
    w->len += len;
}

static void w_text(writer *w, const char *text)
{
    if (text != NULL) {
        w_raw(w, text, strlen(text));
    }
}

static void w_printf(writer *w, const char *format, ...)
{
    char tmp[768];
    va_list args;
    int written;

    va_start(args, format);
    written = vsnprintf(tmp, sizeof(tmp), format, args);
    va_end(args);
    if (written < 0) {
        return;
    }
    if ((size_t)written >= sizeof(tmp)) {
        written = (int)(sizeof(tmp) - 1);
    }
    w_raw(w, tmp, (size_t)written);
}

static void w_escaped(writer *w, const char *text)
{
    char tmp[1024];
    size_t len = ps4u_json_escape(text, tmp, sizeof(tmp));

    if (len >= sizeof(tmp)) {
        len = sizeof(tmp) - 1;
    }
    w_raw(w, tmp, len);
}

size_t ps4u_json_escape(const char *in, char *out, size_t cap)
{
    size_t len = 0;
    const unsigned char *p;

    if (out != NULL && cap > 0) {
        out[0] = '\0';
    }

    for (p = (const unsigned char *)in; *p != 0; p++) {
        char tmp[8];
        const char *rep;
        size_t rep_len;

        if (*p == '"') {
            rep = "\\\"";
            rep_len = 2;
        } else if (*p == '\\') {
            rep = "\\\\";
            rep_len = 2;
        } else if (*p == '\n') {
            rep = "\\n";
            rep_len = 2;
        } else if (*p == '\r') {
            rep = "\\r";
            rep_len = 2;
        } else if (*p == '\t') {
            rep = "\\t";
            rep_len = 2;
        } else if (*p < 0x20) {
            snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned)*p);
            rep = tmp;
            rep_len = 6;
        } else {
            tmp[0] = (char)*p;
            rep = tmp;
            rep_len = 1;
        }

        if (out != NULL && cap > 1 && len < cap - 1) {
            size_t room = cap - 1 - len;
            size_t take = rep_len < room ? rep_len : room;

            memcpy(out + len, rep, take);
        }
        len += rep_len;
    }

    if (out != NULL && cap > 0) {
        out[len < cap ? len : cap - 1] = '\0';
    }
    return len;
}

size_t ps4u_status_json(const ps4u_status *status, char *out, size_t cap)
{
    writer w;
    int percent;

    w.buf = out;
    w.cap = cap;
    w.len = 0;

    if (out != NULL && cap > 0) {
        out[0] = '\0';
    }

    percent = status->percent;
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }

    w_text(&w, "{\"ok\":true,\"version\":\"");
    w_escaped(&w, status->version != NULL ? status->version : "");
    w_printf(&w, "\",\"uptime_s\":%u,\"clients\":%u,\"queue\":%u,\"free_bytes\":%llu,",
             status->uptime_s, status->clients, status->queue, status->free_bytes);
    w_text(&w, "\"install\":{\"phase\":\"");
    w_escaped(&w, status->phase != NULL ? status->phase : "idle");
    w_printf(&w, "\",\"percent\":%d", percent);

    if (status->title != NULL) {
        w_text(&w, ",\"title\":\"");
        w_escaped(&w, status->title);
        w_text(&w, "\"");
    }
    if (status->error != NULL) {
        w_text(&w, ",\"error\":\"");
        w_escaped(&w, status->error);
        w_text(&w, "\"");
    }
    w_text(&w, "}}");

    if (out != NULL && cap > 0) {
        out[w.len < cap ? w.len : cap - 1] = '\0';
    }
    return w.len;
}
