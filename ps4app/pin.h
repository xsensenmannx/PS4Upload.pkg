/*
 * Vierstellige Upload-PIN.
 *
 * Frei von PS4-Abhaengigkeiten, damit die Logik auf dem PC testbar ist.
 * Die PIN steht auf dem Fernseher; der Browser schickt sie im Header
 * X-Upload-Pin. Fehlversuche werden pro Client-IP gezaehlt, damit ein
 * Geraet im LAN nicht alle anderen Uploads sperren kann.
 */

#pragma once

#include <string.h>

#define PIN_DIGITS 4
#define PIN_MAX_FAILURES 5
#define PIN_LOCK_MS (60ull * 1000ull)
#define PIN_CLIENT_CAPACITY 16
#define PIN_HEADER_NAME "X-Upload-Pin"

struct PinState
{
    char value[PIN_DIGITS + 1];
};

struct PinClientState
{
    char client[16];
    unsigned int failures;
    unsigned long long locked_until;
};

static void pin_format(PinState &state, unsigned int number)
{
    for (int index = PIN_DIGITS - 1; index >= 0; index--)
    {
        state.value[index] = (char)('0' + (number % 10u));
        number /= 10u;
    }
    state.value[PIN_DIGITS] = 0;
}

static void pin_generate(PinState &state, unsigned int entropy)
{
    unsigned int value = entropy != 0 ? entropy : 0x9e3779b9u;

    /* xorshift32: keine Krypto, aber die Sperre ist die eigentliche Bremse. */
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    value ^= value >> 11;

    pin_format(state, value % 10000u);
}

/* Vergleicht ohne fruehen Abbruch und verlangt genau vier Ziffern. */
static bool pin_equal(const char *expected, const char *given)
{
    unsigned int difference = 0;
    int length = 0;

    if (expected == 0 || given == 0)
    {
        return false;
    }

    while (given[length] != 0)
    {
        length++;
    }
    if (length != PIN_DIGITS)
    {
        return false;
    }

    for (int index = 0; index < PIN_DIGITS; index++)
    {
        difference |= (unsigned int)(expected[index] ^ given[index]);
    }

    return difference == 0;
}

/*
 * Liest den Wert eines X-Upload-Pin-Headers. Er muss genau vier Ziffern
 * enthalten; Leerzeichen vor/nach dem Wert sind erlaubt, weitere Zeichen
 * nicht.
 */
static bool pin_parse_value(const char *value, char *given, int capacity)
{
    int used = 0;

    if (given == 0 || capacity < PIN_DIGITS + 1)
    {
        return false;
    }
    given[0] = 0;
    if (value == 0)
    {
        return false;
    }

    while (*value == ' ' || *value == '\t')
    {
        value++;
    }
    while (*value >= '0' && *value <= '9' && used < capacity - 1)
    {
        given[used++] = *value++;
    }
    given[used] = 0;

    while (*value == ' ' || *value == '\t')
    {
        value++;
    }

    return used == PIN_DIGITS &&
           (*value == 0 || *value == '\r' || *value == '\n');
}

/*
 * Liefert den Zustand fuer eine Client-IP oder legt ihn in einem freien
 * Tabellenplatz an. Ist die kleine LAN-Tabelle voll, wird der Upload
 * abgewiesen, statt einen fremden Zustand zu ueberschreiben.
 */
static PinClientState *pin_client_state(PinClientState *states, int capacity,
                                        const char *client)
{
    int free_slot = -1;
    int length = 0;

    if (states == 0 || capacity <= 0 || client == 0 || client[0] == 0)
    {
        return 0;
    }

    for (int index = 0; index < capacity; index++)
    {
        if (states[index].client[0] == 0)
        {
            if (free_slot < 0)
            {
                free_slot = index;
            }
        }
        else if (strcmp(states[index].client, client) == 0)
        {
            return &states[index];
        }
    }

    if (free_slot < 0)
    {
        return 0;
    }

    while (client[length] != 0 &&
           length < (int)sizeof(states[free_slot].client) - 1)
    {
        states[free_slot].client[length] = client[length];
        length++;
    }
    if (client[length] != 0)
    {
        return 0;
    }
    states[free_slot].client[length] = 0;
    states[free_slot].failures = 0;
    states[free_slot].locked_until = 0;

    return &states[free_slot];
}

static bool pin_locked(const PinClientState &state, unsigned long long now_ms)
{
    return state.locked_until > now_ms;
}

static void pin_succeeded(PinClientState &state)
{
    state.failures = 0;
    state.locked_until = 0;
}

static void pin_fail(PinClientState &state, unsigned long long now_ms)
{
    state.failures++;

    if (state.failures >= PIN_MAX_FAILURES)
    {
        state.locked_until = now_ms + PIN_LOCK_MS;
    }
}
