# Option B – Payload zuerst: Upload und Installation im Hintergrund

**Stand:** 21. November 2026  
**Basis:** [Upstream-Pin v12.6.16](../UPSTREAM.md) · [AVA1-Pin v1.0.11](../AVA1.md) · [gemeinsamer Kern](../core/README.md)  
**Status:** B0-Link-Spike gebaut: HTTP-Payload 0.5.0 und AVA1 v1.0.11 in einem ELF; AVA1 im Spike noch nicht gestartet, nichts auf einer PS4 geladen; G0/G0b offen

## Ziel

Ein residenter PS4-Payload, der eine PKG annimmt, sie prüft, über BGFT
installiert und dabei weiterläuft, wenn ein Spiel gestartet wird. Bedient wird
er über eine kleine Web-UI im LAN; auf dem Fernseher steht nur Start, Adresse,
PIN und Status.

Das ist der direkte Nutzen des heutigen `ps4app`, aber ohne dessen harte
Grenze: Eine Vordergrund-App wird beim Spielstart pausiert und verliert das
Netz. Deshalb ist der Payload das Produkt und die PKG-App nur der Startknopf.

## Architektur

```text
Handy / PC / Tablet (Browser)
        │ HTTP + Token (UI)      AVA1 :9120 (Transfer, Resume, Pairing)
        ▼
┌──────────────────────── PS4 (GoldHEN) ────────────────────────┐
│  ps4upload.elf  (Payload, muss den Spielstart überleben)       │
│   ├─ AVA1-C-Node :9120   Upload, Resume-Journal, Pairing       │
│   ├─ HTTP :9180          /, /status, /upload, /files; Limits   │
│   ├─ PKG-Prüfung         Magic, Header, SFO, Content-Type      │
│   ├─ BGFT                Task, Fortschritt, Abbruch            │
│   └─ Queue               ein Auftrag, Pause/ETA, Persistenz    │
│                                                                │
│  PS4Upload.pkg  (Setup-App)                                    │
│   └─ Payload starten/stoppen, Adresse + PIN zeigen             │
└────────────────────────────────────────────────────────────────┘
```

## Umfang

**Enthalten**

- Payload-ELF für die ps4-payload-sdk, Start über GoldHEN-BinLoader und
  Autoload aus `/data/GoldHEN/payloads/`
- HTTP-Dienst mit `/`, `/status`, `/upload`, `/files`; Token im Header,
  Default Loopback, LAN nur mit Host-Allowlist, `no-store`
- AVA1-Transfer mit Journal, Resume nach Verbindungsabbruch und nach
  Payload-Neustart
- PKG-Prüfung wie in `ps4app/pkg.h`: Magic, Header-Größe, Eintragstabelle,
  `param.sfo`, Content-Type → BGFT-Typ
- BGFT-Registrierung, Fortschritt, Abbruch; Stillstandserkennung, die die
  Quelldatei **nicht** vorschnell löscht
- Queue mit einem aktiven Auftrag, Pause, ETA, Persistenz unter
  `/data/ps4upload/`
- Mini-Web-UI: Upload, PIN, Status, Log, Installationsfortschritt
- TV-Setup-App: Daemon starten/stoppen, Adresse, PIN, Status

**Nicht enthalten**

- PS5Upload-Engine, Tauri-Client, Docker-UI
- Spielebibliothek, Start/Stop, Deinstallation, Dateimanager
- Cheats, Saves, Power-Aktionen
- Streaming, Capture, `vpad`, Spiel-Konvertierung
- PS5-Unterstützung

## Meilensteine

| Stufe | Inhalt | Gate | Dauer (Größenordnung) |
|---|---|---|---|
| **B0** | Payload-Spike: ELF bauen, senden, `/status` im Spiel beantworten; AVA1-C-Node auf Sockets/pthreads übersetzen | **G0** Payload antwortet nach Spielstart; **G0b** AVA1 pairt und nimmt einen abgebrochenen 1-GB-Transfer wieder auf | 1–2 Wochen |
| **B1** | Upload, Resume, PKG-Prüfung, BGFT, Queue, Token, Fortschritt | **G1** 10-GB-Upload übersteht Abbruch; Installation startet; UI zeigt echten BGFT-Zustand | 2–3 Wochen |
| **B2** | Mini-UI, TV-Setup-App, Update-Pfad, Fehlertexte | **G2** 30-Minuten-Soak mit Spielstart, Upload und Installation ohne Absturz | 1–2 Wochen |

**B0 zuerst.** Ohne G0 ist jede UI-Arbeit spekulativ: Eine Vordergrund-App
kann das Problem nicht lösen, und ein Payload, der beim Spielstart stirbt,
macht den ganzen Ansatz hinfällig. Fallback, falls G0 scheitert: bei der
bestehenden Vordergrund-App bleiben und das Projekt ehrlich als „größere
Upload-App“ bezeichnen.

### B0 – Details

1. ELF mit der ps4-payload-sdk bauen, per BinLoader senden, aus
   `/data/GoldHEN/payloads/` autoloaden.
2. HTTP-Server auf `:9180` mit `/status` (Version, Uptime, Token-Prüfung),
   zunächst ohne UI.
