/*
 * PS4Upload Direkt
 *
 * Zeigt auf der PS4 eine bedienbare Oberflaeche im Bild (CPU-Framebuffer ueber
 * libSceVideoOut) und liefert gleichzeitig auf Port 8080 eine Web-Seite an
 * PS5, Handy oder PC im selben LAN/WLAN aus.
 *
 * Die App bleibt im Vordergrund - anders als eine in den Browser geschobene
 * App, die von der PS4 pausiert wird und danach kein Netzwerk mehr hat.
 *
 * Kein Streaming, keine Spielsteuerung, kein Capture.
 */

#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include <orbis/libkernel.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>

#include "vendor/graphics.h"
#include "vendor/log.h"
#include "diag.h"
#include "pin.h"
#include "pkg.h"
#include "upload.h"
#include "install.h"
#include "version.h"
#include "ui.h"

#define FRAME_WIDTH UI_WIDTH
#define FRAME_HEIGHT UI_HEIGHT
#define FRAME_DEPTH 4

/*
 * Videospeicher realistisch dimensionieren statt pauschal 0xC000000:
 *   1920 * 1080 * 4 Byte = 8.294.400 Byte pro Puffer (7,91 MiB)
 *   je Puffer auf 2 MiB  = 8.388.608 Byte (8 MiB)
 *   2 Puffer             = 16.777.216 Byte (16 MiB) = 0x01000000
 *
 * Jeder Puffer wird einzeln auf 2 MiB ausgerichtet (allocateDisplayMem),
 * damit sceVideoOutRegisterBuffers nicht an der Puffer-Ausrichtung scheitert.
 * Die frueheren 0xC000000 (192 MiB) waren das Zwoelffache des Bedarfs.
 */
#define FRAME_BUFFER_COUNT 2
#define FRAME_BUFFER_ALIGNMENT 0x200000

static const size_t FRAME_BUFFER_BYTES =
    (size_t)FRAME_WIDTH * FRAME_HEIGHT * FRAME_DEPTH;
static const size_t FRAME_BUFFER_ALIGNED_BYTES =
    ((FRAME_BUFFER_BYTES + (size_t)FRAME_BUFFER_ALIGNMENT - 1) /
     (size_t)FRAME_BUFFER_ALIGNMENT) * (size_t)FRAME_BUFFER_ALIGNMENT;
static const size_t VIDEO_MEMORY_BYTES = FRAME_BUFFER_ALIGNED_BYTES * FRAME_BUFFER_COUNT;

static_assert(VIDEO_MEMORY_BYTES == 16u * 1024u * 1024u,
              "Zwei ausgerichtete Puffer belegen genau 16 MiB");

#define LISTEN_PORT 8080
#define REQUEST_MAX 1024
#define RESPONSE_MAX 12288
#define HEADER_MAX 2560
#define BODY_CHUNK 65536
#define LOG_CAPACITY 12
#define UPLOAD_DIR "/data/ps4upload"

std::stringstream debugLogStream;

/* Destruktor ist im Sample nur deklariert, nicht definiert. */
Scene2D::~Scene2D()
{
    this->cleanupInit();
}

static int g_server = -1;
static unsigned int g_requests = 0;
static char g_client_ip[16] = "-";
static char g_local_ip[16] = "192.168.0.0";
static bool g_dirty = true;

static int g_pad = -1;
static unsigned int g_buttons = 0;
static int g_user_id = -1;
static PinState g_pin;
static PinClientState g_pin_clients[PIN_CLIENT_CAPACITY];

/*
 * Ein Client gleichzeitig. Der Body wird in Stuecken gelesen und sofort
 * geschrieben, damit eine 600-MB-PKG nicht im Speicher landet und die
 * Oberflaeche waehrend des Uploads weiter zeichnet.
 */
static struct
{
    int fd;
    int phase;
    char header[HEADER_MAX];
    int header_len;
    FILE *file;
    char path[192];
    char name[80];
    unsigned long long length;
    unsigned long long received;
    unsigned long long started_ms;
    unsigned long long last_io_ms;
    bool force;
} g_conn = { -1, 0, { 0 }, 0, 0, { 0 }, { 0 }, 0, 0, 0, 0, false };

static UiInstallView g_install_view;

static int g_tab = UI_TAB_STATUS;
static int g_log_scroll = 0;

static UiLogEntry g_log[LOG_CAPACITY];
static int g_log_count = 0;

/* ------------------------------------------------------------------------- */
/* Canvas-Adapter: UI zeichnet gegen Canvas, hier gegen den Framebuffer        */
/* ------------------------------------------------------------------------- */

class SceneCanvas : public Canvas
{
public:
    explicit SceneCanvas(Scene2D &scene) : scene_(scene) {}

    void fill(const UiColor &color) override
    {
        Color mapped = { color.r, color.g, color.b };
        scene_.FrameBufferFill(mapped);
    }

    void rect(int x, int y, int width, int height, const UiColor &color) override
    {
        Color mapped = { color.r, color.g, color.b };
        scene_.DrawRectangle(x, y, width, height, mapped);
    }

private:
    Scene2D &scene_;
};

/* ------------------------------------------------------------------------- */
/* Log und Zeit                                                              */
/* ------------------------------------------------------------------------- */

static unsigned int uptime_seconds(void)
{
    return (unsigned int)(sceKernelGetProcessTime() / 1000000ULL);
}

static void log_add(const char *client, const char *line)
{
    for (int index = LOG_CAPACITY - 1; index > 0; index--)
    {
        g_log[index] = g_log[index - 1];
    }

    UiLogEntry *entry = &g_log[0];
    ui_format_uptime(entry->time, sizeof(entry->time), uptime_seconds());

    int used = 0;
    while (client[used] != 0 && used < (int)sizeof(entry->client) - 1)
    {
        entry->client[used] = client[used];
        used++;
    }
    entry->client[used] = 0;

    used = 0;
    while (line[used] != 0 && used < (int)sizeof(entry->line) - 1)
    {
        entry->line[used] = line[used];
        used++;
    }
    entry->line[used] = 0;

    if (g_log_count < LOG_CAPACITY)
    {
        g_log_count++;
    }
}

/* ------------------------------------------------------------------------- */
/* HTTP-Antworten                                                             */
/* ------------------------------------------------------------------------- */

static void write_all(int fd, const char *data, int length)
{
    int sent = 0;
    while (sent < length)
    {
        int written = (int)write(fd, data + sent, (size_t)(length - sent));
        if (written <= 0)
        {
            return;
        }
        sent += written;
    }
}

