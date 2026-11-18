/*
 * Kernel-Log-Ausgaben mit festem Praefix "[ps4upload] ".
 *
 * Bewusst ohne ui.h: die Helfer brauchen nur libkernel und werden von
 * main.cpp und install.h gemeinsam genutzt, damit Hex-/Zahlenformatierung
 * nur an einer Stelle steht.
 */

#pragma once

#include <stddef.h>

#include <orbis/libkernel.h>

static int diag_append(char *buffer, int capacity, int used, const char *text)
{
    while (*text != 0 && used < capacity - 1)
    {
        buffer[used++] = *text++;
    }
    buffer[used] = 0;
    return used;
}

static int diag_uint(char *buffer, int capacity, int used, unsigned int value)
{
    char digits[12];
    int count = 0;

    if (value == 0)
    {
        digits[count++] = '0';
    }
    while (value > 0 && count < (int)sizeof(digits))
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

static int diag_hex(char *buffer, int capacity, int used, unsigned int value)
{
    static const char digits[] = "0123456789abcdef";

    used = diag_append(buffer, capacity, used, "0x");
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

static void diag_line(const char *text)
{
    sceKernelDebugOutText(0, text);
}

static void diag_text(const char *text)
{
    char line[192];
    int used = 0;

    used = diag_append(line, sizeof(line), used, "[ps4upload] ");
    used = diag_append(line, sizeof(line), used, text);
    diag_append(line, sizeof(line), used, "\n");
    diag_line(line);
}

/* "[ps4upload] label = 0x........" */
static void diag_code(const char *label, int value)
{
    char line[176];
    int used = 0;

    used = diag_append(line, sizeof(line), used, "[ps4upload] ");
    used = diag_append(line, sizeof(line), used, label);
    used = diag_append(line, sizeof(line), used, " = ");
    used = diag_hex(line, sizeof(line), used, (unsigned int)value);
    diag_append(line, sizeof(line), used, "\n");
    diag_line(line);
}

/* "[ps4upload] label = 123" */
static void diag_number(const char *label, unsigned int value)
{
    char line[144];
    int used = 0;

    used = diag_append(line, sizeof(line), used, "[ps4upload] ");
    used = diag_append(line, sizeof(line), used, label);
    used = diag_append(line, sizeof(line), used, " = ");
    used = diag_uint(line, sizeof(line), used, value);
    diag_append(line, sizeof(line), used, "\n");
    diag_line(line);
}

/* "[ps4upload] label = N MiB" - size_t, weil Direktspeicher > 4 GiB sein kann. */
static void diag_size(const char *label, size_t bytes)
{
    char line[160];
    int used = 0;

    used = diag_append(line, sizeof(line), used, "[ps4upload] ");
    used = diag_append(line, sizeof(line), used, label);
    used = diag_append(line, sizeof(line), used, " = ");
    used = diag_uint(line, sizeof(line), used, (unsigned int)(bytes / 1048576u));
    diag_append(line, sizeof(line), used, " MiB\n");
    diag_line(line);
}
