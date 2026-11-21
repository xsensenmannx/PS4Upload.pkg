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
#include <stddef.h>

struct PkgInfo
{
    bool valid;
    char title[68];
    char title_id[16];
    char content_id[40];
    char app_ver[8];
    char category[8];
    uint32_t content_type;
    unsigned long long size;
    char error[72];
};

/*
 * Der Content-Type im PKG-Header bestimmt den BGFT-Pakettyp. CATEGORY allein
 * reicht nicht: Ein normaler Patch (gp) hat 0x1A/PS4GD, nur ein Delta-Patch
 * hat 0x1E/PS4DP. packageSubType bleibt leer, wie in funktionierenden
 * lokalen BGFT-Beispielen.
 */
static const char *pkg_bgft_package_type(uint32_t content_type)
{
    switch (content_type)
    {
        case 0x1A: return "PS4GD";
        case 0x1B: return "PS4AC";
        case 0x1C: return "PS4AL";
        case 0x1E: return "PS4DP";
        default: return 0;
    }
}

/* Die App nimmt nur Basis, Update und DLC an. */
static bool pkg_category_supported(const char *category)
{
    return category != 0 &&
           (strcmp(category, "gd") == 0 ||
            strcmp(category, "gp") == 0 ||
            strcmp(category, "ac") == 0);
}

static uint32_t pkg_read_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t pkg_read_le32(const unsigned char *p)
{
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[1] << 8) | (uint32_t)p[0];
}

static uint16_t pkg_read_le16(const unsigned char *p)
{
    return (uint16_t)((uint16_t)p[1] << 8) | (uint16_t)p[0];
}

