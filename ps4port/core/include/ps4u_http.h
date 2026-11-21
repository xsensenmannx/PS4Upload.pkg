#ifndef PS4U_HTTP_H
#define PS4U_HTTP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Obergrenze fuer Content-Length und Resume-Offset: 64 GiB. */
#define PS4U_UPLOAD_MAX_BYTES (64ull * 1024ull * 1024ull * 1024ull)

/* Groesster Kopfblock, den der Payload annimmt (Anfragezeile + Header).
   Zugleich die Groesse seines Anfragepuffers. */
#define PS4U_HEAD_MAX 2048

typedef enum {
    PS4U_METHOD_GET = 0,
    PS4U_METHOD_HEAD,
    PS4U_METHOD_POST,
    PS4U_METHOD_DELETE,
    PS4U_METHOD_OTHER
} ps4u_method;

typedef enum {
    PS4U_ROUTE_ROOT = 0,
    PS4U_ROUTE_STATUS,
    PS4U_ROUTE_UPLOAD,
    PS4U_ROUTE_FILES,
    PS4U_ROUTE_DIAGNOSTICS,
    PS4U_ROUTE_UNKNOWN
} ps4u_route;

typedef struct {
    ps4u_method method;
    ps4u_route route;
    char name[128];            /* leer, wenn kein name-Parameter dabei ist */
    unsigned long long offset; /* 0, wenn kein offset-Parameter dabei ist */
} ps4u_request;

/* Laenge des Kopfblocks inklusive des abschliessenden "\r\n\r\n". Liefert 0,
   solange der Block unvollstaendig ist (oder data NULL ist). Der Body hinter
   dem Block zaehlt nicht mit; so bleibt ein im selben Paket empfangener Body
   beim Zerlegen unberuehrt. */
size_t ps4u_header_block_length(const char *data, size_t len);

/* Parst den Kopfblock, den ps4u_header_block_length geliefert hat; block_len
   ist dessen Laenge inklusive "\r\n\r\n", der Body dahinter stoert nicht.
   Liefert 0 bei Erfolg, -1 wenn der Block nicht auf "\r\n\r\n" endet, nicht
   in PS4U_HEAD_MAX passt oder die Anfragezeile unbrauchbar ist. */
int ps4u_parse_head(const char *data, size_t block_len, ps4u_request *out);

/* Liest Content-Length aus einem Kopfblock; hoechstens PS4U_UPLOAD_MAX_BYTES.
   0 = ok, -1 = fehlt, keine reine Ziffernfolge oder zu gross. */
int ps4u_content_length(const char *head, unsigned long long *out);

/* Sucht einen Header ohne Rücksicht auf Groß-/Kleinschreibung in einem rohen
   Headerblock ("Name: Wert\r\n..."). Kopiert den getrimmten Wert; liefert 0,
   wenn er gefunden wurde und vollständig hineinpasst, sonst -1. */
int ps4u_header_value(const char *headers, const char *name, char *out, size_t cap);

/* Akzeptiert nur einen einfachen .pkg-Dateinamen: keine Pfadtrenner, kein
   führender Punkt, keine Steuerzeichen, Endung .pkg (Groß-/Kleinschreibung
   egal). Liefert 0, wenn der Name sicher ist. */
int ps4u_safe_pkg_name(const char *name);

/* 1 fuer Dateien, die GET /files auflistet: ein sicherer .pkg-Name oder sein
   Resume-Rest "<name>.part". pin.txt, config.txt und das Log bleiben aussen
   vor. */
int ps4u_listable_file(const char *name);

/* Was ein POST auf /upload mit diesem offset tun soll. */
typedef enum {
    PS4U_RESUME_APPEND = 0,  /* an die .part-Datei anhaengen (oder neu beginnen) */
    PS4U_RESUME_FINALIZE,    /* .part ist vollstaendig: pruefen und umbenennen */
    PS4U_RESUME_ALREADY,     /* <name> liegt schon mit genau offset Bytes vor */
    PS4U_RESUME_CONFLICT     /* .part passt nicht zu offset */
} ps4u_resume_plan;

/* Entscheidet anhand der Groessen von Zieldatei und .part-Rest. offset 0
   beginnt immer neu (die .part-Datei wird dabei abgeschnitten). Laenge 0 mit
   passendem .part schliesst nur noch ab; fehlt der Rest, aber die fertige
   Datei hat genau offset Bytes, ist der Upload bereits durch (z. B. nach einer
   verlorenen Antwort). Alles andere ist ein Konflikt. */
ps4u_resume_plan ps4u_resume_plan_for(unsigned long long offset,
                                      unsigned long long length,
                                      unsigned long long part_bytes,
                                      unsigned long long dest_bytes);

/* Offset fuer HEAD /upload: der .part-Rest hat Vorrang, sonst die fertige
   Datei mit *complete = 1. complete darf NULL sein. */
unsigned long long ps4u_resume_offset(unsigned long long part_bytes,
                                      unsigned long long dest_bytes,
                                      int *complete);

/* 200 für eine bekannte Route mit erlaubter Methode, 405 für die falsche
   Methode, 404 für eine unbekannte Route. /upload erlaubt POST (schreiben),
   HEAD (Offset abfragen) und DELETE (aufraeumen), /files und /diagnostics nur
   GET. */
int ps4u_route_status(ps4u_route route, ps4u_method method);

/* Schreibt Statuszeile und Headerblock (bis zum leeren \r\n) nach out und
   haengt extra (rohe "Name: Wert\r\n"-Zeilen) vor Cache-Control ein. extra
   darf NULL sein. Liefert die benoetigte Laenge ohne NUL wie snprintf; ist
   der Puffer zu klein, wird abgeschnitten. Body-Laenge muss vorher
   feststehen. */
size_t ps4u_response_head_extra(int status, const char *content_type,
                                unsigned long long body_len, const char *extra,
                                char *out, size_t cap);

/* Kanonische Textantwort fuer 400, 404 und 405 (andere Codes: "error"). */
size_t ps4u_error_response(int status, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
