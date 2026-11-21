#ifndef PS4U_STATUS_H
#define PS4U_STATUS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *version;   /* Pflicht, z. B. "0.1.0" */
    const char *phase;     /* idle, upload, register, install, done, error */
    const char *title;     /* optional */
    const char *error;     /* optional, nur bei phase=error */
    unsigned uptime_s;
    unsigned clients;
    unsigned queue;
    int percent;           /* wird auf 0..100 begrenzt */
    unsigned long long free_bytes;
} ps4u_status;

/* Schreibt den Status als JSON mit snake_case-Schlüsseln; Strings werden
   maskiert. Liefert die benötigte Länge ohne NUL – wie snprintf. Bei zu
   kleinem Puffer wird abgeschnitten und trotzdem die volle Länge gemeldet. */
size_t ps4u_status_json(const ps4u_status *status, char *out, size_t cap);

/* Maskiert ", \, \n, \r, \t und Steuerzeichen als \u00XX. Liefert die
   benötigte Länge ohne NUL. Auch von der Dateiliste (ps4u_files.h) genutzt. */
size_t ps4u_json_escape(const char *in, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
