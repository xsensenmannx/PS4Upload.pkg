#ifndef PS4U_DIAG_H
#define PS4U_DIAG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Antwortpuffer fuer das Diagnostics-JSON; der Payload legt so viel an. Der
   Hosttest haelt fest, dass der groesste sinnvolle Fall hineinpasst. */
#define PS4U_DIAG_BODY_MAX 1024

/* Ein BGFT-Symbol und das Ergebnis seines sceKernelDlsym-Aufrufs. */
typedef struct {
    const char *name;
    int32_t result; /* 0 = gefunden */
} ps4u_diag_symbol;

/* Ergebnis des Schreib-/Loeschtests im Upload-Ordner. */
typedef struct {
    int write_ok;      /* Testdatei geschrieben und zurueckgelesen */
    int delete_ok;     /* Testdatei wieder entfernt */
    const char *error; /* "path", "open", "write", "read", "remove" oder NULL */
} ps4u_diag_write_result;

/* Rohdaten fuer GET /diagnostics. Der Payload sammelt sie, der Kern baut das
   JSON: keine Datei-Ein-/Ausgabe und keine Systemaufrufe im Builder, damit
   der Aufbau host-testbar bleibt. */
typedef struct {
    const char *version;         /* Pflicht */
    int dir_exists;              /* Upload-Ordner vorhanden */
    unsigned long long free_bytes;
    unsigned long long used_bytes;
    unsigned long long quota;    /* 0 = unbegrenzt */
    ps4u_diag_write_result write_test;
    int pin_file;                /* pin.txt vorhanden und lesbar */
    int pin_valid;               /* Inhalt ist ein brauchbarer PIN */
    int app_inst_ok;             /* sceAppInstUtilInitialize == 0 */
    int32_t app_inst_result;     /* Rueckgabe von sceAppInstUtilInitialize */
    int bgft_loaded;             /* libSceBgft.sprx geladen */
    const char *bgft_path;       /* geladener Kandidat, NULL wenn keiner */
    int32_t bgft_result;         /* Ladecode des letzten Versuchs */
    const ps4u_diag_symbol *symbols;
    size_t symbol_count;
} ps4u_diag;

/* Legt "<dir>/.diagnostics.tmp" an, liest sie zurueck und entfernt sie
   wieder. Der Payload ruft das fuer GET /diagnostics auf; weil es reine
   Datei-Ein-/Ausgabe ist, deckt der Hosttest es gegen ein echtes Verzeichnis
   und einen fehlenden Ordner ab. out == NULL ist erlaubt. */
void ps4u_diag_write_test(const char *dir, ps4u_diag_write_result *out);

/* Baut {"ok":true,"version":…,"dir":…,"write_test":…,"pin":…,"app_inst":…,
   "bgft":{…,"symbols":[{"name":…,"result":…,"found":…}]}}. Liefert die
   benoetigte Laenge ohne NUL wie snprintf; ist der Puffer zu klein, wird
   abgeschnitten. diag == NULL liefert 0. */
size_t ps4u_diag_json(const ps4u_diag *diag, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