static void send_page(int fd, const char *client_ip)
{
    static char body[RESPONSE_MAX];
    static char response[RESPONSE_MAX];
    int used = 0;
    int total = 0;

    used = ui_append_text(body, RESPONSE_MAX, used,
        "<!doctype html><html lang=\"de\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>PS4Upload Direkt</title><style>"
        ":root{color-scheme:dark}"
        "body{margin:0;font:16px/1.55 system-ui,-apple-system,Segoe UI,sans-serif;"
        "background:#0b1220;color:#e8eefc;display:flex;min-height:100vh;"
        "align-items:center;justify-content:center}"
        "main{width:100%;max-width:34rem;padding:2rem}"
        "h1{font-size:1.55rem;margin:.6rem 0 .3rem}"
        "p{margin:.55rem 0;color:#a9b8d6}"
        ".badge{display:inline-block;padding:.15rem .6rem;border-radius:999px;"
        "background:#123a2a;color:#7ef0b0;font-size:.78rem;letter-spacing:.04em}"
        "dl{display:grid;grid-template-columns:auto 1fr;gap:.4rem 1.1rem;"
        "margin:1.6rem 0 0;font-variant-numeric:tabular-nums}"
        "dt{color:#7f8fb0}dd{margin:0}"
        "code{background:#141d31;padding:.1rem .35rem;border-radius:.3rem}"
        "ol{margin:1.2rem 0 0;padding-left:1.2rem;color:#a9b8d6}"
        "li{margin:.25rem 0}"
        "</style></head><body><main>"
        "<span class=\"badge\">PS4 online</span>"
        "<h1>PS4Upload Direkt</h1>"
        "<p>Diese Seite liefert die PS4 selbst aus. Ein PC ist nicht beteiligt.</p>"
        "<dl><dt>Adresse</dt><dd><code>");
    used = ui_append_text(body, RESPONSE_MAX, used, g_local_ip);
    used = ui_append_text(body, RESPONSE_MAX, used,
        ":8080</code></dd><dt>Anfragen</dt><dd>");
    used = ui_append_uint(body, RESPONSE_MAX, used, g_requests);
    used = ui_append_text(body, RESPONSE_MAX, used,
        "</dd><dt>Client</dt><dd><code>");
    used = ui_append_text(body, RESPONSE_MAX, used, client_ip);
    used = ui_append_text(body, RESPONSE_MAX, used,
        "</code></dd><dt>Netz</dt><dd>LAN / WLAN</dd></dl>"
        "<p>Am Fernseher: mit <code>&lt;</code> und <code>&gt;</code> zwischen "
        "STATUS, LOG, NETZ und INSTALL wechseln.</p>"
        "<section style=\"margin-top:1.6rem;padding:1.1rem;border:1px solid #1d2b47;"
        "border-radius:.8rem;background:#101a2c\">"
        "<h2 style=\"font-size:.9rem;letter-spacing:.06em;color:#7f8fb0;margin:0 0 .8rem;"
        "text-transform:uppercase\">Update</h2>"
        "<p id=\"upd\" style=\"margin:0 0 .7rem;font-size:.9rem\">Installierte Version wird geladen ...</p>"
        "<button id=\"check\" style=\"padding:.5rem 1rem;border:1px solid #2c4370;"
        "border-radius:.5rem;background:#16233a;color:#e8eefc;font-weight:600\">Release pruefen</button>"
        "<a id=\"dl\" target=\"_blank\" rel=\"noopener\" style=\"display:none;margin-left:.5rem;"
        "padding:.5rem 1rem;border-radius:.5rem;background:#2360be;color:#fff;font-weight:600;"
        "text-decoration:none\">PKG laden</a>"
        "<button id=\"pick\" style=\"display:none;margin-left:.5rem;padding:.5rem 1rem;"
        "border:1px solid #2c4370;border-radius:.5rem;background:#16233a;color:#e8eefc\">"
        "Datei waehlen</button>"
        "</section>"
        "<section style=\"margin-top:1rem;padding:1.1rem;border:1px solid #1d2b47;"
        "border-radius:.8rem;background:#101a2c\">"
        "<h2 style=\"font-size:.9rem;letter-spacing:.06em;color:#7f8fb0;margin:0 0 .8rem;"
        "text-transform:uppercase\">PKG hochladen</h2>"
        "<input id=\"f\" type=\"file\" accept=\".pkg\" style=\"color:#a9b8d6\">"
        "<input id=\"pin\" inputmode=\"numeric\" maxlength=\"4\" placeholder=\"PIN vom TV\" "
        "style=\"width:7rem;margin-left:.5rem;padding:.5rem;border:1px solid #1d2b47;"
        "border-radius:.5rem;background:#0c1422;color:#e8eefc\">"
        "<button id=\"go\" style=\"margin-left:.6rem;padding:.5rem 1rem;border:0;"
        "border-radius:.5rem;background:#2360be;color:#fff;font-weight:600\">Senden</button>"
        "<div style=\"height:.6rem;border-radius:999px;background:#0c1422;overflow:hidden;"
        "margin-top:.9rem\"><i id=\"bar\" style=\"display:block;height:100%;width:0;"
        "background:#2360be;transition:width .2s\"></i></div>"
        "<p id=\"out\" style=\"margin:.5rem 0 0;font-size:.9rem\">Noch keine Datei gewaehlt.</p>"
        "</section>"
        "<section style=\"margin-top:1rem;padding:1.1rem;border:1px solid #1d2b47;"
        "border-radius:.8rem;background:#101a2c\">"
        "<h2 style=\"font-size:.9rem;letter-spacing:.06em;color:#7f8fb0;margin:0 0 .8rem;"
        "text-transform:uppercase\">Installation auf der PS4</h2>"
        "<p id=\"inst\" style=\"margin:0\">Bereit.</p></section>"
        "<script>"
        "var f=document.getElementById('f'),go=document.getElementById('go'),"
        "bar=document.getElementById('bar'),out=document.getElementById('out'),"
        "inst=document.getElementById('inst'),pin=document.getElementById('pin'),timer=null,"
        "upd=document.getElementById('upd'),dl=document.getElementById('dl'),"
        "check=document.getElementById('check'),pick=document.getElementById('pick'),"
        "installedVersion='',updDone=false;"
        "var names={idle:'Bereit',upload:'Upload',check:'Pruefung',register:'Registrierung',"
        "install:'Installation',done:'Fertig',failed:'Fehler'};"
        "function show(j){var t=names[j.phase]||j.phase;if(j.percent>=0){t+=' '+j.percent+' %';}"
        "if(j.title){t+=' - '+j.title;}if(j.error){t+=' ('+j.note+' 0x'+(j.error>>>0).toString(16)+')';}"
        "inst.textContent=t;}"
        "function poll(){fetch('/status',{cache:'no-store'}).then(function(r){return r.json();})"
        ".then(function(j){show(j.install);"
        "if(j.version&&!installedVersion){installedVersion=j.version;"
        "if(!updDone){upd.textContent='Installiert: '+installedVersion;}}}).catch(function(){});}"
        "function startPoll(){if(timer){return;}timer=setInterval(poll,1000);}"
        "function cmpVersion(a,b){var x=a.split('.'),y=b.split('.');"
        "for(var i=0;i<3;i++){var d=(parseInt(x[i],10)||0)-(parseInt(y[i],10)||0);if(d){return d;}}return 0;}"
        "check.onclick=function(){updDone=true;check.disabled=true;upd.textContent='Pruefe Release ...';"
        "fetch('https://api.github.com/repos/xsensenmannx/PS4Upload.pkg/releases/latest',{cache:'no-store'})"
        ".then(function(r){if(!r.ok){throw 0;}return r.json();})"
        ".then(function(rel){var tag=(rel.tag_name||'').replace(/^v/,'');"
        "var asset=(rel.assets||[]).filter(function(a){return /\\.pkg$/i.test(a.name);})[0];"
        "if(!asset){throw 0;}if(!installedVersion){"
        "upd.textContent='Installierte Version unbekannt - Seite neu laden.';return;}"
        "var c=cmpVersion(tag,installedVersion);"
        "if(c>0){upd.textContent='Update '+tag+' verfuegbar (installiert '+installedVersion+').';"
        "dl.href=asset.browser_download_url;dl.download=asset.name;"
        "dl.style.display='inline-block';pick.style.display='inline-block';}"
        "else if(c===0){upd.textContent='Bereits aktuell: '+installedVersion+'.';}"
        "else{upd.textContent='Installiert ist neuer ('+installedVersion+') als der Release ('+tag+').';}})"
        ".catch(function(){upd.textContent='Release-Abfrage fehlgeschlagen (Internet noetig).';})"
        ".then(function(){check.disabled=false;});};"
        "pick.onclick=function(){f.click();};"
        "function sendUpload(force){var file=f.files[0];"
        "if(!/^[0-9]{4}$/.test(pin.value)){out.textContent='Bitte die 4-stellige PIN vom Fernseher eingeben.';return;}"
        "if(!file){out.textContent='Bitte eine PKG waehlen.';return;}"
        "var xhr=new XMLHttpRequest();xhr.open('POST','/upload?name='+encodeURIComponent(file.name)"
        "+(force?'&force=1':''));"
        "xhr.setRequestHeader('X-Upload-Pin',pin.value);"
        "go.disabled=true;"
        "xhr.upload.onprogress=function(e){if(e.lengthComputable){"
        "bar.style.width=(e.loaded/e.total*100)+'%';"
        "out.textContent='Upload '+Math.round(e.loaded/e.total*100)+' %';}};"
        "xhr.onload=function(){go.disabled=false;"
        "if(xhr.status===200){out.textContent='Empfangen. Installation laeuft.';}"
        "else if(xhr.status===403){out.textContent='PIN falsch oder gesperrt - Code steht am Fernseher.';pin.value='';}"
        "else if(xhr.status===423){out.textContent='Installation laeuft noch - bitte warten.';}"
        "else if(xhr.status===409){var msg='Gleiche oder aeltere Version.';"
        "try{msg=JSON.parse(xhr.responseText).error||msg;}catch(e){}"
        "out.textContent=msg;if(confirm(msg+'\\n\\nTrotzdem installieren?')){sendUpload(true);}}"
        "else{out.textContent='Fehler '+xhr.status;}"
        "poll();startPoll();};"
        "xhr.onerror=function(){go.disabled=false;out.textContent='Netzwerkfehler.';};"
        "xhr.send(file);}"
        "go.onclick=function(){sendUpload(false);};"
        "poll();"
        "</script>"
        "<p>Streaming, Spielsteuerung und Capture sind nicht Teil dieses Stands.</p>"
        "</main></body></html>");

    total = ui_append_text(response, RESPONSE_MAX, 0,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "Content-Length: ");
    total = ui_append_uint(response, RESPONSE_MAX, total, (unsigned int)used);
    total = ui_append_text(response, RESPONSE_MAX, total, "\r\n\r\n");
    memcpy(response + total, body, (size_t)used);
    total += used;

    write_all(fd, response, total);
}

