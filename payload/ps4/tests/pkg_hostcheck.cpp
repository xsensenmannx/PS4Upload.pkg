/*
 * Host-Test der PKG-Pruefung aus ps4app/pkg.h.
 *
 * Baut im uebergebenen Arbeitsverzeichnis eine minimale PKG (Kopf,
 * Eintragstabelle mit param.sfo) und prueft zweierlei: eine stimmige Datei
 * wird angenommen und ihre Felder gelesen; jede gezielte Manipulation wird
 * abgelehnt und der Grund benannt.
 *
 * Aufruf: pkg_hostcheck <arbeitsverzeichnis>
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pkg.h"

namespace {

const char kContentId[] = "IV0000-BREW00090_00-PS4UPLOAD0000000";
const char kTitle[] = "PS4Upload Hosttest";
const char kTitleId[] = "BREW00090";

/* Aufbau, den pkg_probe erwartet (alle Kopffelder big endian). */
const std::size_t kEntryTable = 0x440;
const std::size_t kEntrySize = 32;
const std::size_t kSfoOffset = kEntryTable + kEntrySize;
const std::size_t kEntryCountOffset = 0x10;
const std::size_t kTableOffsetOffset = 0x18;
const std::size_t kContentIdOffset = 0x40;
const std::size_t kContentIdLength = 36;
const std::size_t kContentTypeOffset = 0x74;
const std::size_t kDeclaredSizeOffset = 0x430;

const std::uint32_t kSfoEntryId = 0x1000;
const std::uint32_t kContentTypeBase = 0x1A;

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const std::string &label)
{
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", label.c_str());
    g_checks++;
    if (!condition)
    {
        g_failures++;
    }
}

struct Options
{
    const char *magic = "\x7f" "CNT";
    const char *header_content_id = kContentId;
    const char *title = kTitle;
    const char *title_id = kTitleId;
    const char *content_id = kContentId;
    const char *app_ver = "01.00";
    const char *category = "gd";
    bool with_category = true;
    bool break_sfo_magic = false;
    std::uint32_t entry_id = kSfoEntryId;
    std::uint32_t entry_count = 1;
    std::uint32_t content_type = kContentTypeBase;
    std::uint32_t sfo_size_extra = 0;
    std::uint64_t declared_extra = 0;
};

void put_le16(std::vector<unsigned char> &data, std::size_t offset, std::uint16_t value)
{
    data[offset] = (unsigned char)(value & 0xffu);
    data[offset + 1] = (unsigned char)((value >> 8) & 0xffu);
}

void put_le32(std::vector<unsigned char> &data, std::size_t offset, std::uint32_t value)
{
    for (int index = 0; index < 4; index++)
    {
        data[offset + (std::size_t)index] = (unsigned char)((value >> (8 * index)) & 0xffu);
    }
}

void put_be32(std::vector<unsigned char> &data, std::size_t offset, std::uint32_t value)
{
    for (int index = 0; index < 4; index++)
    {
        data[offset + (std::size_t)index] =
            (unsigned char)((value >> (8 * (3 - index))) & 0xffu);
    }
}

void put_be64(std::vector<unsigned char> &data, std::size_t offset, std::uint64_t value)
{
    for (int index = 0; index < 8; index++)
    {
        data[offset + (std::size_t)index] =
            (unsigned char)((value >> (8 * (7 - index))) & 0xffu);
    }
}

void put_text(std::vector<unsigned char> &data, std::size_t offset, const char *text,
              std::size_t capacity)
{
    std::size_t length = std::strlen(text);

    if (length > capacity)
    {
        length = capacity;
    }
    std::memcpy(&data[offset], text, length);
}

