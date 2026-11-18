/*
 * Oberflaeche von PS4Upload Direkt.
 *
 * Frei von PS4-Abhaengigkeiten: gezeichnet wird gegen die abstrakte
 * Canvas-Schnittstelle. Auf der PS4 steckt darunter Scene2D (Framebuffer),
 * beim Vorschau-Rendern auf dem PC eine Bitmap.
 */

#pragma once

#include <string.h>

#include "stb_easy_font.h"
#include "version.h"

#define UI_WIDTH 1920
#define UI_HEIGHT 1080

#define UI_TAB_STATUS 0
#define UI_TAB_LOG 1
#define UI_TAB_NET 2
#define UI_TAB_INSTALL 3
#define UI_TAB_COUNT 4

#define UI_LOG_VISIBLE 8

struct UiColor
{
    unsigned char r;
    unsigned char g;
    unsigned char b;
};

class Canvas
{
public:
    virtual ~Canvas() {}
    virtual void fill(const UiColor &color) = 0;
    virtual void rect(int x, int y, int width, int height, const UiColor &color) = 0;
};

struct UiLogEntry
{
    char time[12];
    char client[18];
    char line[44];
};

struct UiInstallView
{
    const char *phase;
    const char *file;
    const char *title;
    const char *content_id;
    const char *note;
    const char *pin;
    int percent;
    int error;
    bool done;
    bool failed;
    bool active;
};

struct UiState
{
    const char *address;
    const char *client;
    unsigned int requests;
    unsigned int uptime_seconds;
    bool server_ok;
    int tab;
    int log_scroll;
    const UiLogEntry *log;
    int log_count;
    const UiInstallView *install;
};

static int ui_text_len(const char *text)
{
    int length = 0;
    while (text[length] != 0)
    {
        length++;
    }
    return length;
}

static int ui_append_text(char *buffer, int capacity, int used, const char *text)
{
    while (*text != 0 && used < capacity - 1)
    {
        buffer[used++] = *text++;
    }
    buffer[used] = 0;
    return used;
}

static int ui_append_hex(char *buffer, int capacity, int used, unsigned int value)
{
    static const char digits[] = "0123456789abcdef";

    used = ui_append_text(buffer, capacity, used, "0x");
    for (int shift = 28; shift >= 0; shift -= 4)
    {
        if (used < capacity - 1)
        {
            buffer[used++] = digits[(value >> shift) & 0xF];
        }
    }
    buffer[used] = 0;
    return used;
}