static int json_append(char *buffer, int capacity, int used, const char *text)
{
    while (*text != 0 && used < capacity - 1)
    {
        char value = *text++;

        if (value == '"' || value == '\\' || (unsigned char)value < 0x20)
        {
            value = '_';
        }
        buffer[used++] = value;
    }
    buffer[used] = 0;
    return used;
}

static const char *install_json_phase(void)
{
    switch (g_install.phase)
    {
        case INSTALL_PHASE_UPLOAD: return "upload";
        case INSTALL_PHASE_CHECK: return "check";
        case INSTALL_PHASE_REGISTER: return "register";
        case INSTALL_PHASE_RUNNING: return "install";
        case INSTALL_PHASE_DONE: return "done";
        case INSTALL_PHASE_FAILED: return "failed";
        default: return "idle";
    }
}

static void send_json(int fd)
{
    static char body[768];
    static char response[1024];
    int used = 0;
    int total = 0;

    used = ui_append_text(body, sizeof(body), used,
        "{\"ok\":true,\"app\":\"ps4upload-direct\",\"version\":\"" APP_VERSION "\","
        "\"address\":\"");
    used = ui_append_text(body, sizeof(body), used, g_local_ip);
    used = ui_append_text(body, sizeof(body), used, "\",\"port\":8080,\"requests\":");
    used = ui_append_uint(body, sizeof(body), used, g_requests);
    used = ui_append_text(body, sizeof(body), used, ",\"uptime\":");
    used = ui_append_uint(body, sizeof(body), used, uptime_seconds());
    used = ui_append_text(body, sizeof(body), used, ",\"install\":{\"phase\":\"");
    used = ui_append_text(body, sizeof(body), used, install_json_phase());
    used = ui_append_text(body, sizeof(body), used, "\",\"percent\":");
    if (g_install.percent < 0)
    {
        used = ui_append_text(body, sizeof(body), used, "-1");
    }
    else
    {
        used = ui_append_uint(body, sizeof(body), used, (unsigned int)g_install.percent);
    }
    used = ui_append_text(body, sizeof(body), used, ",\"error\":");
    used = ui_append_uint(body, sizeof(body), used, (unsigned int)g_install.error);
    used = ui_append_text(body, sizeof(body), used, ",\"title\":\"");
    used = json_append(body, sizeof(body), used, g_install.title);
    used = ui_append_text(body, sizeof(body), used, "\",\"file\":\"");
    used = json_append(body, sizeof(body), used, g_install.name);
    used = ui_append_text(body, sizeof(body), used, "\",\"note\":\"");
    used = json_append(body, sizeof(body), used, g_install.note);
    used = ui_append_text(body, sizeof(body), used, "\"}}");

    total = ui_append_text(response, sizeof(response), 0,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "Content-Length: ");
    total = ui_append_uint(response, sizeof(response), total, (unsigned int)used);
    total = ui_append_text(response, sizeof(response), total, "\r\n\r\n");
    memcpy(response + total, body, (size_t)used);
    total += used;

    write_all(fd, response, total);
}