/* param.sfo: \0PSF, Kopf mit Tabellen-Offsets, Eintraege mit 16 Byte. */
std::vector<unsigned char> build_sfo(const Options &options)
{
    struct Field
    {
        const char *key;
        const char *value;
    };

    std::vector<Field> fields;
    fields.push_back({ "TITLE", options.title });
    fields.push_back({ "TITLE_ID", options.title_id });
    fields.push_back({ "CONTENT_ID", options.content_id });
    fields.push_back({ "APP_VER", options.app_ver });
    if (options.with_category)
    {
        fields.push_back({ "CATEGORY", options.category });
    }

    const std::size_t count = fields.size();
    const std::uint32_t key_table = (std::uint32_t)(20 + count * 16);
    std::string keys;
    std::string values;
    std::vector<std::uint32_t> key_offsets;
    std::vector<std::uint32_t> data_offsets;
    std::vector<std::uint32_t> lengths;

    for (const Field &field : fields)
    {
        key_offsets.push_back((std::uint32_t)keys.size());
        keys.append(field.key);
        keys.push_back('\0');
        data_offsets.push_back((std::uint32_t)values.size());
        lengths.push_back((std::uint32_t)std::strlen(field.value) + 1);
        values.append(field.value);
        values.push_back('\0');
    }

    const std::uint32_t data_table = key_table + (std::uint32_t)keys.size();
    std::vector<unsigned char> sfo(20 + count * 16 + keys.size() + values.size(), 0);

    std::memcpy(&sfo[0], "\0PSF", 4);
    put_le32(sfo, 8, key_table);
    put_le32(sfo, 12, data_table);
    put_le32(sfo, 16, (std::uint32_t)count);

    for (std::size_t index = 0; index < count; index++)
    {
        const std::size_t entry = 20 + index * 16;

        put_le16(sfo, entry, (std::uint16_t)key_offsets[index]);
        put_le16(sfo, entry + 2, 0x0204); /* Format Utf8, von pkg.h nicht geprueft */
        put_le32(sfo, entry + 4, lengths[index]);
        put_le32(sfo, entry + 8, lengths[index]);
        put_le32(sfo, entry + 12, data_offsets[index]);
    }

    std::memcpy(&sfo[20 + count * 16], keys.data(), keys.size());
    std::memcpy(&sfo[data_table], values.data(), values.size());

    if (options.break_sfo_magic)
    {
        sfo[3] = 'X';
    }
    return sfo;
}

std::vector<unsigned char> build_package(const Options &options)
{
    std::vector<unsigned char> sfo = build_sfo(options);
    const std::size_t total = kSfoOffset + sfo.size();
    std::vector<unsigned char> pkg(total, 0);

    std::memcpy(&pkg[0], options.magic, 4);
    put_be32(pkg, kEntryCountOffset, options.entry_count);
    put_be32(pkg, kTableOffsetOffset, (std::uint32_t)kEntryTable);
    put_text(pkg, kContentIdOffset, options.header_content_id, kContentIdLength);
    put_be32(pkg, kContentTypeOffset, options.content_type);
    put_be64(pkg, kDeclaredSizeOffset, (std::uint64_t)total + options.declared_extra);

    put_be32(pkg, kEntryTable, options.entry_id);
    put_be32(pkg, kEntryTable + 16, (std::uint32_t)kSfoOffset);
    put_be32(pkg, kEntryTable + 20,
             (std::uint32_t)sfo.size() + options.sfo_size_extra);

    std::memcpy(&pkg[kSfoOffset], sfo.data(), sfo.size());
    return pkg;
}

bool write_file(const std::string &path, const std::vector<unsigned char> &data)
{
    FILE *file = std::fopen(path.c_str(), "wb");

    if (file == 0)
    {
        return false;
    }
    const std::size_t written = std::fwrite(data.data(), 1, data.size(), file);
    std::fclose(file);
    return written == data.size();
}

void expect_reject(const std::string &dir, const char *name, const Options &options,
                   const char *needle)
{
    const std::string path = dir + "/" + name + ".pkg";
    PkgInfo info;
    const bool accepted = write_file(path, build_package(options)) &&
                          pkg_probe(path.c_str(), &info);
    const std::string detail = std::string(name) + " -> " + info.error;

    check(!accepted, "lehnt " + detail + " ab");
    check(!accepted && std::strstr(info.error, needle) != 0,
          std::string("nennt den Grund: ") + detail);
}

} // namespace

