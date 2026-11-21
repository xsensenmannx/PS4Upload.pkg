# AVA1-Pin: phantomptr/ava1 v1.0.11

**Stand:** 21. November 2026  
**Zweck:** eigener C-Node für den PS4-Port, getrennt von PS5Upload; keine Streaming-Komponente.

## Pin

| Feld | Wert |
|---|---|
| Repository | <https://github.com/phantomptr/ava1> |
| Release | `v1.0.11` (19. November 2026) |
| Tag-Objekt | `69decea817067cfe6aa2100c9221a1d5671801d9` (annotiert) |
| Commit | `deee3bbce1e65be48b71f20fe09cefd10431b906` |
| Lizenz | MIT **oder** Apache-2.0; Monocypher BSD-2-Clause/CC0; BLAKE3 CC0/Apache-2.0 |
| Lokaler Snapshot | `upstream-ava1/` (in `.gitignore`, 5,9 MB) |

Der Snapshot lässt sich jederzeit neu erzeugen:

```bash
git clone --depth 1 --branch v1.0.11 https://github.com/phantomptr/ava1.git upstream-ava1
git -C upstream-ava1 rev-parse HEAD
# deee3bbce1e65be48b71f20fe09cefd10431b906
```

## Was übernommen wird

- `c/*.c` – Server, Verbindungen, Datenpfad, Journal/Apply, Copy, RPC und Crypto
- `c/gen/ava1_gen.{c,h}` – generierte Codecs
- `c/third_party/monocypher/` – Noise/Crypto
- `c/third_party/blake3/` – portables BLAKE3, keine SIMD-Objekte
- [`ava1/platform_ps4.c`](ava1/platform_ps4.c) – unser PS4-Plattform-Shim

Die Upstream-Dateien bleiben unverändert. Der Shim ersetzt nur
`platform_posix.c`/`platform_ps5.c` und hält den Fünf-Funktionen-Vertrag aus
`c/ava1_platform.h`.

## Was nicht übernommen wird

- Rust-Crates, Beispiele, Schema/Spec/Vektoren (nur Referenz)
- `platform_posix.c`, `platform_ps5.c`, `ava1_chacha_avx2.c`
- Release-Binaries
- **Kein Streaming:** Der AVA1-C-Node transportiert Dateien und Ordner; er
  enthält keinen Video-, Audio-, Capture- oder `vpad`-Code. `remoteplay/` und
  der Streaming-Pfad bleiben unberührt.

## Portabilitätsprüfung

```bash
python ps4port/tools/ava1_portcheck.py
```

Ergebnis auf dem PC (21. November 2026):

- **25 AVA1-C-Quellen** plus Generator, Shim, Monocypher, drei portablen
  BLAKE3-Dateien und Link-Test kompiliert mit
  `zig cc --target=x86_64-freebsd`, den FreeBSD-9-Headern der
  ps4-payload-sdk und `-O2 -fPIC -fno-stack-protector -Wall -Wextra -Werror`.
- **Link OK:** `crt1.o`/`elf_x86_64.x` plus
  `-lc -lSceLibcInternal -lSceNet -lpthread -lkernel_web` ergibt ein
  `ET_DYN`-x86-64-ELF mit **2.036.768 Bytes**.
- **`-femulated-tls` ist Pflicht:** `ava1_server.c` hat
  `static __thread uint8_t rpc_peer[32]`; ohne emuliertes TLS scheitert der
  Link an `STT_TLS`/`PT_TLS`.
- **Portable Crypto:** `-DAVA1_AEAD_PORTABLE` und BLAKE3 ohne SSE/AVX. Der
  PS4-Jaguar hat kein AVX2; `ava1_chacha_avx2.c` wird nicht gebaut.
- **Drei PS5-Aufrufe fehlen auf der PS4:** `posix_fallocate`,
  `posix_fadvise` und `utimensat`. Der Shim ersetzt sie durch `ftruncate`,
  `ENOTSUP` (Read-back liest dann gegebenenfalls aus dem Cache) und `futimes`.
  `ftruncate` legt die Datei an, reserviert aber keine Blöcke; `ENOSPC` kann
  erst beim Schreiben kommen.
- **Vorhanden und gelinkt:** `sysctl(KERN_ARND)`, `pthread_*`, `poll`,
  `socket`, `clock_gettime`, `getrlimit`, `mmap`/`msync` und `O_DIRECT`
  (der Node fällt bei `EINVAL` auf normale Reads zurück).

**Nicht geprüft:** Ausführung, Thread-/Socket-Verhalten, `/data`-Dateisystem,
Durchsatz, Pairing und Resume auf einer PS4. Das bleibt Gate **G0b**. Der
Check beweist nur, dass der C-Node mit dem Shim gegen die SDK baut und linkt.

## Größe

Der AVA1-Link-Test ist 2.036.768 Bytes groß, der HTTP-Payload 169.976 Bytes.
Der kombinierte B0-Link-Spike (`python ps4port/tools/b0_link_spike.py`) ist
2.152.512 Bytes groß; AVA1 wird darin noch nicht gestartet. Das ist eine
Größenordnung, keine Laufzeitmessung. Details: [`B0-STATUS.md`](B0-STATUS.md).

## Lizenzhinweise

AVA1 steht unter MIT **oder** Apache-2.0; Monocypher unter BSD-2-Clause/CC0,
BLAKE3 unter CC0/Apache-2.0. Die Hinweise der mitgelieferten Teile bleiben
erhalten. Der PS4-Shim ist eigener Code dieses Projekts und steht unter
GPL-3.0 wie der Rest des Ports (siehe [`../LICENSE`](../LICENSE)).