static void send_status(int fd, int code, const char *reason, const char *allow)
{
    static char response[512];
    int used = 0;
    const char *body = code == 405 ? "method not allowed" : "not found";

    used = ui_append_text(response, sizeof(response), 0, "HTTP/1.1 ");
    used = ui_append_uint(response, sizeof(response), used, (unsigned int)code);
    used = ui_append_text(response, sizeof(response), used, " ");
    used = ui_append_text(response, sizeof(response), used, reason);
    used = ui_append_text(response, sizeof(response), used,
        "\r\nContent-Type: text/plain; charset=utf-8\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n");
    if (allow != 0)
    {
        used = ui_append_text(response, sizeof(response), used, "Allow: ");
        used = ui_append_text(response, sizeof(response), used, allow);
        used = ui_append_text(response, sizeof(response), used, "\r\n");
    }
    used = ui_append_text(response, sizeof(response), used, "Content-Length: ");
    used = ui_append_uint(response, sizeof(response), used, (unsigned int)ui_text_len(body));
    used = ui_append_text(response, sizeof(response), used, "\r\n\r\n");
    used = ui_append_text(response, sizeof(response), used, body);

    write_all(fd, response, used);
}

static int path_matches(const char *path, const char *expected)
{
    int index = 0;
    while (expected[index] != 0)
    {
        if (path[index] != expected[index])
        {
            return 0;
        }
        index++;
    }
    return path[index] == 0 || path[index] == '?' || path[index] == ' ';
}

/* ------------------------------------------------------------------------- */
/* HTTP-Server                                                               */
/* ------------------------------------------------------------------------- */

static void server_start(void)
{
    int option = 1;
    struct sockaddr_in address;

    g_server = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server < 0)
    {
        diag_text("socket() fehlgeschlagen");
        diag_code("errno", errno);
        return;
    }

    setsockopt(g_server, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(LISTEN_PORT);

    if (bind(g_server, (struct sockaddr *)&address, sizeof(address)) != 0)
    {
        diag_text("bind() fehlgeschlagen - Port belegt?");
        diag_number("Port", LISTEN_PORT);
        diag_code("errno", errno);
        close(g_server);
        g_server = -1;
        return;
    }

    if (listen(g_server, 8) != 0)
    {
        diag_text("listen() fehlgeschlagen");
        diag_code("errno", errno);
        close(g_server);
        g_server = -1;
        return;
    }

    diag_number("HTTP-Server Port", LISTEN_PORT);
}

static unsigned long long uptime_ms(void)
{
    return sceKernelGetProcessTime() / 1000ull;
}

static void conn_close(void)
{
    if (g_conn.file != 0)
    {
        fclose(g_conn.file);
        g_conn.file = 0;
    }
    if (g_conn.fd >= 0)
    {
        close(g_conn.fd);
        g_conn.fd = -1;
    }

    g_conn.phase = 0;
    g_conn.header_len = 0;
    g_conn.header[0] = 0;
    g_conn.length = 0;
    g_conn.received = 0;
    g_conn.started_ms = 0;
    g_conn.last_io_ms = 0;
    g_conn.path[0] = 0;
    g_conn.force = false;
}

