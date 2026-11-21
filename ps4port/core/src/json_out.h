/*
 * Kleiner JSON-Puffer fuer die Antworten im Kern (die .c-Dateien in src):
 * sammelt Text und
 * Zahlen, schneidet am Pufferende ab und meldet trotzdem die volle Laenge wie
 * snprintf. Bewusst static im Header - keine oeffentliche Schnittstelle, und
 * die beiden Nutzer (files.c, diag.c) teilen sich eine Implementierung.
 */

#ifndef PS4U_JSON_OUT_H
#define PS4U_JSON_OUT_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} ps4u_out;

static void ps4u_out_raw(ps4u_out *o, const char *text, size_t len)
{
    if (o->buf != NULL && o->cap > 1 && o->len < o->cap - 1) {
        size_t room = o->cap - 1 - o->len;
        size_t take = len < room ? len : room;

        memcpy(o->buf + o->len, text, take);
    }
    o->len += len;
}

static void ps4u_out_text(ps4u_out *o, const char *text)
{
    if (text != NULL) {
        ps4u_out_raw(o, text, strlen(text));
    }
}

static void ps4u_out_unsigned(ps4u_out *o, const char *prefix,
                              unsigned long long value)
{
    char tmp[48];
    int written = snprintf(tmp, sizeof(tmp), "%s%llu", prefix, value);

    if (written > 0) {
        ps4u_out_raw(o, tmp, (size_t)written);
    }
}

#endif