3. AVA1-C-Node (Apache-2.0/MIT, [Pin v1.0.11](../AVA1.md)) für die PS4-SDK
   übersetzen: pthreads, Sockets, `poll`, `clock_gettime`. Der Host-Check
   `python ps4port/tools/ava1_portcheck.py` kompiliert und linkt ihn bereits
   mit `platform_ps4.c`; auf der Konsole folgen `hello`/`status`, dann 1 GB mit
   absichtlichem Abbruch und Resume.
4. Spiel starten, während der Payload läuft, und beide Dienste erneut
   abfragen. Ergebnis mit Firmware, GoldHEN-Version und Rückgabecodes
   protokollieren.

**Stand 21.11.2026:** Die SDK v0.9 ist eingerichtet (fester SHA-256), der
HTTP-Payload 0.5.0 baut (169.976 Bytes), der AVA1-C-Node linkt allein
(2.036.768 Bytes), und der kombinierte B0-Link-Spike linkt (2.152.512 Bytes).
Der Spike startet AVA1 noch nicht; er beweist nur, dass beide Stacks in einem
ELF koexistieren. Details und Prüfgrenzen: [`B0-STATUS.md`](../B0-STATUS.md).

### B1 – Details

- Upload-Strom direkt auf Platte, 64-KiB-Stücke, kein vollständiger RAM-Puffer
- zwei PKG-Kopien plus 64 MiB Reserve vor dem Upload prüfen
- APP_VER-Vergleich wie heute, gleiche/ältere Version nur mit `force=1`
- BGFT-Task aus dem Payload; Fortschrittsfelder firmwareabhängig behandeln
- Stillstand: Task stoppen, abmelden, Quelle behalten, klaren Fehler zeigen
- Token pro Gerät, Ratenbegrenzung, fünf Fehlversuche → kurze Sperre
- Queue und Journal über einen Payload-Neustart hinweg fortsetzen

## Wiederverwendung

| Vorhanden | Verwendung |
|---|---|
| `ps4app/pkg.h` | PKG-/SFO-Prüfung und Content-Type → BGFT-Typ, unverändert |
| `ps4app/install.h` | BGFT-Registrierung und Fortschritt; Stillstand entschärfen |
| `ps4app/upload.h` | Timeout, Reserve, doppelte Prüfung |
| `ps4app/pin.h` | PIN, Client-Sperren; um Token erweitern |
| `ps4app/main.cpp` | HTTP-Server und HTML/JS als Ausgangspunkt |
| `ps4app/ui.h` | TV-Oberfläche für die Setup-App |
| `upstream-ps5upload/payload/src/webui_server.c` | Token, Host-Allowlist, Verbindungs- und Quelllimits, `Range`, `no-store` als Vorlage |
| `upstream-ps5upload/payload/installer/` | Muster: PKG über Loopback-HTTP an Sonys Installer |
| `upstream-ps5upload/payload/src/pkg_info.c` | Header-/Eintragsprüfung als Gegenprobe |
| [AVA1-C-Node](../AVA1.md) (`phantomptr/ava1` v1.0.11) | Transfer, Resume, Pairing, RPC |

## Ports und Ablage

| Zweck | Wert |
|---|---|
| AVA1 | `9120` (konfigurierbar) |
| HTTP/UI | `9180`, Default Loopback, LAN optional mit Allowlist |
| Zustand | `/data/ps4upload/` (`config.json`, `peers.json`, `queue/`, `logs/`) |
| PKG-Zwischenlager | `/data/ps4upload/pkg/` |
| Payload-Versand | GoldHEN-BinLoader `9090` (nur ausgehend) |

## Abnahmekriterien

- **G0/G0b/G1/G2** auf einer echten PS4 bestanden und mit Firmware,
  GoldHEN-Version, Datum und Rückgabecodes dokumentiert.
- `python ps4port/core/tools/hostcheck.py` bleibt grün.
- PKG-Struktur mit `python tools/ps4remoteplay.py verify …` bzw. dem
  PS4Upload-Verify geprüft, falls ein Paket entsteht.
- Kein Streaming-, `vpad`- oder PS5-Code enthalten.
- Keine Aussage „funktioniert“, die nicht durch ein Gate gedeckt ist.

## Risiken

| Risiko | Wirkung | Gegenmaßnahme |
|---|---|---|
| Payload wird beim Spielstart beendet oder verliert das Netz | hoch | G0 vor allem anderen; sonst Fallback Vordergrund-App |
| AVA1-C-Node läuft auf der PS4 nicht (pthreads, Sockets, Dateisystem) | hoch | Host-Link ist grün, sagt aber nichts über Laufzeit; G0b-Spike; Fallback HTTP-Range plus PIN |
| BGFT akzeptiert unsignierte PKG auf Retail-Firmware nicht | hoch | ehrlicher Fehler, GoldHEN-DPI als Referenz, Hardware-Gate |
| Fortschrittsfelder bleiben auf 0 | mittel | Stillstand über Task-Zustand, nicht nur Bytes |
| Offener HTTP-Dienst | hoch | Token, Loopback-Default, Allowlist, Limits, keine Shell |
| Kein TLS | mittel | dokumentieren; PIN/Token nicht als abhörsicher verkaufen |

## Aufwand

6–10 Wochen für B0–B2 mit vorhandener Testkonsole und GoldHEN-Erfahrung.
Ohne Konsole ist keine der drei Stufen abschließbar.