static int header_prefix(const char *line, const char *name)
{
    for (int index = 0; name[index] != 0; index++)
    {
        char a = line[index];
        char b = name[index];

        if (a >= 'A' && a <= 'Z')
        {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z')
        {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b)
        {
            return 0;
        }
    }
    return 1;
}

static const char *header_value(const char *header, const char *name)
{
    const char *line = header;
    int name_length = ui_text_len(name);

    while (*line != 0)
    {
        const char *end = strstr(line, "\r\n");

        if (end == 0)
        {
            break;
        }
        if ((end - line) > name_length && header_prefix(line, name) && line[name_length] == ':')
        {
            const char *value = line + name_length + 1;

            while (*value == ' ')
            {
                value++;
            }
            return value;
        }
        line = end + 2;
    }

    return 0;
}

static unsigned long long header_number(const char *header, const char *name)
{
    const char *value = header_value(header, name);
    unsigned long long number = 0;

    if (value == 0)
    {
        return 0;
    }

    while (*value >= '0' && *value <= '9')
    {
        unsigned long long digit = (unsigned long long)(*value - '0');

        if (number > (ULLONG_MAX - digit) / 10ull)
        {
            return ULLONG_MAX;
        }
        number = number * 10ull + digit;
        value++;
    }

    return number;
}

static int hex_digit(char value)
{
    if (value >= '0' && value <= '9')
    {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f')
    {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F')
    {
        return value - 'A' + 10;
    }
    return -1;
}

/* Dateiname aus ?name=, prozent-dekodiert und auf sichere Zeichen reduziert. */
static void upload_name(const char *target, char *out, int capacity)
{
    const char *query = strstr(target, "?name=");
    int used = 0;

    out[0] = 0;

    if (query != 0)
    {
        const char *value = query + 6;

        while (*value != 0 && *value != ' ' && *value != '&' && used < capacity - 1)
        {
            char decoded = *value++;

            if (decoded == '%' && hex_digit(value[0]) >= 0 && hex_digit(value[1]) >= 0)
            {
                decoded = (char)(hex_digit(value[0]) * 16 + hex_digit(value[1]));
                value += 2;
            }

            if ((decoded >= 'a' && decoded <= 'z') || (decoded >= 'A' && decoded <= 'Z') ||
                (decoded >= '0' && decoded <= '9') || decoded == '-' || decoded == '_' ||
                decoded == '.')
            {
                out[used++] = decoded;
            }
            else
            {
                out[used++] = '_';
            }
        }
    }

    if (used == 0)
    {
        const char *fallback = "upload.pkg";

        while (fallback[used] != 0 && used < capacity - 1)
        {
            out[used] = fallback[used];
            used++;
        }
    }

    out[used] = 0;

    {
        int length = ui_text_len(out);
        bool has_suffix = false;

        if (length >= 4 && out[length - 4] == '.')
        {
            char a = out[length - 3];
            char b = out[length - 2];
            char c = out[length - 1];

            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            has_suffix = a == 'p' && b == 'k' && c == 'g';
        }

        if (!has_suffix && length + 4 < capacity)
        {
            out[length++] = '.';
            out[length++] = 'p';
            out[length++] = 'k';
            out[length++] = 'g';
            out[length] = 0;
        }
    }
}

static void send_result(int fd, int code, const char *reason, bool ok, const PkgInfo *info, const char *error)
{
    static char body[640];
    static char response[896];
    int used = 0;
    int total = 0;

    used = ui_append_text(body, sizeof(body), used, "{\"ok\":");
    used = ui_append_text(body, sizeof(body), used, ok ? "true" : "false");

    if (ok && info != 0)
    {
        used = ui_append_text(body, sizeof(body), used, ",\"title\":\"");
        used = json_append(body, sizeof(body), used, info->title);
        used = ui_append_text(body, sizeof(body), used, "\",\"content_id\":\"");
        used = json_append(body, sizeof(body), used, info->content_id);
        used = ui_append_text(body, sizeof(body), used, "\",\"size\":");
        used = ui_append_uint(body, sizeof(body), used,
                              (unsigned int)(info->size > 0xFFFFFFFFull ? 0xFFFFFFFFull : info->size));
    }
    else
    {
        used = ui_append_text(body, sizeof(body), used, ",\"error\":\"");
        used = json_append(body, sizeof(body), used, error != 0 ? error : "unbekannt");
        used = ui_append_text(body, sizeof(body), used, "\"");
    }
    used = ui_append_text(body, sizeof(body), used, "}");

    total = ui_append_text(response, sizeof(response), 0, "HTTP/1.1 ");
    total = ui_append_uint(response, sizeof(response), total, (unsigned int)code);
    total = ui_append_text(response, sizeof(response), total, " ");
    total = ui_append_text(response, sizeof(response), total, reason);
    total = ui_append_text(response, sizeof(response), total,
        "\r\nContent-Type: application/json; charset=utf-8\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "Content-Length: ");
    total = ui_append_uint(response, sizeof(response), total, (unsigned int)used);
    total = ui_append_text(response, sizeof(response), total, "\r\n\r\n");
    memcpy(response + total, body, (size_t)used);
    total += used;

    write_all(fd, response, total);
}

/*
 * Prueft die PIN aus dem Header X-Upload-Pin serverseitig. Fehlversuche
 * werden pro Client-IP gezaehlt und gesperrt; die PIN selbst steht nur auf
 * dem Fernseher.
 */
static bool pin_allow_upload(const char *header, const char *client_ip)
{
    const char *value = header_value(header, PIN_HEADER_NAME);
    PinClientState *client = pin_client_state(g_pin_clients, PIN_CLIENT_CAPACITY, client_ip);
    char given[PIN_DIGITS + 1];
    char note[80];
    unsigned long long now = uptime_ms();
    int index;
    bool parsed;

    if (client == 0)
    {
        diag_text("PIN-Clienttabelle voll - Upload abgewiesen");
        install_mark_failed_note(0, "PIN-CLIENTTABELLE VOLL");
        return false;
    }

    if (pin_locked(*client, now))
    {
        diag_text("Upload abgewiesen: PIN fuer diesen Client gesperrt");
        install_mark_failed_note(0, "PIN GESPERRT - KURZ WARTEN");
        return false;
    }

    parsed = pin_parse_value(value, given, (int)sizeof(given));
    if (parsed && pin_equal(g_pin.value, given))
    {
        pin_succeeded(*client);
        return true;
    }

    pin_fail(*client, now);

    index = 0;
    index = diag_append(note, sizeof(note), index, "Upload abgewiesen: falsche PIN (Versuch ");
    index = diag_uint(note, sizeof(note), index, client->failures);
    diag_append(note, sizeof(note), index, ")");
    diag_text(note);

    if (pin_locked(*client, now))
    {
        diag_text("Zu viele Fehlversuche - Upload 60 s fuer diesen Client gesperrt");
    }

    index = 0;
    index = ui_append_text(note, sizeof(note), index, "PIN FALSCH - VERSUCH ");
    index = ui_append_uint(note, sizeof(note), index, client->failures);
    install_mark_failed_note(0, note);

    return false;
}

static void conn_finish_upload(void)
{
    PkgInfo info;
    bool ok;
    bool closed_ok = true;
    int fd = g_conn.fd;

    if (g_conn.file != 0)
    {
        if (fclose(g_conn.file) != 0)
        {
            closed_ok = false;
        }
        g_conn.file = 0;
    }

    if (!closed_ok)
    {
        remove(g_conn.path);
        install_mark_failed_note(-5, "SPEICHER VOLL ODER SCHREIBFEHLER");
        send_result(fd, 507, "Insufficient Storage", false, 0,
                    "Schreibfehler oder Speicher voll");
        log_add(g_client_ip, "UPLOAD FEHLER");
        conn_close();
        return;
    }

    install_mark_check();

    ok = pkg_probe(g_conn.path, &info);
    if (!ok)
    {
        remove(g_conn.path);
        install_mark_failed_note(-1, info.error);
        send_result(fd, 400, "Bad Request", false, 0, info.error);
        log_add(g_client_ip, "PKG UNGUELTIG");
        conn_close();
        return;
    }

    {
        int installed = pkg_version_value(APP_VER);
        int package = pkg_version_value(info.app_ver);
        bool not_newer;

        /* Der Parser lehnt das schon ab; hier bleibt die 409-Sperre auch bei
         * einem spaeteren Parserfehler fail-closed. */
        if (installed < 0 || package < 0)
        {
            remove(g_conn.path);
            install_mark_failed_note(-8, "APP_VER UNGUELTIG");
            send_result(fd, 400, "Bad Request", false, 0, "APP_VER ungueltig");
            log_add(g_client_ip, "PKG VERSION UNGUELTIG");
            conn_close();
            return;
        }

        not_newer = package <= installed;

        if (not_newer && !g_conn.force)
        {
            char message[128];
            char note[64];
            int used = 0;
            int index = 0;

            used = ui_append_text(message, sizeof(message), used, "Paket ");
            used = ui_append_text(message, sizeof(message), used, info.app_ver);
            used = ui_append_text(message, sizeof(message), used, " ist nicht neuer als installiert ");
            used = ui_append_text(message, sizeof(message), used, APP_VER);
            used = ui_append_text(message, sizeof(message), used, " - Installation abgebrochen");

            index = ui_append_text(note, sizeof(note), index, "VERSION ");
            index = ui_append_text(note, sizeof(note), index, info.app_ver);
            index = ui_append_text(note, sizeof(note), index, " NICHT NEUER");

            remove(g_conn.path);
            install_mark_failed_note(0, note);
            send_result(fd, 409, "Conflict", false, 0, message);
            log_add(g_client_ip, "VERSION ZU ALT");
            diag_text(message);
            conn_close();
            return;
        }

        if (not_newer)
        {
            diag_text("Nicht neuere Version auf Wunsch installiert");
        }
    }

    send_result(fd, 200, "OK", true, &info, 0);
    log_add(g_client_ip, "PKG EMPFANGEN");

    install_begin(g_conn.path, info, g_user_id);

    conn_close();
}

static void conn_write_body(const char *data, int bytes)
{
    int writable;

    if (bytes <= 0 || g_conn.file == 0)
    {
        return;
    }

    writable = bytes;
    if (g_conn.received + (unsigned long long)writable > g_conn.length)
    {
        writable = (int)(g_conn.length - g_conn.received);
    }
    if (writable <= 0)
    {
        return;
    }

    if (fwrite(data, 1, (size_t)writable, g_conn.file) != (size_t)writable)
    {
        int fd = g_conn.fd;

        fclose(g_conn.file);
        g_conn.file = 0;
        remove(g_conn.path);
        install_mark_failed_note(-5, "SPEICHER VOLL ODER SCHREIBFEHLER");
        send_result(fd, 507, "Insufficient Storage", false, 0, "Schreibfehler oder Speicher voll");
        log_add(g_client_ip, "UPLOAD FEHLER");
        conn_close();
        return;
    }

    g_conn.received += (unsigned long long)writable;
    install_mark_upload(g_conn.received, g_conn.length);
    g_dirty = true;

    if (g_conn.received >= g_conn.length)
    {
        conn_finish_upload();
    }
}

static void conn_abort_upload(const char *note)
{
    if (g_conn.file != 0)
    {
        fclose(g_conn.file);
        g_conn.file = 0;
    }
    if (g_conn.path[0] != 0)
    {
        remove(g_conn.path);
        g_conn.path[0] = 0;
    }

    install_mark_failed_note(-6, note);
    log_add(g_client_ip, note);
    conn_close();
}

static unsigned long long upload_free_bytes(void)
{
    struct statvfs fs;
    unsigned long long block;

    if (statvfs(UPLOAD_DIR, &fs) != 0)
    {
        diag_code("statvfs", errno);
        return 0;
    }

    block = fs.f_frsize != 0 ? (unsigned long long)fs.f_frsize
                             : (unsigned long long)fs.f_bsize;
    return (unsigned long long)fs.f_bavail * block;
}

static bool conn_begin_upload(const char *target, unsigned long long content_length)
{
    int used = 0;

    g_conn.force = strstr(target, "force=1") != 0;

    if (install_busy())
    {
        send_result(g_conn.fd, 423, "Locked", false, 0, "Installation laeuft noch");
        log_add(g_client_ip, "UPLOAD BLOCKIERT - INSTALLATION LAEUFT");
        return false;
    }

    if (!pin_allow_upload(g_conn.header, g_client_ip))
    {
        send_result(g_conn.fd, 403, "Forbidden", false, 0, "PIN falsch oder gesperrt");
        log_add(g_client_ip, "PIN ABGEWIESEN");
        return false;
    }

    if (content_length == 0)
    {
        send_status(g_conn.fd, 411, "Length Required", 0);
        install_mark_failed_note(-3, "KEINE LAENGE ANGEGEBEN");
        return false;
    }

    mkdir(UPLOAD_DIR, 0777);
    {
        unsigned long long free_bytes = upload_free_bytes();

        if (!upload_bgft_space_ok(content_length, free_bytes, UPLOAD_SPACE_RESERVE_BYTES))
        {
            send_result(g_conn.fd, 507, "Insufficient Storage", false, 0,
                        "Zu wenig freier Speicher");
            install_mark_failed_note(-7, "ZU WENIG SPEICHER");
            diag_size("Freier Speicher", (size_t)free_bytes);
            diag_size("Upload-Groesse", (size_t)content_length);
            log_add(g_client_ip, "ZU WENIG SPEICHER");
            return false;
        }
    }

    upload_name(target, g_conn.name, sizeof(g_conn.name));

    used = ui_append_text(g_conn.path, sizeof(g_conn.path), used, UPLOAD_DIR "/");
    used = ui_append_text(g_conn.path, sizeof(g_conn.path), used, g_conn.name);

    /* Die vorherige Datei erst jetzt loeschen: BGFT koennte sie waehrend der
     * Installation noch pruefen. Bei gleichem Namen wird sie ohnehin durch
     * das Oeffnen mit "wb" ersetzt. */
    if (g_install.path[0] != 0 && strcmp(g_install.path, g_conn.path) != 0)
    {
        remove(g_install.path);
    }

    g_conn.file = fopen(g_conn.path, "wb");
    if (g_conn.file == 0)
    {
        send_status(g_conn.fd, 507, "Insufficient Storage", 0);
        install_mark_failed_note(-4, "DATEI NICHT ANLEGBAR");
        log_add(g_client_ip, "DATEI NICHT ANLEGBAR");
        return false;
    }

    g_conn.length = content_length;
    g_conn.received = 0;
    g_conn.phase = 2;

    install_reset(g_conn.name);
    install_mark_upload(0, content_length);
    log_add(g_client_ip, "UPLOAD GESTARTET");
    g_dirty = true;

    return true;
}

static void conn_handle_request(void)
{
    char method[8];
    char target[256];
    const char *cursor = g_conn.header;
    const char *body;
    const char *expect;
    unsigned long long content_length;
    int used = 0;
    int body_bytes = 0;

    used = 0;
    while (*cursor != 0 && *cursor != ' ' && used < (int)sizeof(method) - 1)
    {
        method[used++] = *cursor++;
    }
    method[used] = 0;
    while (*cursor == ' ')
    {
        cursor++;
    }

    used = 0;
    while (*cursor != 0 && *cursor != ' ' && used < (int)sizeof(target) - 1)
    {
        target[used++] = *cursor++;
    }
    target[used] = 0;

    body = strstr(g_conn.header, "\r\n\r\n");
    if (body != 0)
    {
        body += 4;
        body_bytes = g_conn.header_len - (int)(body - g_conn.header);
    }

    content_length = header_number(g_conn.header, "content-length");
    expect = header_value(g_conn.header, "expect");

    g_requests++;
    g_dirty = true;

    if (method[0] == 'G' && method[1] == 'E' && method[2] == 'T' && method[3] == 0)
    {
        if (path_matches(target, "/status"))
        {
            send_json(g_conn.fd);
        }
        else if (path_matches(target, "/"))
        {
            send_page(g_conn.fd, g_client_ip);
        }
        else
        {
            send_status(g_conn.fd, 404, "Not Found", 0);
        }
        log_add(g_client_ip, target);
        conn_close();
        return;
    }

    if (!(method[0] == 'P' && method[1] == 'O' && method[2] == 'S' && method[3] == 'T' && method[4] == 0))
    {
        send_status(g_conn.fd, 405, "Method Not Allowed", "GET, POST");
        log_add(g_client_ip, method);
        conn_close();
        return;
    }

    if (!path_matches(target, "/upload"))
    {
        send_status(g_conn.fd, 404, "Not Found", 0);
        log_add(g_client_ip, target);
        conn_close();
        return;
    }

    if (!conn_begin_upload(target, content_length))
    {
        conn_close();
        return;
    }

    /* Erst nach Sperre, PIN und Speicherpruefung weitersenden lassen. */
    if (expect != 0 && header_prefix(expect, "100-continue"))
    {
        write_all(g_conn.fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    }

    if (body_bytes > 0)
    {
        conn_write_body(body, body_bytes);
    }
}

static void server_poll(void)
{
    unsigned long long now = uptime_ms();

    if (g_server < 0)
    {
        return;
    }

    if (g_conn.fd < 0)
    {
        fd_set read_set;
        struct timeval timeout;
        struct sockaddr_in client_address;
        socklen_t client_length = sizeof(client_address);
        int client_fd;

        FD_ZERO(&read_set);
        FD_SET(g_server, &read_set);
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;

        if (select(g_server + 1, &read_set, 0, 0, &timeout) <= 0)
        {
            return;
        }

        client_fd = accept(g_server, (struct sockaddr *)&client_address, &client_length);
        if (client_fd < 0)
        {
            return;
        }

        inet_ntop(AF_INET, &client_address.sin_addr, g_client_ip, sizeof(g_client_ip));

        g_conn.fd = client_fd;
        g_conn.phase = 1;
        g_conn.header_len = 0;
        g_conn.header[0] = 0;
        g_conn.started_ms = now;
        g_conn.last_io_ms = now;
        return;
    }

    if (g_conn.phase == 1)
    {
        fd_set read_set;
        struct timeval timeout;
        int room = (int)sizeof(g_conn.header) - 1 - g_conn.header_len;
        int received;

        if (now - g_conn.started_ms > 10000ull)
        {
            conn_close();
            return;
        }
        if (room <= 0)
        {
            conn_close();
            return;
        }

        FD_ZERO(&read_set);
        FD_SET(g_conn.fd, &read_set);
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;

        if (select(g_conn.fd + 1, &read_set, 0, 0, &timeout) <= 0)
        {
            return;
        }

        received = (int)read(g_conn.fd, g_conn.header + g_conn.header_len, (size_t)room);
        if (received <= 0)
        {
            conn_close();
            return;
        }

        g_conn.header_len += received;
        g_conn.header[g_conn.header_len] = 0;
        g_conn.last_io_ms = now;

        if (strstr(g_conn.header, "\r\n\r\n") != 0)
        {
            conn_handle_request();
        }
        return;
    }

    if (g_conn.phase == 2)
    {
        char buffer[BODY_CHUNK];

        for (int round = 0; round < 16 && g_conn.phase == 2; round++)
        {
            fd_set read_set;
            struct timeval timeout;
            int received;

            if (upload_idle_expired(now, g_conn.last_io_ms, UPLOAD_IDLE_TIMEOUT_MS))
            {
                conn_abort_upload("UPLOAD TIMEOUT");
                break;
            }

            FD_ZERO(&read_set);
            FD_SET(g_conn.fd, &read_set);
            timeout.tv_sec = 0;
            timeout.tv_usec = 0;

            if (select(g_conn.fd + 1, &read_set, 0, 0, &timeout) <= 0)
            {
                break;
            }

            received = (int)read(g_conn.fd, buffer, sizeof(buffer));
            if (received <= 0)
            {
                conn_abort_upload("UPLOAD ABGEBROCHEN");
                break;
            }

            g_conn.last_io_ms = uptime_ms();
            conn_write_body(buffer, received);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Eigene LAN-Adresse ermitteln (UDP-Verbindung, es wird nichts gesendet)     */
/* ------------------------------------------------------------------------- */

static void detect_local_ip(void)
{
    int probe;
    struct sockaddr_in destination;
    struct sockaddr_in local;
    socklen_t local_length = sizeof(local);
    unsigned char probe_address[4] = { 8, 8, 8, 8 };

    probe = socket(AF_INET, SOCK_DGRAM, 0);
    if (probe < 0)
    {
        return;
    }

    memset(&destination, 0, sizeof(destination));
    destination.sin_family = AF_INET;
    destination.sin_port = htons(53);
    memcpy(&destination.sin_addr.s_addr, probe_address, sizeof(probe_address));

    if (connect(probe, (struct sockaddr *)&destination, sizeof(destination)) == 0 &&
        getsockname(probe, (struct sockaddr *)&local, &local_length) == 0)
    {
        inet_ntop(AF_INET, &local.sin_addr, g_local_ip, sizeof(g_local_ip));
    }

    close(probe);

    {
        char line[64];
        int used = 0;

        used = diag_append(line, sizeof(line), used, "LAN-Adresse: ");
        used = diag_append(line, sizeof(line), used, g_local_ip);
        diag_text(line);
    }
}

/* ------------------------------------------------------------------------- */
/* Controller                                                                */
/* ------------------------------------------------------------------------- */

static void pad_start(void)
{
    int32_t user_id = -1;

    sceUserServiceInitialize(0);

    if (sceUserServiceGetInitialUser(&user_id) < 0)
    {
        DEBUGLOG << "Kein Benutzer fuer den Controller gefunden";
        return;
    }

    g_user_id = (int)user_id;

    scePadInit();
    g_pad = scePadOpen(user_id, 0, 0, 0);

    if (g_pad < 0)
    {
        DEBUGLOG << "Controller konnte nicht geoeffnet werden: 0x" << g_pad;
        return;
    }

    DEBUGLOG << "Controller bereit";
}

static void pad_poll(void)
{
    OrbisPadData data;

    if (g_pad < 0)
    {
        return;
    }

    memset(&data, 0, sizeof(data));
    if (scePadReadState(g_pad, &data) != 0)
    {
        return;
    }

    unsigned int pressed = data.buttons & ~g_buttons;
    g_buttons = data.buttons;

    if (pressed == 0)
    {
        return;
    }

    if (pressed & ORBIS_PAD_BUTTON_LEFT)
    {
        g_tab = (g_tab + UI_TAB_COUNT - 1) % UI_TAB_COUNT;
        g_dirty = true;
    }
    if (pressed & ORBIS_PAD_BUTTON_RIGHT)
    {
        g_tab = (g_tab + 1) % UI_TAB_COUNT;
        g_dirty = true;
    }

    int max_scroll = g_log_count > UI_LOG_VISIBLE ? g_log_count - UI_LOG_VISIBLE : 0;

    if (pressed & ORBIS_PAD_BUTTON_UP)
    {
        if (g_log_scroll > 0)
        {
            g_log_scroll--;
        }
        g_dirty = true;
    }
    if (pressed & ORBIS_PAD_BUTTON_DOWN)
    {
        if (g_log_scroll < max_scroll)
        {
            g_log_scroll++;
        }
        g_dirty = true;
    }
}

/* ------------------------------------------------------------------------- */

/*
 * Meldet die Stufe, an der Scene2D::Init abgebrochen ist, mit dem Rueckgabecode
 * genau dieser API. Keine eigene Probe: der nach einem Fehlschlag noch offene
 * Video-Handle haette nur einen Folgefehler gezeigt. cleanupInit() raeumt auf,
 * die gemeldete Stufe ist also die tatsaechliche Fehlerstelle.
 */
static void report_video_failure(Scene2D &scene, size_t requested)
{
    char line[192];
    int used = 0;

    diag_text("Videoausgabe fehlgeschlagen");

    used = diag_append(line, sizeof(line), used, "Stufe: ");
    used = diag_append(line, sizeof(line), used, scene.GetInitStageName());
    used = diag_append(line, sizeof(line), used, ", Code ");
    used = diag_hex(line, sizeof(line), used, (unsigned int)scene.GetInitCode());
    diag_text(line);

    used = 0;
    used = diag_append(line, sizeof(line), used, "Anforderung: ");
    used = diag_uint(line, sizeof(line), used, FRAME_BUFFER_COUNT);
    used = diag_append(line, sizeof(line), used, " Puffer a ");
    used = diag_uint(line, sizeof(line), used, (unsigned int)FRAME_BUFFER_BYTES);
    used = diag_append(line, sizeof(line), used, " Byte, zusammen ");
    used = diag_uint(line, sizeof(line), used, (unsigned int)(requested / 1048576u));
    used = diag_append(line, sizeof(line), used, " MiB");
    diag_text(line);

    diag_size("Direktspeicher gesamt", sceKernelGetDirectMemorySize());
    diag_text("App laeuft ohne Bild weiter - HTTP-Server bleibt erreichbar");
}

static void install_view_sync(void)
{
    g_install_view.phase = install_phase_name(g_install.phase);
    g_install_view.file = g_install.name[0] != 0 ? g_install.name : "-";
    g_install_view.title = g_install.title[0] != 0 ? g_install.title : "-";
    g_install_view.content_id = g_install.content_id[0] != 0 ? g_install.content_id : "-";
    g_install_view.note = g_install.note;
    g_install_view.pin = g_pin.value;
    g_install_view.percent = g_install.percent;
    g_install_view.error = g_install.error;
    g_install_view.done = g_install.phase == INSTALL_PHASE_DONE;
    g_install_view.failed = g_install.phase == INSTALL_PHASE_FAILED;
    g_install_view.active = g_install.phase == INSTALL_PHASE_UPLOAD ||
                            g_install.phase == INSTALL_PHASE_CHECK ||
                            g_install.phase == INSTALL_PHASE_REGISTER ||
                            g_install.phase == INSTALL_PHASE_RUNNING;
}

static int g_last_install_phase = -1;
static int g_last_install_percent = -100;

int main(void)
{
    static Scene2D scene(FRAME_WIDTH, FRAME_HEIGHT, FRAME_DEPTH);
    SceneCanvas canvas(scene);
    UiState state;
    int frame_id = 0;

    debugLogStream.str("");

    if (scene.Init(VIDEO_MEMORY_BYTES, FRAME_BUFFER_COUNT))
    {
        char line[96];
        int used = 0;

        used = diag_append(line, sizeof(line), used, "Videoausgabe bereit: ");
        used = diag_uint(line, sizeof(line), used, (unsigned int)(VIDEO_MEMORY_BYTES / 1048576u));
        used = diag_append(line, sizeof(line), used, " MiB, ");
        used = diag_uint(line, sizeof(line), used, FRAME_BUFFER_COUNT);
        used = diag_append(line, sizeof(line), used, " Puffer");
        diag_text(line);
    }
    else
    {
        report_video_failure(scene, VIDEO_MEMORY_BYTES);

        detect_local_ip();
        server_start();

        for (;;)
        {
            server_poll();
            install_poll(uptime_ms());
            sceKernelUsleep(16 * 1000);
        }
    }

    scene.SetActiveFrameBuffer(0);

    detect_local_ip();
    server_start();
    pad_start();

    mkdir(UPLOAD_DIR, 0777);
    install_reset(0);

    {
        unsigned int entropy = (unsigned int)sceKernelGetProcessTime();

        entropy ^= (unsigned int)(uintptr_t)&g_pin;
        entropy ^= (unsigned int)(uintptr_t)&g_conn;
        pin_generate(g_pin, entropy);
        diag_text("Upload-PIN erzeugt");
    }

    state.address = g_local_ip;
    state.client = g_client_ip;
    state.log = g_log;
    state.install = &g_install_view;

    for (;;)
    {
        pad_poll();
        server_poll();
        install_poll(uptime_ms());
        install_view_sync();

        if (g_install.phase != g_last_install_phase || g_install.percent != g_last_install_percent)
        {
            g_last_install_phase = g_install.phase;
            g_last_install_percent = g_install.percent;
            g_dirty = true;
        }

        if (g_dirty)
        {
            state.client = g_client_ip;
            state.requests = g_requests;
            state.uptime_seconds = uptime_seconds();
            state.server_ok = g_server >= 0;
            state.tab = g_tab;
            state.log_scroll = g_log_scroll;
            state.log_count = g_log_count;

            ui_draw(canvas, state);
            scene.SubmitFlip(frame_id);
            scene.FrameWait(frame_id);
            scene.FrameBufferSwap();
            frame_id++;
            g_dirty = false;
        }
        else
        {
            sceKernelUsleep(16 * 1000);
        }
    }

    return 0;
}