static uint64_t pkg_read_be64(const unsigned char *p)
{
    return ((uint64_t)pkg_read_be32(p) << 32) | pkg_read_be32(p + 4);
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

static bool pkg_is_upper(char value)
{
    return value >= 'A' && value <= 'Z';
}

static bool pkg_is_digit(char value)
{
    return value >= '0' && value <= '9';
}

/* PS4-Title-ID: vier Grossbuchstaben und fuenf Ziffern, exakt neun Zeichen. */
static bool pkg_valid_title_id(const char *text)
{
    if (text == 0)
    {
        return false;
    }

    for (int index = 0; index < 4; index++)
    {
        if (!pkg_is_upper(text[index]))
        {
            return false;
        }
    }
    for (int index = 4; index < 9; index++)
    {
        if (!pkg_is_digit(text[index]))
        {
            return false;
        }
    }

    return text[9] == 0;
}

/*
 * Content-ID: 36 Zeichen, z. B.
 * IV0000-BREW00090_00-PS4UPLOAD0000000
 * Die neun Zeichen nach dem ersten Bindestrich muessen die TITLE_ID sein.
 */
static bool pkg_valid_content_id(const char *text)
{
    if (text == 0)
    {
        return false;
    }

    if (!pkg_is_upper(text[0]) || !pkg_is_upper(text[1]))
    {
        return false;
    }
    for (int index = 2; index < 6; index++)
    {
        if (!pkg_is_digit(text[index]))
        {
            return false;
        }
    }
    if (text[6] != '-')
    {
        return false;
    }
    for (int index = 7; index < 11; index++)
    {
        if (!pkg_is_upper(text[index]))
        {
            return false;
        }
    }
    for (int index = 11; index < 16; index++)
    {
        if (!pkg_is_digit(text[index]))
        {
            return false;
        }
    }
    if (text[16] != '_' || !pkg_is_digit(text[17]) || !pkg_is_digit(text[18]) ||
        text[19] != '-')
    {
        return false;
    }
    for (int index = 20; index < 36; index++)
    {
        if (!pkg_is_upper(text[index]) && !pkg_is_digit(text[index]))
        {
            return false;
        }
    }

    return text[36] == 0;
}

static bool pkg_content_id_matches_title_id(const char *content_id, const char *title_id)
{
    return pkg_valid_content_id(content_id) &&
           pkg_valid_title_id(title_id) &&
           memcmp(content_id + 7, title_id, 9) == 0;
}

/*
 * Kopiert einen SFO-String nur, wenn er exakt mit NUL endet. Der
 * length-Wert aus dem SFO-Eintrag muss also das abschliessende NUL enthalten.
 */
static bool pkg_copy_sfo_string(const unsigned char *value, uint64_t length,
                                char *destination, size_t capacity)
{
    if (value == 0 || destination == 0 || length == 0 || capacity == 0)
    {
        return false;
    }

    for (uint64_t index = 0; index < length; index++)
    {
        if (value[index] == 0)
        {
            if (index + 1 != length || index >= capacity)
            {
                return false;
            }
            memcpy(destination, value, (size_t)index);
            destination[index] = 0;
            return true;
        }
    }

    return false;
}

static bool pkg_key_equals(const unsigned char *key, uint64_t key_length,
                           const char *expected)
{
    size_t expected_length = strlen(expected);
    return key_length == expected_length &&
           memcmp(key, expected, expected_length) == 0;
}

static void pkg_fail(PkgInfo *info, const char *message)
{
    info->valid = false;
    pkg_copy(info->error, sizeof(info->error), message, 200);
}

/* param.sfo: \0PSF, Kopf mit Tabellen-Offsets, Eintraege mit 16 Byte. */
static bool pkg_parse_sfo(const unsigned char *sfo, uint64_t size, PkgInfo *info)
{
    uint64_t key_table;
    uint64_t data_table;
    uint64_t count;
    uint64_t index_end;
    bool has_title = false;
    bool has_title_id = false;
    bool has_content_id = false;
    bool has_app_ver = false;
    bool has_category = false;

    if (sfo == 0 || size < 20 || memcmp(sfo, "\0PSF", 4) != 0)
    {
        pkg_fail(info, "param.sfo Aufbau ungueltig");
        return false;
    }

    key_table = pkg_read_le32(sfo + 8);
    data_table = pkg_read_le32(sfo + 12);
    count = pkg_read_le32(sfo + 16);

    if (count == 0 || count > 64)
    {
        pkg_fail(info, "param.sfo Eintraege ungueltig");
        return false;
    }

    /* Alle Offsets und Groessen in 64 Bit rechnen, bevor Zeiger entstehen. */
    index_end = 20ull + count * 16ull;
    if (index_end > size ||
        key_table < index_end || key_table > size ||
        data_table < key_table || data_table > size)
    {
        pkg_fail(info, "param.sfo Tabellen ungueltig");
        return false;
    }

    for (uint64_t index = 0; index < count; index++)
    {
        uint64_t entry;
        uint64_t key_offset;
        uint64_t data_offset;
        uint64_t length;
        uint64_t key_position;
        uint64_t data_position;
        uint64_t key_length = 0;
        const unsigned char *key;
        const unsigned char *value;

        entry = 20ull + index * 16ull;
        key_offset = pkg_read_le16(sfo + entry);
        length = pkg_read_le32(sfo + entry + 4);
        data_offset = pkg_read_le32(sfo + entry + 12);

        key_position = key_table + key_offset;
        data_position = data_table + data_offset;

        if (key_position >= size)
        {
            pkg_fail(info, "param.sfo Schluessel ungueltig");
            return false;
        }

        while (key_position + key_length < size &&
               sfo[key_position + key_length] != 0)
        {
            key_length++;
        }
        if (key_position + key_length >= size)
        {
            pkg_fail(info, "param.sfo Schluessel ohne NUL");
            return false;
        }
        if (key_position >= data_table ||
            key_length > data_table - key_position - 1)
        {
            pkg_fail(info, "param.sfo Schluessel überlappt Daten");
            return false;
        }

        if (data_position > size || length > size - data_position)
        {
            pkg_fail(info, "param.sfo Wert ungueltig");
            return false;
        }

        key = sfo + key_position;
        value = sfo + data_position;

        if (pkg_key_equals(key, key_length, "TITLE"))
        {
            if (!pkg_copy_sfo_string(value, length, info->title, sizeof(info->title)))
            {
                pkg_fail(info, "TITLE ungueltig");
                return false;
            }
            has_title = true;
        }
        else if (pkg_key_equals(key, key_length, "TITLE_ID"))
        {
            if (!pkg_copy_sfo_string(value, length, info->title_id, sizeof(info->title_id)))
            {
                pkg_fail(info, "TITLE_ID ungueltig");
                return false;
            }
            has_title_id = true;
        }
        else if (pkg_key_equals(key, key_length, "CONTENT_ID"))
        {
            if (!pkg_copy_sfo_string(value, length, info->content_id, sizeof(info->content_id)))
            {
                pkg_fail(info, "CONTENT_ID ungueltig");
                return false;
            }
            has_content_id = true;
        }
        else if (pkg_key_equals(key, key_length, "APP_VER"))
        {
            if (!pkg_copy_sfo_string(value, length, info->app_ver, sizeof(info->app_ver)))
            {
                pkg_fail(info, "APP_VER ungueltig");
                return false;
            }
            has_app_ver = true;
        }
        else if (pkg_key_equals(key, key_length, "CATEGORY"))
        {
            if (!pkg_copy_sfo_string(value, length, info->category, sizeof(info->category)))
            {
                pkg_fail(info, "CATEGORY ungueltig");
                return false;
            }
            has_category = true;
        }
    }

    if (!has_title || !has_title_id || !has_content_id || !has_app_ver || !has_category)
    {
        pkg_fail(info, "param.sfo Pflichtfeld fehlt");
        return false;
    }
    if (info->title[0] == 0)
    {
        pkg_fail(info, "TITLE fehlt");
        return false;
    }
    if (!pkg_valid_title_id(info->title_id))
    {
        pkg_fail(info, "TITLE_ID ungueltig");
        return false;
    }
    if (!pkg_valid_content_id(info->content_id))
    {
        pkg_fail(info, "CONTENT_ID ungueltig");
        return false;
    }
    if (pkg_version_value(info->app_ver) < 0)
    {
        pkg_fail(info, "APP_VER ungueltig");
        return false;
    }
    if (!pkg_category_supported(info->category))
    {
        pkg_fail(info, "CATEGORY nicht unterstuetzt");
        return false;
    }
    if (!pkg_content_id_matches_title_id(info->content_id, info->title_id))
    {
        pkg_fail(info, "CONTENT_ID passt nicht zur TITLE_ID");
        return false;
    }

    return true;
}

static bool pkg_probe(const char *path, PkgInfo *info)
{
    unsigned char header[0x440];
    unsigned char sfo[8192];
    char header_content_id[40];
    uint64_t declared;
    uint64_t entry_count;
    uint64_t table_offset;
    uint64_t sfo_offset = 0;
    uint64_t sfo_size = 0;
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

    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        pkg_fail(info, "Dateigroesse nicht lesbar");
        return false;
    }
    file_size = ftell(file);
    if (file_size < (long)sizeof(header))
    {
        fclose(file);
        pkg_fail(info, "Datei zu klein fuer eine PKG");
        return false;
    }
    info->size = (unsigned long long)file_size;

    if (fseek(file, 0, SEEK_SET) != 0 ||
        fread(header, 1, sizeof(header), file) != sizeof(header))
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

    pkg_copy(header_content_id, sizeof(header_content_id),
             (const char *)(header + 0x40), 36);
    if (!pkg_valid_content_id(header_content_id))
    {
        fclose(file);
        pkg_fail(info, "Header-Content-ID ungueltig");
        return false;
    }
    pkg_copy(info->content_id, sizeof(info->content_id), header_content_id, 36);
    info->content_type = pkg_read_be32(header + 0x74);

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

    for (uint64_t index = 0; index < entry_count; index++)
    {
        unsigned char entry[32];
        uint64_t entry_position = table_offset + index * 32ull;

        if (entry_position > info->size || info->size - entry_position < sizeof(entry))
        {
            break;
        }
        if (fseek(file, (long)entry_position, SEEK_SET) != 0)
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

    if (sfo_size == 0 || sfo_size > sizeof(sfo) ||
        sfo_offset > info->size || sfo_size > info->size - sfo_offset)
    {
        fclose(file);
        pkg_fail(info, "param.sfo zeigt aus der Datei heraus");
        return false;
    }

    if (fseek(file, (long)sfo_offset, SEEK_SET) != 0 ||
        fread(sfo, 1, (size_t)sfo_size, file) != sfo_size)
    {
        fclose(file);
        pkg_fail(info, "param.sfo nicht lesbar");
        return false;
    }
    fclose(file);

    if (!pkg_parse_sfo(sfo, sfo_size, info))
    {
        if (strcmp(info->error, "unbekannt") == 0)
        {
            pkg_fail(info, "param.sfo ungueltig");
        }
        return false;
    }

    if (strcmp(info->content_id, header_content_id) != 0)
    {
        pkg_fail(info, "Content-ID passt nicht zum Header");
        return false;
    }
    if (pkg_bgft_package_type(info->content_type) == 0)
    {
        pkg_fail(info, "PKG-Content-Type nicht unterstuetzt");
        return false;
    }

    info->valid = true;
    info->error[0] = 0;
    return true;
}
