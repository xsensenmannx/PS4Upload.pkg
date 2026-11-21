#ifndef PS4U_FILES_H
#define PS4U_FILES_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    unsigned long long bytes;
} ps4u_file_entry;

/* Liest die Quote aus einer config.txt: Zeilen "schluessel=wert", '#' beginnt
   einen Kommentar, Leerzeilen sind erlaubt. Erkannt wird nur "quota_bytes";
   fehlt der Schluessel, ist der Wert 0 oder unbrauchbar, gibt es keine Quote
   (Rueckgabe 0 = unbegrenzt). Mehrere Zeilen: die letzte gueltige gewinnt. */
unsigned long long ps4u_config_quota(const char *text);

/* 1, wenn add Bytes bei used noch in die Quote passen. quota == 0 heisst
   unbegrenzt. */
int ps4u_quota_ok(unsigned long long used, unsigned long long add,
                  unsigned long long quota);

/* Baut {"ok":true,"used":…,"quota":…,"free_bytes":…,"files":[…]} mit
   {"name":…,"bytes":…} je Eintrag. Liefert die benoetigte Laenge ohne NUL wie
   snprintf; ist der Puffer zu klein, wird abgeschnitten. */
size_t ps4u_files_json(const ps4u_file_entry *files, size_t count,
                       unsigned long long used, unsigned long long quota,
                       unsigned long long free_bytes, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
