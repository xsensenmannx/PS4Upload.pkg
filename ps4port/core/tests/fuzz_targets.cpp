/*
 * Fuzz-Ziele fuer die Flaechen, die im LAN ohne PIN erreichbar sind:
 *
 *   * ps4u_parse_head / ps4u_header_block_length / ps4u_header_value
 *     (Anfragekopf, Query, Content-Length, PIN-Header),
 *   * pkg_probe aus ps4app/pkg.h (PKG-Kopf, Eintragstabelle, param.sfo).
 *
 * Zwei Betriebsarten, ein Ziel:
 *
 *   * libFuzzer: clang++ -fsanitize=fuzzer,address,undefined (CI-Lauf).
 *   * Standalone: -DPS4U_FUZZ_STANDALONE, dann `fuzz_targets <N> <seed-datei>`
 *     ueber ps4port/core/tools/fuzzcheck.py; noetig, weil zig kein
 *     libFuzzer-Runtime mitbringt (Windows/Entwicklung).
 *
 * Das PKG-Ziel schreibt die Eingabe in eine temporaere Datei, weil pkg_probe
 * einen Pfad erwartet. Es laeuft nur bei passender Signatur, sonst kehrt
 * pkg_probe sofort zurueck und jeder Durchlauf waere nur Festplatten-I/O.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "pkg.h"
#include "ps4u_http.h"

namespace {

/* Eine Eingabe ueber 64 KiB bringt keinen neuen Pfad, kostet aber Zeit. */
const std::size_t kMaxInput = 64 * 1024;

void fuzz_request_block(const std::uint8_t *data, std::size_t size)
{
    const char *raw = reinterpret_cast<const char *>(data);
    std::string text(raw, size);
    ps4u_request request;
    char value[64];
    unsigned long long number = 0;
    std::size_t block = ps4u_header_block_length(raw, size);

    if (block != 0 && ps4u_parse_head(raw, block, &request) == 0) {
        (void)ps4u_route_status(request.route, request.method);
        (void)ps4u_safe_pkg_name(request.name);
        (void)ps4u_listable_file(request.name);
    }
    /* header_value und content_length arbeiten auf NUL-terminierten Texten. */
    (void)ps4u_content_length(text.c_str(), &number);
    (void)ps4u_header_value(text.c_str(), "X-Upload-Pin", value, sizeof(value));
    (void)ps4u_header_value(text.c_str(), "Content-Length", value, sizeof(value));
    (void)ps4u_header_value(text.c_str(), "X-Upload-Offset", value, sizeof(value));
}

void fuzz_pkg(const std::uint8_t *data, std::size_t size)
{
    /* Ohne PKG-Signatur prueft pkg_probe nur vier Bytes und kehrt zurueck. */
    if (size < 4 || std::memcmp(data, "\x7f" "CNT", 4) != 0) {
        return;
    }
    const char *path = std::getenv("PS4U_FUZZ_PKG");
    if (path == nullptr) {
        path = "fuzz-pkg.tmp";
    }
    FILE *file = std::fopen(path, "wb");
    if (file == nullptr) {
        return;
    }
    std::fwrite(data, 1, size, file);
    std::fclose(file);

    PkgInfo info;
    std::memset(&info, 0, sizeof(info));
    (void)pkg_probe(path, &info);
    std::remove(path);
}

void fuzz_one(const std::uint8_t *data, std::size_t size)
{
    fuzz_request_block(data, size);
    fuzz_pkg(data, size);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
    if (data == nullptr || size == 0 || size > kMaxInput) {
        return 0;
    }
    fuzz_one(data, size);
    return 0;
}

#ifdef PS4U_FUZZ_STANDALONE

namespace {

std::uint64_t g_state = 0x9e3779b97f4a7c15ull;

std::uint32_t next_random(void)
{
    /* xorshift64*: deterministisch, damit ein Fehlschlag reproduzierbar ist. */
    g_state ^= g_state >> 12;
    g_state ^= g_state << 25;
    g_state ^= g_state >> 27;
    return (std::uint32_t)((g_state * 0x2545f4914f6cdd1dull) >> 32);
}

bool read_file(const char *path, std::vector<std::uint8_t> *out)
{
    FILE *file = std::fopen(path, "rb");
    long length;

    if (file == nullptr) {
        return false;
    }
    std::fseek(file, 0, SEEK_END);
    length = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (length <= 0 || length > (long)kMaxInput) {
        std::fclose(file);
        return false;
    }
    out->resize((std::size_t)length);
    if (std::fread(out->data(), 1, out->size(), file) != out->size()) {
        std::fclose(file);
        return false;
    }
    std::fclose(file);
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    static const char http_seed[] =
        "POST /upload?name=spiel.pkg&offset=1024 HTTP/1.1\r\n"
        "Content-Length: 2048\r\nX-Upload-Pin: 4711\r\n\r\n"
        "GET /diagnostics HTTP/1.1\r\nX-Upload-Pin: 0000\r\n\r\n";
    std::vector<std::vector<std::uint8_t>> seeds;
    unsigned long iterations = 20000;
    unsigned long executed = 0;

    if (argc > 1) {
        iterations = std::strtoul(argv[1], nullptr, 10);
    }
    seeds.emplace_back(http_seed, http_seed + sizeof(http_seed) - 1);
    if (argc > 2) {
        std::vector<std::uint8_t> seed;

        if (read_file(argv[2], &seed)) {
            seeds.push_back(seed);
        } else {
            std::fprintf(stderr, "Seed nicht lesbar: %s\n", argv[2]);
        }
    }

    for (unsigned long index = 0; index < iterations; index++) {
        const std::vector<std::uint8_t> &base = seeds[next_random() % seeds.size()];
        std::vector<std::uint8_t> input = base;
        unsigned flips = 1 + (next_random() % 12);

        for (unsigned flip = 0; flip < flips && !input.empty(); flip++) {
            input[next_random() % input.size()] = (std::uint8_t)next_random();
        }
        /* Gelegentlich kuerzen oder anhaengen, damit auch Grenzlaengen laufen. */
        if (!input.empty() && next_random() % 4 == 0) {
            input.resize(next_random() % input.size());
        }
        if (next_random() % 4 == 0) {
            input.push_back((std::uint8_t)next_random());
        }
        if (input.empty()) {
            continue;
        }
        fuzz_one(input.data(), input.size());
        executed++;
    }

    std::printf("Fuzz (Standalone): %lu Eingaben aus %zu Basen\n", executed,
                seeds.size());
    return 0;
}

#endif
