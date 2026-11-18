/*
 * Minimale PKG-Pruefung ohne Sony-Abhaengigkeiten.
 *
 * Liest Kopfdaten und param.sfo aus einer PKG-Datei, damit vor der
 * Installation feststeht: Signatur stimmt, Groesse stimmt, Titel und
 * Content-ID sind bekannt. Laeuft auf der PS4 und auf dem PC (Host-Test).
 */

#pragma once

#include <stdio.h>
#include <string.h>
#include <stdint.h>

struct PkgInfo
{
    bool valid;
    char title[68];
    char title_id[16];
    char content_id[40];
    char app_ver[8];
    unsigned long long size;
    char error[72];
};

static unsigned int pkg_read_be32(const unsigned char *p)
{
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
           ((unsigned int)p[2] << 8) | (unsigned int)p[3];
}

static unsigned int pkg_read_le32(const unsigned char *p)
{
    return ((unsigned int)p[3] << 24) | ((unsigned int)p[2] << 16) |
           ((unsigned int)p[1] << 8) | (unsigned int)p[0];
}

static unsigned long long pkg_read_be64(const unsigned char *p)
{
    return ((unsigned long long)pkg_read_be32(p) << 32) | pkg_read_be32(p + 4);
}

static void pkg_copy(char *destination, int capacity, const char *source, int limit)
{
    int length = 0;

    while (length < limit && length < capacity - 1 && source[length] != 0)
    {
        destination[length] = source[length];
        length++;
    }
    destination[length] = 0;
}

/* "MM.mm" -> MM*100+mm; -1, wenn das Format nicht passt. */
static int pkg_version_value(const char *text)
{
    int major = 0;
    int minor = 0;
    int index = 0;

    if (text == 0)
    {
        return -1;
    }

    while (index < 2 && text[index] >= '0' && text[index] <= '9')
    {
        major = major * 10 + (text[index] - '0');
        index++;
    }
    if (index != 2 || text[2] != '.')
    {
        return -1;
    }

    index = 3;
    while (index < 5 && text[index] >= '0' && text[index] <= '9')
    {
        minor = minor * 10 + (text[index] - '0');
        index++;
    }
    if (index != 5 || text[5] != 0)
    {
        return -1;
    }

    return major * 100 + minor;
}

static void pkg_fail(PkgInfo *info, const char *message)
{
    info->valid = false;
    pkg_copy(info->error, sizeof(info->error), message, 200);
}

/* param.sfo: \0PSF, Kopf mit Tabellen-Offsets, Eintraege mit 16 Byte. */
static bool pkg_parse_sfo(const unsigned char *sfo, int size, PkgInfo *info)
{
    unsigned int key_table;
    unsigned int data_table;
    unsigned int count;

    if (size < 20 || memcmp(sfo, "\0PSF", 4) != 0)
    {
        return false;
    }

    key_table = pkg_read_le32(sfo + 8);
    data_table = pkg_read_le32(sfo + 12);
    count = pkg_read_le32(sfo + 16);
    if (count > 64)
    {
        count = 64;
    }

    for (unsigned int index = 0; index < count; index++)
    {
        const unsigned char *entry;
        unsigned int key_offset;
        unsigned int length;
        unsigned int data_offset;
        const char *key;
        const char *value;
        int available;

        if ((int)(20 + (index + 1) * 16) > size)
        {
            break;
        }

        entry = sfo + 20 + index * 16;
        key_offset = (unsigned int)entry[0] | ((unsigned int)entry[1] << 8);
        length = pkg_read_le32(entry + 4);
        data_offset = pkg_read_le32(entry + 12);

        if (key_table + key_offset + 12 >= (unsigned int)size)
        {
            continue;
        }
        if (data_table + data_offset >= (unsigned int)size)
        {
            continue;
        }

        key = (const char *)(sfo + key_table + key_offset);
        value = (const char *)(sfo + data_table + data_offset);
        available = size - (int)(data_table + data_offset);
        if ((int)length > available)
        {
            length = (unsigned int)available;
        }

        if (strncmp(key, "TITLE", 6) == 0 && key[5] == 0)
        {
            pkg_copy(info->title, sizeof(info->title), value, (int)length);
        }
        else if (strncmp(key, "TITLE_ID", 9) == 0 && key[8] == 0)
        {
            pkg_copy(info->title_id, sizeof(info->title_id), value, (int)length);
        }
        else if (strncmp(key, "CONTENT_ID", 11) == 0 && key[10] == 0)
        {
            pkg_copy(info->content_id, sizeof(info->content_id), value, (int)length);
        }
        else if (strncmp(key, "APP_VER", 8) == 0 && key[7] == 0)
        {
            pkg_copy(info->app_ver, sizeof(info->app_ver), value, (int)length);
        }
    }

    return info->title[0] != 0;
}

