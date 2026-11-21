# Gemeinsamer Payload-Kern

Reiner C11-Code, der auf dem PC gebaut und getestet wird und später im
PS4-Payload läuft. Er enthält bewusst **keine** PS4-Header, Sockets, Threads
oder Sony-APIs – damit die Logik ohne Konsole prüfbar bleibt.

## Was drin ist

| Datei | Aufgabe |
|---|---|
| `include/ps4u_http.h`, `src/http.c` | Kopfblock aus dem Socket-Puffer schneiden (`ps4u_header_block_length`), Anfragezeile parsen (GET/HEAD/POST/DELETE, `name` und `offset`), `Content-Length` und Header lesen, `.pkg`-Namen und `/files`-Einträge prüfen, Statuscode je Route, HTTP-Kopf und Fehlertexte bauen |
| `include/ps4u_status.h`, `src/status.c` | `/status` als JSON mit `snake_case`, Maskierung (`ps4u_json_escape`) und begrenztem Puffer |
| `include/ps4u_files.h`, `src/files.c` | Quote aus `config.txt` (`ps4u_config_quota`), Quotenprüfung (`ps4u_quota_ok`) und die JSON-Liste für `GET /files` |
| `include/ps4u_diag.h`, `src/diag.c` | JSON für `GET /diagnostics` (`ps4u_diag_json`): Platz/Quote, Schreib-/Löschtest, PIN-Datei, AppInstUtil-Rückgabe und je BGFT-Symbol das dlsym-Ergebnis; dazu der Schreib-/Löschtest selbst (`ps4u_diag_write_test`) — reine Datei-Ein-/Ausgabe, deshalb host-testbar |
| `src/json_out.h` | gemeinsamer JSON-Puffer für `files.c` und `diag.c` (Text, Zahlen, Abschneiden am Pufferende) |
| `tests/hostcheck.c` | Selbsttest für alle Fälle oben |
| `tests/fuzz_targets.cpp` | Fuzz-Ziele für die Flächen ohne PIN: Anfragekopf (`ps4u_parse_head`, `ps4u_header_value`, `ps4u_content_length`) und PKG-Kopf (`pkg_probe`); libFuzzer-tauglich, mit `-DPS4U_FUZZ_STANDALONE` auch ohne libFuzzer lauffähig |
| `tools/fuzzcheck.py` | baut `fuzz_targets.cpp` mit zig als Standalone und fährt deterministische Mutationen von HTTP-Seeds und einer echten Minimal-PKG (`pkg_hostcheck --seed`) |
| `tools/hostcheck.py` | baut mit `zig cc -Wall -Wextra -Werror` und führt den Selbsttest aus; baut zusätzlich den PKG-Hosttest des Payloads (`payload/ps4/tests/pkg_hostcheck.cpp`, C++17 gegen `ps4app/pkg.h`) und den Install-Hosttest (`payload/ps4/tests/install_hostcheck.cpp` gegen `ps4app/install_core.h`) |

## Was noch fehlt

Der Kern ist die Basis für den G0-Spike, kein fertiger Payload:

- Socket-Annahme, Verbindungslimits und Zeitüberschreitungen (POSIX, später
  ps4-payload-sdk)
- Daemon-Lebenszyklus, Config unter `/data/ps4upload/`, Logring
- AVA1-C-Node und Pairing
- BGFT-Registrierung und Fortschritt (der Upload-Strom mit Resume und
  PKG-Prüfung liegt im Payload, nicht hier)
- die eigentliche Web-UI und die TV-Setup-App

## Prüfen

```bash
python ps4port/core/tools/hostcheck.py
```

Erwartet: `258 Checks, 0 Fehler` für den Kern, `47 Checks, 0 Fehler` für die
PKG-Prüfung und `109 Checks, 0 Fehler` für die Installations-Zustandsmaschine
(Fake-Backend), Rückgabecode 0. GitHub Actions fährt dieselben drei Programme
in [`.github/workflows/payload.yml`](../../.github/workflows/payload.yml). Ein
weiterer Job baut dieselben drei Programme mit clang und
`-fsanitize=address,undefined` (Leak-Erkennung an, UBSan ohne Recovery), damit
Speicher- und UB-Fehler vor dem Kernelkontext auffallen; ein dritter fährt
einen 60-Sekunden-libFuzzer-Lauf über `tests/fuzz_targets.cpp`. Wer keinen libFuzzer hat (Windows/zig), nimmt den
Standalone-Lauf:

```bash
python ps4port/core/tools/fuzzcheck.py 50000
```

Der Build landet unter
`ps4port/core/build/` (über `build/` ignoriert).

## Portierungsregeln

- Keine dynamische Speicheranlage in den Parse-Funktionen; alle Puffer haben
  eine feste Obergrenze.
- Kein `rename()` über Mountgrenzen, keine großen Puffer auf Thread-Stacks.
- Jede blockierende Wartezeit bekommt eine Obergrenze.
- JSON-Schlüssel bleiben `snake_case`; jeder eingesetzte String wird maskiert.
- Der Kern bleibt frei von PIN-/Token-Politik; die liegt im Payload
  (`ps4app/pin.h`).

## PS4-Build (noch nicht eingerichtet)

Der Kern ist so geschrieben, dass ihn die
[ps4-payload-sdk](https://github.com/ps4-payload-dev/sdk) (FreeBSD-9-Header,
GPLv3+) übersetzen kann. Der eigentliche Payload-Build und der Test auf einer
Konsole sind der erste Schritt von
[Option B](../option-b-payload/README.md); ohne diese Hardware gibt es keine
Lauffähigkeitsaussage.
