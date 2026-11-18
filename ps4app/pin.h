/*
 * Vierstellige Upload-PIN.
 *
 * Frei von PS4-Abhaengigkeiten, damit die Logik auf dem PC testbar ist.
 * Die PIN steht auf dem Fernseher; der Browser muss sie beim Upload
 * mitschicken. Gegen Durchprobieren sperrt pin_fail() nach fuenf
 * Fehlversuchen fuer eine Minute.
 */

#pragma once

#define PIN_DIGITS 4
#define PIN_MAX_FAILURES 5
#define PIN_LOCK_MS (60ull * 1000ull)

struct PinState
{
    char value[PIN_DIGITS + 1];
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
    state.failures = 0;
    state.locked_until = 0;
}

/* Vergleicht ohne fruehen Abbruch und verlangt genau vier Ziffern. */
static bool pin_equal(const char *expected, const char *given)
{
    unsigned int difference = 0;
    int length = 0;

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

static bool pin_locked(const PinState &state, unsigned long long now_ms)
{
    return state.locked_until > now_ms;
}

static void pin_succeeded(PinState &state)
{
    state.failures = 0;
    state.locked_until = 0;
}

static void pin_fail(PinState &state, unsigned long long now_ms)
{
    state.failures++;

    if (state.failures >= PIN_MAX_FAILURES)
    {
        state.locked_until = now_ms + PIN_LOCK_MS;
    }
}