static bool pkg_probe(const char *path, PkgInfo *info)
{
    unsigned char header[0x440];
    unsigned long long declared;
    unsigned int entry_count;
    unsigned int table_offset;
    unsigned int sfo_offset = 0;
    unsigned int sfo_size = 0;
    bool found_sfo = false;
    long file_size;
    FILE *file;

    memset(info, 0, sizeof(*info));
    pkg_copy(info->error, sizeof(info->error), "unbekannt", 20);

    file = fopen(path, "rb");
    if (file == 0)
    {
        pkg_fail(info, "Datei nicht lesbar");
        return false;
    }

    fseek(file, 0, SEEK_END);
    file_size = ftell(file);
    if (file_size < (long)sizeof(header))
    {
        fclose(file);
        pkg_fail(info, "Datei zu klein fuer eine PKG");
        return false;
    }
    info->size = (unsigned long long)file_size;

    fseek(file, 0, SEEK_SET);
    if (fread(header, 1, sizeof(header), file) != sizeof(header))
    {
        fclose(file);
        pkg_fail(info, "Kopf nicht lesbar");
        return false;
    }

    if (memcmp(header, "\x7f" "CNT", 4) != 0)
    {
        fclose(file);
        pkg_fail(info, "keine PKG-Signatur (7f434e54)");
        return false;
    }

    pkg_copy(info->content_id, sizeof(info->content_id), (const char *)(header + 0x40), 36);

    declared = pkg_read_be64(header + 0x430);
    if (declared != info->size)
    {
        fclose(file);
        pkg_fail(info, "Header-Groesse passt nicht zur Datei");
        return false;
    }

    entry_count = pkg_read_be32(header + 0x10);
    table_offset = pkg_read_be32(header + 0x18);
    if (entry_count == 0 || entry_count > 64)
    {
        fclose(file);
        pkg_fail(info, "Eintragstabelle unbrauchbar");
        return false;
    }

    for (unsigned int index = 0; index < entry_count; index++)
    {
        unsigned char entry[32];

        if (fseek(file, (long)(table_offset + index * 32), SEEK_SET) != 0)
        {
            break;
        }
        if (fread(entry, 1, sizeof(entry), file) != sizeof(entry))
        {
            break;
        }
        if (pkg_read_be32(entry) == 0x1000)
        {
            sfo_offset = pkg_read_be32(entry + 16);
            sfo_size = pkg_read_be32(entry + 20);
            found_sfo = true;
            break;
        }
    }

    if (!found_sfo)
    {
        fclose(file);
        pkg_fail(info, "param.sfo fehlt");
        return false;
    }

    if (sfo_size == 0 || sfo_size > 8192 ||
        (unsigned long long)sfo_offset + sfo_size > info->size)
    {
        fclose(file);
        pkg_fail(info, "param.sfo zeigt aus der Datei heraus");
        return false;
    }

    {
        static unsigned char sfo[8192];

        if (fseek(file, (long)sfo_offset, SEEK_SET) != 0 ||
            fread(sfo, 1, sfo_size, file) != sfo_size)
        {
            fclose(file);
            pkg_fail(info, "param.sfo nicht lesbar");
            return false;
        }
        fclose(file);

        if (!pkg_parse_sfo(sfo, (int)sfo_size, info))
        {
            pkg_fail(info, "param.sfo ohne Titel");
            return false;
        }
    }

    if (info->content_id[0] == 0)
    {
        pkg_fail(info, "CONTENT_ID fehlt");
        return false;
    }

    info->valid = true;
    info->error[0] = 0;
    return true;
}