int main(int argc, char **argv)
{
    /* Fuzz-Korpus: dieselbe Minimal-PKG, die unten geprueft wird, als Seed
       ablegen (ps4port/core/tools/fuzzcheck.py und die CI nutzen das). */
    if (argc >= 3 && std::strcmp(argv[1], "--seed") == 0)
    {
        const std::string path = std::string(argv[2]) + "/seed.pkg";
        const std::vector<unsigned char> seed = build_package(Options());

        if (!write_file(path, seed))
        {
            std::fprintf(stderr, "Seed nicht schreibbar: %s\n", path.c_str());
            return 1;
        }
        std::printf("Seed: %s (%zu Bytes)\n", path.c_str(), seed.size());
        return 0;
    }
    if (argc < 2)
    {
        std::fprintf(stderr,
                     "Aufruf: pkg_hostcheck <arbeitsverzeichnis>\n"
                     "        pkg_hostcheck --seed <verzeichnis>\n");
        return 2;
    }
    const std::string dir = argv[1];

    /* 1. Stimmige PKG: annehmen und Felder lesen. */
    const std::string good_path = dir + "/gute.pkg";
    const std::vector<unsigned char> good = build_package(Options());
    PkgInfo info;

    check(write_file(good_path, good),
          "PKG geschrieben (" + std::to_string(good.size()) + " Bytes)");
    check(pkg_probe(good_path.c_str(), &info), "nimmt eine stimmige PKG an");
    check(std::strcmp(info.title, kTitle) == 0, std::string("TITLE: ") + info.title);
    check(std::strcmp(info.title_id, kTitleId) == 0,
          std::string("TITLE_ID: ") + info.title_id);
    check(std::strcmp(info.content_id, kContentId) == 0,
          std::string("CONTENT_ID: ") + info.content_id);
    check(std::strcmp(info.app_ver, "01.00") == 0, std::string("APP_VER: ") + info.app_ver);
    check(std::strcmp(info.category, "gd") == 0,
          std::string("CATEGORY: ") + info.category);
    check(info.content_type == kContentTypeBase, "Content-Type 0x1A (PS4GD)");
    check(info.size == good.size(), "Groesse stammt aus der Datei");
    check(info.error[0] == 0, "kein Fehlertext bei gueltiger PKG");

    /* 2. Gezielte Manipulationen: jede muss mit passendem Text scheitern. */
    Options options;

    options = Options();
    options.magic = "\x7f" "CNX";
    expect_reject(dir, "signatur", options, "PKG-Signatur");

    options = Options();
    options.header_content_id = "iv0000-BREW00090_00-PS4UPLOAD0000000";
    expect_reject(dir, "header-klein", options, "Header-Content-ID");

    options = Options();
    options.header_content_id = "IV0000-BREW00091_00-PS4UPLOAD0000000";
    expect_reject(dir, "header-anders", options, "Content-ID passt nicht zum Header");

    options = Options();
    options.content_type = 0x77;
    expect_reject(dir, "content-type", options, "Content-Type nicht unterstuetzt");

    options = Options();
    options.declared_extra = 4096;
    expect_reject(dir, "header-groesse", options, "Header-Groesse");

    options = Options();
    options.entry_count = 0;
    expect_reject(dir, "keine-eintraege", options, "Eintragstabelle");

    options = Options();
    options.entry_count = 65;
    expect_reject(dir, "zu-viele-eintraege", options, "Eintragstabelle");

    options = Options();
    options.entry_id = 0x1200;
    expect_reject(dir, "ohne-sfo", options, "param.sfo fehlt");

    options = Options();
    options.sfo_size_extra = 4096;
    expect_reject(dir, "sfo-ausserhalb", options, "zeigt aus der Datei heraus");

    options = Options();
    options.break_sfo_magic = true;
    expect_reject(dir, "sfo-magic", options, "param.sfo Aufbau");

    options = Options();
    options.title_id = "BREW0009";
    expect_reject(dir, "title-id-kurz", options, "TITLE_ID ungueltig");

    options = Options();
    options.title_id = "BREW0009a";
    expect_reject(dir, "title-id-klein", options, "TITLE_ID ungueltig");

    options = Options();
    options.content_id = "IV0000-BREW00091_00-PS4UPLOAD0000000";
    expect_reject(dir, "content-id-passt-nicht", options,
                  "CONTENT_ID passt nicht zur TITLE_ID");

    options = Options();
    options.category = "zz";
    expect_reject(dir, "category", options, "CATEGORY nicht unterstuetzt");

    options = Options();
    options.with_category = false;
    expect_reject(dir, "ohne-category", options, "Pflichtfeld fehlt");

    options = Options();
    options.app_ver = "1.0";
    expect_reject(dir, "app-ver", options, "APP_VER ungueltig");

    /* 3. Grobe Dateifehler. */
    PkgInfo missing;
    check(!pkg_probe((dir + "/fehlt.pkg").c_str(), &missing), "lehnt fehlende Datei ab");
    check(std::strstr(missing.error, "nicht lesbar") != 0,
          std::string("nennt den Grund: ") + missing.error);

    const std::string empty_path = dir + "/leer.pkg";
    check(write_file(empty_path, std::vector<unsigned char>()), "leere Datei geschrieben");
    PkgInfo empty;
    check(!pkg_probe(empty_path.c_str(), &empty), "lehnt zu kleine Datei ab");
    check(std::strstr(empty.error, "zu klein") != 0,
          std::string("nennt den Grund: ") + empty.error);

    std::printf("\n%d Checks, %d Fehler\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
