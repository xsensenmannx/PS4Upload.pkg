/*
 * Begrenzte Stillstands-Erkennung fuer BGFT.
 *
 * Der Tracker speichert eine Signatur aus Fortschritt und Task-Zustand.
 * Erst eine Aenderung setzt den Zeitstempel zurueck; bleibt die Signatur
 * zu lange gleich, meldet check() einen Stillstand. Lange Beobachtungs-
 * luecken (App pausiert/blockiert) setzen die Messung neu auf.
 */

#pragma once

#include <stdint.h>

/* 180 s ohne jede Zustands-/Fortschrittsaenderung gelten als Stillstand. */
#define INSTALL_STALL_TIMEOUT_MS (180ull * 1000ull)

struct InstallStallTracker
{
    bool initialized;
    unsigned long long last_change_ms;
    unsigned long long last_seen_ms;
    uint64_t signature;
};

static inline uint64_t stall_mix(uint64_t hash, uint64_t value)
{
    hash ^= value;
    hash *= 1099511628211ull;
    return hash;
}

static inline void stall_tracker_reset(InstallStallTracker *tracker)
{
    if (tracker == 0)
    {
        return;
    }
    tracker->initialized = false;
    tracker->last_change_ms = 0;
    tracker->last_seen_ms = 0;
    tracker->signature = 0;
}

/*
 * Rueckgabe true bedeutet Stillstand. Der erste Aufruf initialisiert nur.
 * Ein Ruecksprung der Uhr (z. B. Neustart) initialisiert ebenfalls neu.
 */
static inline bool stall_tracker_check(InstallStallTracker *tracker,
                                uint64_t signature,
                                unsigned long long now_ms,
                                unsigned long long timeout_ms)
{
    if (tracker == 0)
    {
        return false;
    }

    /*
     * Eine lange Beobachtungsluecke (App pausiert oder blockiert) ist kein
     * Stillstand: Es gab in dieser Zeit einfach keine Polls. Erst nach dem
     * naechsten regulaeren Poll beginnt die Zeitmessung erneut.
     */
    if (timeout_ms != 0 && tracker->initialized && now_ms >= tracker->last_seen_ms &&
        now_ms - tracker->last_seen_ms >= timeout_ms)
    {
        tracker->last_change_ms = now_ms;
        tracker->last_seen_ms = now_ms;
        tracker->signature = signature;
        return false;
    }

    if (!tracker->initialized || now_ms < tracker->last_change_ms)
    {
        tracker->initialized = true;
        tracker->last_change_ms = now_ms;
        tracker->last_seen_ms = now_ms;
        tracker->signature = signature;
        return false;
    }

    if (signature != tracker->signature)
    {
        tracker->last_change_ms = now_ms;
        tracker->last_seen_ms = now_ms;
        tracker->signature = signature;
        return false;
    }

    tracker->last_seen_ms = now_ms;
    return now_ms - tracker->last_change_ms >= timeout_ms;
}