static int ui_append_uint(char *buffer, int capacity, int used, unsigned int value)
{
    char digits[12];
    int count = 0;

    if (value == 0)
    {
        digits[count++] = '0';
    }
    while (value > 0)
    {
        digits[count++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (count > 0 && used < capacity - 1)
    {
        buffer[used++] = digits[--count];
    }
    buffer[used] = 0;
    return used;
}

/* stb_easy_font liefert Vierecke; wir fuellen sie als Rechtecke. */
static void ui_draw_text(Canvas &canvas, int origin_x, int origin_y, float scale, const UiColor &color, const char *text)
{
    static unsigned char vertex_buffer[96 * 1024];
    unsigned char white[4] = { 255, 255, 255, 255 };

    int quads = stb_easy_font_print(0.0f, 0.0f, (char *)text, white, vertex_buffer, sizeof(vertex_buffer));
    struct Vertex
    {
        float x;
        float y;
        float z;
        unsigned char color[4];
    };
    Vertex *vertices = (Vertex *)vertex_buffer;

    for (int quad = 0; quad < quads; quad++)
    {
        Vertex *corner = vertices + (quad * 4);
        float min_x = corner[0].x, max_x = corner[0].x;
        float min_y = corner[0].y, max_y = corner[0].y;

        for (int index = 1; index < 4; index++)
        {
            if (corner[index].x < min_x) min_x = corner[index].x;
            if (corner[index].x > max_x) max_x = corner[index].x;
            if (corner[index].y < min_y) min_y = corner[index].y;
            if (corner[index].y > max_y) max_y = corner[index].y;
        }

        int left = origin_x + (int)(min_x * scale);
        int top = origin_y + (int)(min_y * scale);
        int width = (int)((max_x - min_x) * scale) + 1;
        int height = (int)((max_y - min_y) * scale) + 1;

        if (width <= 0 || height <= 0)
        {
            continue;
        }
        canvas.rect(left, top, width, height, color);
    }
}

static float ui_text_width(const char *text, float scale)
{
    return (float)stb_easy_font_width((char *)text) * scale;
}

static void ui_draw_text_right(Canvas &canvas, int right_x, int origin_y, float scale, const UiColor &color, const char *text)
{
    ui_draw_text(canvas, right_x - (int)ui_text_width(text, scale), origin_y, scale, color, text);
}

static void ui_draw_text_centered(Canvas &canvas, int center_x, int origin_y, float scale, const UiColor &color, const char *text)
{
    ui_draw_text(canvas, center_x - (int)(ui_text_width(text, scale) / 2.0f), origin_y, scale, color, text);
}

static void ui_format_uptime(char *buffer, int capacity, unsigned int seconds)
{
    int used = 0;
    unsigned int rest = seconds % 60;

    used = ui_append_uint(buffer, capacity, used, seconds / 60);
    used = ui_append_text(buffer, capacity, used, ":");
    if (rest < 10)
    {
        used = ui_append_text(buffer, capacity, used, "0");
    }
    ui_append_uint(buffer, capacity, used, rest);
}

static void ui_draw_tabs(Canvas &canvas, int active_tab)
{
    static const char *labels[UI_TAB_COUNT] = { "STATUS", "LOG", "NETZ", "INSTALL" };

    UiColor accent = { 35, 96, 190 };
    UiColor card = { 20, 29, 49 };
    UiColor white = { 232, 238, 252 };
    UiColor muted = { 127, 143, 176 };

    for (int tab = 0; tab < UI_TAB_COUNT; tab++)
    {
        int x = 90 + tab * 300;
        bool active = tab == active_tab;

        canvas.rect(x, 170, 270, 76, active ? accent : card);
        if (active)
        {
            canvas.rect(x, 246, 270, 4, white);
        }
        ui_draw_text_centered(canvas, x + 135, 190, 3.0f, active ? white : muted, labels[tab]);
    }
}

static void ui_draw_status_tab(Canvas &canvas, const UiState &state)
{
    UiColor accent = { 35, 96, 190 };
    UiColor card = { 20, 29, 49 };
    UiColor white = { 232, 238, 252 };
    UiColor muted = { 127, 143, 176 };
    UiColor green = { 126, 240, 176 };

    char line[128];
    int used;

    canvas.rect(90, 290, 10, 670, accent);

    ui_draw_text(canvas, 130, 330, 2.6f, muted, "IM BROWSER OEFFNEN - PS5, HANDY ODER PC");

    used = 0;
    used = ui_append_text(line, sizeof(line), used, "HTTP://");
    used = ui_append_text(line, sizeof(line), used, state.address);
    used = ui_append_text(line, sizeof(line), used, ":8080");
    ui_draw_text(canvas, 130, 390, 6.4f, white, line);

    ui_draw_text(canvas, 130, 520, 2.3f, muted, "GLEICHES LAN/WLAN WIE DIE PS4 - PORT 8080");

    canvas.rect(130, 580, 1600, 2, card);

    used = 0;
    used = ui_append_text(line, sizeof(line), used, "SERVER: ");
    used = ui_append_text(line, sizeof(line), used, state.server_ok ? "ONLINE" : "FEHLER");
    ui_draw_text(canvas, 130, 620, 2.9f, state.server_ok ? green : white, line);

    used = 0;
    used = ui_append_text(line, sizeof(line), used, "ANFRAGEN: ");
    used = ui_append_uint(line, sizeof(line), used, state.requests);
    ui_draw_text(canvas, 130, 690, 2.9f, white, line);

    used = 0;
    used = ui_append_text(line, sizeof(line), used, "LETZTER CLIENT: ");
    used = ui_append_text(line, sizeof(line), used, state.client);
    ui_draw_text(canvas, 130, 760, 2.9f, white, line);

    if (state.install != 0 && state.install->pin != 0)
    {
        used = 0;
        used = ui_append_text(line, sizeof(line), used, "PIN FUER UPLOAD: ");
        used = ui_append_text(line, sizeof(line), used, state.install->pin);
        ui_draw_text(canvas, 130, 826, 2.6f, white, line);
    }

    ui_draw_text(canvas, 130, 880, 2.2f, muted, "MIT < > ZWISCHEN DEN REITERN WECHSELN");
}

static void ui_draw_log_tab(Canvas &canvas, const UiState &state)
{
    UiColor white = { 232, 238, 252 };
    UiColor muted = { 127, 143, 176 };
    UiColor line_color = { 36, 49, 76 };

    ui_draw_text(canvas, 130, 330, 2.3f, muted, "ZEIT");
    ui_draw_text(canvas, 330, 330, 2.3f, muted, "CLIENT");
    ui_draw_text(canvas, 650, 330, 2.3f, muted, "ANFRAGE");

    if (state.log_count == 0)
    {
        ui_draw_text(canvas, 130, 400, 3.0f, white, "NOCH KEINE ANFRAGEN");
        ui_draw_text(canvas, 130, 470, 2.2f, muted, "DIE SEITE IM BROWSER OEFFNEN - DANN ERSCHEINT HIER JEDE ANFRAGE");
        return;
    }

    int first = state.log_scroll;
    int last = first + UI_LOG_VISIBLE;

    for (int index = first; index < last && index < state.log_count; index++)
    {
        const UiLogEntry *entry = &state.log[index];
        int y = 390 + (index - first) * 66;

        ui_draw_text(canvas, 130, y, 2.5f, muted, entry->time);
        ui_draw_text(canvas, 330, y, 2.5f, white, entry->client);
        ui_draw_text(canvas, 650, y, 2.5f, white, entry->line);

        canvas.rect(130, y + 46, 1600, 1, line_color);
    }

    if (state.log_count > UI_LOG_VISIBLE)
    {
        ui_draw_text(canvas, 1300, 950, 2.1f, muted, "MIT ^ v SCROLLEN");
    }
}

static void ui_draw_net_tab(Canvas &canvas, const UiState &state)
{
    UiColor white = { 232, 238, 252 };
    UiColor muted = { 127, 143, 176 };
    UiColor green = { 126, 240, 176 };

    char line[128];
    int used;
    int y = 340;

    static const char *labels[6] = { "ADRESSE", "PORT", "SERVER", "ANFRAGEN", "LETZTER CLIENT", "NETZ" };

    for (int row = 0; row < 6; row++)
    {
        ui_draw_text(canvas, 150, y, 2.5f, muted, labels[row]);

        used = 0;
        switch (row)
        {
            case 0:
                used = ui_append_text(line, sizeof(line), used, "HTTP://");
                used = ui_append_text(line, sizeof(line), used, state.address);
                used = ui_append_text(line, sizeof(line), used, ":8080");
                break;
            case 1:
                used = ui_append_text(line, sizeof(line), used, "8080 (TCP)");
                break;
            case 2:
                used = ui_append_text(line, sizeof(line), used, state.server_ok ? "ONLINE" : "FEHLER");
                break;
            case 3:
                used = ui_append_uint(line, sizeof(line), used, state.requests);
                break;
            case 4:
                used = ui_append_text(line, sizeof(line), used, state.client);
                break;
            default:
                used = ui_append_text(line, sizeof(line), used, "LAN ODER WLAN, GLEICHES NETZ");
                break;
        }

        bool highlight = row == 2 && state.server_ok;
        ui_draw_text(canvas, 620, y, 2.5f, highlight ? green : white, line);
        y += 92;
    }

    ui_draw_text(canvas, 150, 900, 2.1f, muted, "DIESE APP BLEIBT IM VORDERGRUND - NUR SO BLEIBT DER SERVER ERREICHBAR");
}

static void ui_draw_install_tab(Canvas &canvas, const UiState &state)
{
    UiColor accent = { 35, 96, 190 };
    UiColor white = { 232, 238, 252 };
    UiColor muted = { 127, 143, 176 };
    UiColor green = { 126, 240, 176 };
    UiColor red = { 244, 132, 132 };
    UiColor track = { 12, 20, 34 };
    UiColor line_color = { 36, 49, 76 };

    const UiInstallView *view = state.install;
    const char *phase = view != 0 && view->phase != 0 ? view->phase : "BEREIT";
    bool failed = view != 0 && view->failed;
    bool done = view != 0 && view->done;
    int percent = view != 0 ? view->percent : -1;

    static const char *labels[4] = { "DATEI", "TITEL", "CONTENT-ID", "FEHLER" };
    char line[192];
    int used;
    int y = 590;

    ui_draw_text(canvas, 130, 330, 2.3f, muted, "PKG EMPFANGEN UND UEBER BGFT INSTALLIEREN");
    ui_draw_text(canvas, 130, 374, 5.0f, failed ? red : (done ? green : white), phase);

    if (view != 0 && view->pin != 0)
    {
        used = ui_append_text(line, sizeof(line), 0, "PIN: ");
        used = ui_append_text(line, sizeof(line), used, view->pin);
        ui_draw_text_right(canvas, 1830, 326, 3.2f, white, line);
    }

    if (percent >= 0)
    {
        used = ui_append_uint(line, sizeof(line), 0, (unsigned int)percent);
        used = ui_append_text(line, sizeof(line), used, " %");
        ui_draw_text_right(canvas, 1830, 386, 4.2f, failed ? red : muted, line);
    }

    canvas.rect(130, 490, 1600, 34, track);
    if (percent > 0)
    {
        int filled = 1600 * (percent > 100 ? 100 : percent) / 100;
        canvas.rect(130, 490, filled, 34, failed ? red : (done ? green : accent));
    }

    for (int row = 0; row < 4; row++)
    {
        const char *value = "-";

        if (view != 0)
        {
            if (row == 0)
            {
                value = view->file;
            }
            else if (row == 1)
            {
                value = view->title;
            }
            else if (row == 2)
            {
                value = view->content_id;
            }
        }

        if (row == 3 && failed)
        {
            used = 0;
            if (view->note != 0 && view->note[0] != 0)
            {
                used = ui_append_text(line, sizeof(line), used, view->note);
            }
            if (view->error != 0)
            {
                if (used > 0)
                {
                    used = ui_append_text(line, sizeof(line), used, " ");
                }
                used = ui_append_hex(line, sizeof(line), used, (unsigned int)view->error);
            }
            value = line;
        }

        ui_draw_text(canvas, 150, y, 2.4f, muted, labels[row]);
        ui_draw_text(canvas, 560, y, 2.4f, row == 3 && failed ? red : white, value != 0 ? value : "-");
        canvas.rect(150, y + 52, 1600, 1, line_color);
        y += 88;
    }

    ui_draw_text(canvas, 150, 930, 2.1f, muted, "PS5 ODER HANDY: SEITE OEFFNEN, PKG WAEHLEN, HOCHLADEN");
}

static void ui_draw(Canvas &canvas, const UiState &state)
{
    UiColor background = { 11, 18, 32 };
    UiColor card = { 16, 24, 41 };
    UiColor accent = { 35, 96, 190 };
    UiColor white = { 232, 238, 252 };
    UiColor muted = { 127, 143, 176 };

    char uptime[16];

    canvas.fill(background);
    canvas.rect(0, 0, UI_WIDTH, 8, accent);

    ui_draw_text(canvas, 90, 62, 5.2f, white, "PS4UPLOAD DIREKT");

    ui_format_uptime(uptime, sizeof(uptime), state.uptime_seconds);
    ui_draw_text_right(canvas, 1830, 78, 2.6f, muted, uptime);

    ui_draw_tabs(canvas, state.tab);

    canvas.rect(90, 290, 1740, 670, card);

    switch (state.tab)
    {
        case UI_TAB_LOG:
            ui_draw_log_tab(canvas, state);
            break;
        case UI_TAB_NET:
            ui_draw_net_tab(canvas, state);
            break;
        case UI_TAB_INSTALL:
            ui_draw_install_tab(canvas, state);
            break;
        default:
            ui_draw_status_tab(canvas, state);
            break;
    }

    ui_draw_text(canvas, 92, 1000, 2.1f, muted, "< > REITER        ^ v SCROLLEN        PS-TASTE: ZURUECK ZUM MENUE");
    ui_draw_text_right(canvas, 1830, 1000, 2.0f, muted, APP_VERSION " - BREW00090");
}
