/*
 * Grenzen und Pruefungen fuer den Upload.
 *
 * Bewusst ohne PS4-Abhaengigkeiten, damit die Logik auf dem PC testbar ist.
 * Die eigentliche Dateisystem- und Socket-Arbeit bleibt in main.cpp.
 */

#pragma once

#include <stdbool.h>

/* Ein Body, der so lange keine Bytes mehr liefert, gilt als haengen. */
#define UPLOAD_IDLE_TIMEOUT_MS (30ull * 1000ull)

/* Reserve fuer System, Logs und den naechsten Start; nicht fuer PKGs gedacht. */
#define UPLOAD_SPACE_RESERVE_BYTES (64ull * 1024ull * 1024ull)

/* BGFT kopiert die lokale PKG noch einmal nach /user/app. */
#define UPLOAD_BGFT_COPIES 2ull

static bool upload_idle_expired(unsigned long long now_ms,
                                unsigned long long last_io_ms,
                                unsigned long long timeout_ms)
{
    if (last_io_ms == 0 || now_ms < last_io_ms)
    {
        return false;
    }
    return now_ms - last_io_ms >= timeout_ms;
}

/* Content-Length muss in den freien Platz passen, ohne die Reserve anzutasten. */
static bool upload_space_ok(unsigned long long content_length,
                            unsigned long long free_bytes,
                            unsigned long long reserve_bytes)
{
    if (content_length > free_bytes)
    {
        return false;
    }
    return free_bytes - content_length >= reserve_bytes;
}

/*
 * BGFT legt waehrend der Installation eine zweite Kopie an. Der Upload muss
 * also Platz fuer zwei PKG-Kopien plus die Systemreserve lassen; die
 * Division vermeidet einen Ueberlauf bei absichtlich riesigen Headern.
 */
static bool upload_bgft_space_ok(unsigned long long content_length,
                                 unsigned long long free_bytes,
                                 unsigned long long reserve_bytes)
{
    unsigned long long usable;

    if (free_bytes < reserve_bytes)
    {
        return false;
    }
    usable = free_bytes - reserve_bytes;
    return content_length <= usable / UPLOAD_BGFT_COPIES;
}

/*
 * Dasselbe fuer einen Resume: die on_disk_bytes der .part-Datei liegen schon
 * auf der Platte und fehlen deshalb in free_bytes. Ohne sie wieder
 * dazuzurechnen, wuerde ein fortgesetzter Upload an der Reserve scheitern,
 * obwohl nach dem Umbenennen genauso viel Platz frei ist wie bei einem
 * Upload in einem Zug.
 */
static bool upload_bgft_space_ok_resume(unsigned long long content_length,
                                        unsigned long long free_bytes,
                                        unsigned long long on_disk_bytes,
                                        unsigned long long reserve_bytes)
{
    unsigned long long available = free_bytes + on_disk_bytes;

    if (available < free_bytes)
    {
        available = ~(unsigned long long)0;
    }
    return upload_bgft_space_ok(content_length, available, reserve_bytes);
}
