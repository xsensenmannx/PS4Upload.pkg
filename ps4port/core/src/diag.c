#include "ps4u_diag.h"

#include <stdio.h>
#include <string.h>

#include "json_out.h"
#include "ps4u_status.h"

/* Testdatei des Schreib-/Loeschtests; sie liegt im Upload-Ordner, endet nicht
   auf .pkg und taucht deshalb weder in /files noch in der Quote auf. */
#define DIAG_TEST_NAME ".diagnostics.tmp"
#define DIAG_TEST_TEXT "ps4upload-diag"

static void out_bool(ps4u_out *o, int value)
{
    ps4u_out_text(o, value ? "true" : "false");
}

static void out_signed(ps4u_out *o, const char *prefix, long long value)
{
    char tmp[48];
    int written = snprintf(tmp, sizeof(tmp), "%s%lld", prefix, value);

    if (written > 0) {
        ps4u_out_raw(o, tmp, (size_t)written);
    }
}

/* Strings werden maskiert; ein zu langer Wert wird leer gemeldet, statt die
   Antwort abzuschneiden. NULL steht fuer "gibt es nicht". */
static void out_string(ps4u_out *o, const char *text)
{
    char escaped[192];

    if (text == NULL) {
        ps4u_out_text(o, "null");
        return;
    }
    if (ps4u_json_escape(text, escaped, sizeof(escaped)) >= sizeof(escaped)) {
        ps4u_out_text(o, "\"\"");
        return;
    }
    ps4u_out_text(o, "\"");
    ps4u_out_text(o, escaped);
    ps4u_out_text(o, "\"");
}

void ps4u_diag_write_test(const char *dir, ps4u_diag_write_result *out)
{
    static const char text[] = DIAG_TEST_TEXT;
    size_t want = sizeof(text) - 1;
    char path[512];
    char back[sizeof(text)];
    FILE *file;
    int written;
    size_t got;

    if (out == NULL) {
        return;
    }
    out->write_ok = 0;
    out->delete_ok = 0;
    out->error = NULL;

    if (dir == NULL) {
        out->error = "path";
        return;
    }
    written = snprintf(path, sizeof(path), "%s/%s", dir, DIAG_TEST_NAME);
    if (written <= 0 || (size_t)written >= sizeof(path)) {
        out->error = "path";
        return;
    }

    file = fopen(path, "wb");
    if (file == NULL) {
        out->error = "open";
        return;
    }
    got = fwrite(text, 1, want, file);
    if (got != want || fclose(file) != 0) {
        out->error = "write";
        remove(path);
        return;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        out->error = "read";
        remove(path);
        return;
    }
    got = fread(back, 1, want, file);
    fclose(file);
    if (got != want || memcmp(back, text, want) != 0) {
        out->error = "read";
        remove(path);
        return;
    }

    /* Schreiben und Lesen haben geklappt; das Loeschen ist ein eigener Punkt,
       damit ein voller Ordner nicht als Schreibfehler gemeldet wird. */
    out->write_ok = 1;
    out->delete_ok = remove(path) == 0;
    if (!out->delete_ok) {
        out->error = "remove";
    }
}

size_t ps4u_diag_json(const ps4u_diag *diag, char *out, size_t cap)
{
    ps4u_out o;
    size_t i;
    int first = 1;

    if (diag == NULL) {
        return 0;
    }

    o.buf = out;
    o.cap = cap;
    o.len = 0;
    if (out != NULL && cap > 0) {
        out[0] = '\0';
    }

    ps4u_out_text(&o, "{\"ok\":true,\"version\":");
    out_string(&o, diag->version);

    ps4u_out_text(&o, ",\"dir\":{\"exists\":");
    out_bool(&o, diag->dir_exists);
    ps4u_out_unsigned(&o, ",\"free_bytes\":", diag->free_bytes);
    ps4u_out_unsigned(&o, ",\"used_bytes\":", diag->used_bytes);
    ps4u_out_unsigned(&o, ",\"quota\":", diag->quota);
    ps4u_out_text(&o, "}");

    ps4u_out_text(&o, ",\"write_test\":{\"write\":");
    out_bool(&o, diag->write_test.write_ok);
    ps4u_out_text(&o, ",\"delete\":");
    out_bool(&o, diag->write_test.delete_ok);
    ps4u_out_text(&o, ",\"error\":");
    out_string(&o, diag->write_test.error);
    ps4u_out_text(&o, "}");

    ps4u_out_text(&o, ",\"pin\":{\"file\":");
    out_bool(&o, diag->pin_file);
    ps4u_out_text(&o, ",\"valid\":");
    out_bool(&o, diag->pin_valid);
    ps4u_out_text(&o, "}");

    ps4u_out_text(&o, ",\"app_inst\":{\"ok\":");
    out_bool(&o, diag->app_inst_ok);
    out_signed(&o, ",\"result\":", (long long)diag->app_inst_result);
    ps4u_out_text(&o, "}");

    ps4u_out_text(&o, ",\"bgft\":{\"loaded\":");
    out_bool(&o, diag->bgft_loaded);
    ps4u_out_text(&o, ",\"path\":");
    out_string(&o, diag->bgft_path);
    out_signed(&o, ",\"result\":", (long long)diag->bgft_result);
    ps4u_out_text(&o, ",\"symbols\":[");

    for (i = 0; diag->symbols != NULL && i < diag->symbol_count; i++) {
        const ps4u_diag_symbol *symbol = &diag->symbols[i];

        if (symbol->name == NULL) {
            continue;
        }
        if (!first) {
            ps4u_out_text(&o, ",");
        }
        first = 0;
        ps4u_out_text(&o, "{\"name\":");
        out_string(&o, symbol->name);
        out_signed(&o, ",\"result\":", (long long)symbol->result);
        ps4u_out_text(&o, ",\"found\":");
        out_bool(&o, symbol->result == 0);
        ps4u_out_text(&o, "}");
    }
    ps4u_out_text(&o, "]}}");

    if (out != NULL && cap > 0) {
        out[o.len < cap ? o.len : cap - 1] = '\0';
    }
    return o.len;
}
