# PS4 gegen PS5Upload: Funktionsabgleich

**Stand:** 21. November 2026
**Bezug:** [phantomptr/ps5upload](https://github.com/phantomptr/ps5upload) v12.6.16 (Client für Android/Linux/Windows, Engine, PS5-Payload)
**Status:** `ps4manage.py` ist gebaut und auf dem PC geprüft; die PS4-Hardwareprüfung steht aus

## Kurzfassung

„Genau dieselben Funktionen wie die Android-/Linux-App“ zerfällt auf der PS4 in
drei Gruppen:

1. **Heute nutzbar** – Verwaltung vom Handy/PC über die Dienste, die GoldHEN
   ohnehin mitbringt: FTP (2121), BinLoader (9090), Remote Package Installer
   (12800). Das liefert `ps4manage.py`.
2. **Nur mit residentem Payload** – alles, was während eines laufenden Spiels
   erreichbar sein muss und Sony-APIs braucht: Spiel starten/beenden,
   Deinstallation, Power, Temperatur. Eine Vordergrund-App wird beim Spielstart
   pausiert und hat kein Netz ([`ps4app/README.md`](../ps4app/README.md)).
3. **Auf der PS4 nicht möglich** – Streaming/„Play“, „Play Together“,
   Controller-Emulation als Host, Workshop-Konvertierung, PS5-spezifische
   Mounts. Die PS4 ist Empfänger für chiaki, nicht Host; die Streaming-Teile
   von PS5Upload stehen unter AGPL-3.0 und setzen PS5-Capture voraus.

## Was `ps4manage.py` heute abdeckt

| Upstream-Funktion | PS4-Weg | Status |
|---|---|---|
| **Games** (installierte Titel, Updates/DLC gruppiert, Icons) | `param.sfo` aus `/user/appmeta/<TITLE_ID>/`, Icon aus `icon0.png` | **implementiert** |
| **Browse and manage: Dateien** (Liste, Download, Upload, Umbenennen, Löschen, Ordner) | GoldHEN-FTP; Schreiben nur unter `/data`, `/user`, `/mnt` | **implementiert** |
| **Cheats** (Dateien verwalten) | `/data/GoldHEN/cheats` (`.json`, `.shn`), lesen/schreiben/löschen | **implementiert** |
| **Payload sender** | Liste aus `payloads/` und `goldhen/`, Versand an BinLoader 9090, auch direkter Datei-Upload | **implementiert** |
| **Save Data: Back up now** | `/user/home/<user>/savedata/<TITLE_ID>` als ZIP herunterladen (nur lesen) | **implementiert** |
| **Upload & Install** (PKG) | `ps4upload.py` über RPI 12800 oder GoldHEN-Installer | **vorhanden** (separates Tool) |
| **Upload mit Resume/Queue** | Resume per `HEAD`-Offset und `?offset=` im Payload; Warteschlange nur im PC-Tool, AVA1-Journal fehlt | **Resume ja, Queue offen** |
| **Games: Play/Stop, Uninstall** | `sceLncUtilLaunchApp`, `sceSystemServiceKillApp`, `sceAppInstUtilAppUnInstall*` | **offen** – braucht Payload |
| **Power** (Rest Mode, Reboot, Shutdown) | Systemservice im Payload | **offen** – braucht Payload |
| **Hardware-Ansicht, Lüfterkurve** | Payload + Sensoren | **offen** – braucht Payload |
| **Play** (Streaming zum Client) | kein Capture-/Encoder-Pfad auf der PS4 | **nicht möglich** |
| **Play Together, virtueller Controller als Host** | setzt Streaming voraus | **nicht möglich** |
| **Workshop / Konvertierung** | PC-Werkzeug, nicht auf der Konsole | **nicht im Scope** |
| **Minion** (Konsole bedient sich selbst im Browser) | eigene UI in `ps4app` (Port 8080) | teilweise – nur Upload/Install |
| **Mehrere Geräte, Pairing** | heute keine Authentifizierung; nur Loopback der UI | **offen** |

## Architektur heute

```text
Handy / PC (Browser)                 PS4 (GoldHEN)
   │                                    │
   ├── http://127.0.0.1:9899  ──────────┤  ps4manage.py (PC)
   │      Bibliothek, Dateien,          ├─ FTP   :2121  (ftpsrv)
   │      Cheats, Sicherungen,          ├─ BinLoader :9090
   │      Payloads                      └─ RPI   :12800
   │
   └── ps4upload.py :9898  ────────────── PKG-Upload + Installation
```

`ps4manage.py` bindet nur an `127.0.0.1`: Wer die Oberfläche erreicht, kann auf
der Konsole Dateien löschen. Für Zugriff vom Handy entweder `--port` mit
kurzem SSH-Tunnel freigeben oder den Manager auf dem Gerät starten, das den
Zugriff haben soll. Bewusst kein offener LAN-Dienst wie bei PS5Upload
(`PS5UPLOAD_ALLOW_IP`), solange es keine Geräteauthentifizierung gibt.

Warum GoldHEN-FTP und nicht AVA1: AVA1 braucht einen eigenen Payload auf der
Konsole. Der existiert noch nicht; der host-getestete Kern liegt in
[`ps4port/core`](../ps4port/core/README.md), der Payload-Build ist Meilenstein
B0 in [`ps4port/option-b-payload`](../ps4port/option-b-payload/README.md).

## Stufenplan bis zur Parität

| Stufe | Inhalt | Voraussetzung | Gate |
|---|---|---|---|
| **1 – heute** | `ps4manage.py`: Bibliothek, Dateien, Cheats, Payloads, Sicherungen (lesen), Upload+Install über `ps4upload.py` | GoldHEN mit FTP | Auf echter PS4 einmal durchgeklickt |
| **2** | Payload auf Basis `ps4port/core`: `/status`, `POST /upload` mit Resume (`HEAD`/`?offset=`), `DELETE`, `GET /files`, Quote aus `config.txt`, PKG-Prüfung, PIN, Timeout, Reserve und Diagnose-Log sind gebaut; der BGFT-Code liegt hinter einer host-getesteten Backend-Tabelle und wird per dlsym geladen, ist aber noch nicht im Payload; Hardwareprüfung offen | ps4-payload-sdk, Testkonsole | G0: Payload antwortet nach Spielstart |
| **3** | Spiel starten/beenden, Deinstallation, Power, Temperatur über den Payload | Stufe 2 | G2: drei Spiele starten/stoppen ohne `app.db`-Schaden |
| **4** | Geräte-Token/Paaring für die UI, mehrere Clients, optional AVA1-Anbindung an die PS5Upload-Engine | Stufe 2 | Zwei Geräte, fremde IP abgewiesen |

Aufwand Stufe 2–3: laut [`docs/PS4-UPLOAD-SUITE-FEASIBILITY.md`](PS4-UPLOAD-SUITE-FEASIBILITY.md)
grob 2–3 Monate mit Testkonsole. Stufe 1 ist der Teil, der ohne Konsole
entwickelbar und ohne Payload sofort benutzbar ist.

## Bewusste Nicht-Ziele

- Streaming/„Play“, „Play Together“, virtueller Controller als Host.
- PS5-Unterstützung; dafür bleibt PS5Upload zuständig.
- Schreiben in `app.db` oder `/user/app` von Hand.
- Spiel-Konvertierung/Dumps auf der Konsole.

## Was geprüft ist

- `ps4manage.py` kompiliert, Oberfläche und API antworten lokal (HTTP 200).
- Bibliotheks-Gruppierung, Pfadprüfung und FTP-Listing-Parser laufen gegen
  Fixtures.
- `payload/ps4/` baut ein PS4-Payload-ELF (`dist/PS4Upload-Payload-0.6.2.elf`,
  215.752 Bytes, SHA-256 `c122cae8…b5efbc1`, ET_DYN x86-64, `/status`,
  `/upload`, `/files`, `/diagnostics`, `X-Upload-Pin`, `X-Upload-Offset` und
  `X-Upload-Complete` enthalten); die Form
  entspricht dem Hello-Sample der ps4-payload-sdk. Beim Durchsehen des
  Upload-Pfads fiel auf, dass der Headerblock zuvor an der Anfragezeile
  abgeschnitten wurde (`Content-Length` und PIN waeren nie gefunden worden).
  Seit 0.4.0 liegt das Zerlegen im Kern: `ps4u_header_block_length()`,
  `ps4u_parse_head()` und `ps4u_content_length()` sind host-getestet, der
  Payload ruft nur noch auf. In 0.4.1 ist der Weg dorthin kuerzer:
  `ps4u_parse_head()` bekommt die Blocklaenge vom Finder (kein zweiter Scan
  ueber den Puffer), `ps4u_content_length()` prueft gegen
  `PS4U_UPLOAD_MAX_BYTES`, und der Payload liest PIN und Laenge direkt aus
  seinem Anfragepuffer (`PS4U_HEAD_MAX`), statt den Block in einen zweiten
  Puffer zu kopieren.
  P2 ist mit 0.5.0 im Payload: `GET /files` listet Name und Groesse aller
  Pakete samt `.pkg.part`-Resten mit Quote und freiem Platz; die Quote kommt
  aus `config.txt` (`quota_bytes`), ein Upload darueber antwortet `507` mit
  `used`/`quota`. Beim Resume zaehlt der `.part`-Rest wieder zum freien Platz
  (`upload_bgft_space_ok_resume`), sonst scheiterte ein fortgesetzter Upload
  faelschlich an der BGFT-Reserve. In 0.5.1 ist der Abschluss idempotent:
  `HEAD` meldet bei einem fertigen Paket die Groesse plus
  `X-Upload-Complete: 1`, und ein wiederholter `POST` mit `Content-Length: 0`
  auf dieselbe Groesse antwortet `200` mit `already` statt `409`. P0 in 0.6.0
  (seit 0.6.1 aus einer gemeinsamen, orbis-freien Liste in
  `ps4app/bgft_symbols.h`, die `install_bgft.h` und `/diagnostics` teilen):
  `GET /diagnostics` (PIN, nur GET) meldet als JSON freien Platz, Quote,
  Schreib-/Loeschtest im Upload-Ordner, die PIN-Datei, das Ergebnis von
  `sceAppInstUtilInitialize` und je BGFT-Symbol das `sceKernelDlsym`-Ergebnis,
  nachdem `libSceBgft.sprx` per `sceKernelLoadStartModule` geladen wurde
  (erst Systempfad, dann Soname). Damit traegt das ELF jetzt
  `libSceAppInstUtil.sprx` in `DT_NEEDED`. Das JSON baut
  `ps4u_diag_json()` in `ps4port/core` und ist host-getestet; ob Laden und
  AppInstUtil auf einer Konsole klappen, ist offen (genau dafuer ist der
  Endpunkt da). In 0.6.2 liegen auch der Schreib-/Loeschtest
  (`ps4u_diag_write_test`, gegen echtes und fehlendes Verzeichnis getestet)
  und der gemeinsame JSON-Puffer (`src/json_out.h`) im Kern; das BGFT-Modul
  wird nur beim ersten Abruf geladen und dann wiederverwendet.
- P1-1 (Stand 21.11.2026): Die Installation liegt hinter einer Backend-Tabelle
  (`ps4app/install_core.h`, sechs Aufrufe), `ps4app/install_bgft.h` holt BGFT
  per `sceKernelLoadStartModule`/`sceKernelDlsym` aus `libSceBgft.sprx`.
  `payload/ps4/tests/install_hostcheck.cpp` faehrt die Zustandsmaschine mit
  einem Fake-Backend: 109 Checks, 0 Fehler (Erfolg, Init-/Register-/
  Start-/Fortschrittsfehler, gemeldeter Installationsfehler, Stillstand,
  Prozentquellen, Poll-Drosselung, Uhr-Ruecksprung, Namensgrenzen, fehlendes
  Backend). `link-check` baut `install.h` gegen die orbis-Header und linkt
  ohne `libSceBgft` (DT_NEEDED nur `libkernel_web`, `libSceNet`,
  `libSceLibcInternal`, `libSceAppInstUtil`, `libSceUserService`); der
  Gegenversuch ohne orbis-Header linkt `sceAppInstUtilAppInstallPkg` allein
  gegen `libSceAppInstUtil`. Frueher aufgefallen und behoben: `errno` fehlt im
  Payload-Link (`libc.a` ohne `__errno_location`). Ob der Lader
  `libSceBgft.sprx` in einen Payload-Prozess laedt, ist ungeprueft.
- AVA1-C-Node `v1.0.11` ist als eigene Quelle gepinnt
  ([`AVA1.md`](../ps4port/AVA1.md), `upstream-ava1/`, nicht versioniert) und mit
  `ps4port/tools/ava1_portcheck.py` gegen die
  ps4-payload-sdk kompiliert und gelinkt: 25 AVA1-Quellen, `platform_ps4.c`,
  `-femulated-tls`, portable Crypto, 2.036.768-Byte-ELF. Laufzeit, Pairing und
  Resume auf der PS4 bleiben offen.
- B0-Link-Spike (`python ps4port/tools/b0_link_spike.py`, Stand 21.11.2026):
  HTTP-Payload 0.5.0 und AVA1-C-Node in einem 2.152.512-Byte-ELF gelinkt;
  DT_NEEDED `libSceLibcInternal.sprx`, `libSceNet.sprx`, `libkernel_web.sprx`.
  AVA1 wird darin nicht gestartet; Laden, Ports, Threads und G0/G0b bleiben
  offen.
- Fuzzing der Flächen ohne PIN (`ps4port/core/tests/fuzz_targets.cpp`):
  `ps4u_parse_head`, `ps4u_header_value`, `ps4u_content_length` und
  `pkg_probe` (PKG-Kopf, Eintragstabelle, param.sfo). In der CI 60 Sekunden
  libFuzzer mit ASan/UBSan, lokal über `tools/fuzzcheck.py` als
  Standalone-Lauf (zig kennt `-fsanitize=fuzzer` nicht): 50.000 deterministische
  Mutationen aus HTTP-Seeds und einer echten Minimal-PKG ohne Absturz. Der
  lokale Lauf hat keine Sanitizer - die liefert nur der CI-Lauf.
- Dieselben drei Hosttests baut die CI zusaetzlich mit clang unter
  `-fsanitize=address,undefined` (Leaks an, UBSan ohne Recovery); der Lauf
  ist lokal nicht nachgestellt, weil hier kein clang installiert ist.
- `ps4port/core/tools/hostcheck.py`: 258 Checks, 0 Fehler (Anfragezeile,
  Kopfblock/Content-Length, Header, Query/Offset, Routen, Antwortaufbau,
  Status-JSON, Dateiliste/Quote, Diagnostics-JSON, Schreib-/Loeschtest, die
  gemeinsame BGFT-Symbolliste und die Reserve-Regeln aus `upload.h`).
- `payload/ps4/tests/pkg_hostcheck.cpp` (vom selben Runner gebaut): 47 Checks,
  0 Fehler. `pkg_probe` nimmt eine erzeugte Minimal-PKG (Kopf,
  Eintragstabelle, `param.sfo`) an und liest TITLE, TITLE_ID, CONTENT_ID,
  APP_VER, CATEGORY, Content-Type und Groesse; 16 manipulierte Varianten
  (Signatur, Header-Content-ID, Content-Type, Header-Groesse, Eintragszahl,
  fehlende `.sfo`, `.sfo` ausserhalb der Datei, SFO-Magic, TITLE_ID, APP_VER,
  CATEGORY, Pflichtfeld, Content-ID-Brueche) sowie fehlende und leere Datei
  werden mit dem erwarteten Fehlertext abgelehnt.
- `tools/ps4payload-client.py`: Status, Upload mit und ohne Resume, falsche PIN,
  `DELETE`, Namen mit Leerzeichen (prozentual kodiert), `409` mit echtem Offset
  und ein Resume ohne offene Bytes gegen einen lokalen Mock des Payloads
  geprüft (34 Checks, 0 Fehler), inklusive „fertige Datei meldet Groesse und
  `X-Upload-Complete`, wiederholter Abschluss liefert `already`" sowie
  `diagnostics` (lesbare Ausgabe, falsche und fehlende PIN).
- Nicht geprüft: echte PS4 (kein Gerät im Projekt), `GET /diagnostics` auf
  Hardware (statvfs, Schreibtest, `sceAppInstUtilInitialize`, BGFT-Laden und
  dlsym), Laden des HTTP-Payloads
  und des B0-Spikes, AVA1-Threads/-Sockets, FTP-Sonderfälle (Rechte,
  Sonderzeichen), große Sicherungen.
