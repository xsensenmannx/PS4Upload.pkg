# Option A – Voller Port: PS5Upload-Funktionsumfang auf der PS4

**Stand:** 20. November 2026  
**Basis:** [Upstream-Pin v12.6.16](../UPSTREAM.md) · [AVA1-Pin v1.0.11](../AVA1.md) · [gemeinsamer Kern](../core/README.md) · [Option B](../option-b-payload/README.md)  
**Status:** Plan; setzt auf dem Payload aus B auf, kein eigener Fork

## Ziel

Die PS4 bekommt denselben Verwaltungsumfang wie PS5Upload – Upload,
Installation, Bibliothek, Dateien, Start/Stop, Deinstallation, Cheats,
Payload-Versand und Power – und lässt sich später mit derselben Engine bzw.
demselben Client bedienen. Streaming bleibt ausdrücklich draußen.

A ist **keine zweite Codebasis**. Der Payload aus B wird erweitert; getrennt
sind die Projekte, weil Scope, Risiken und Abnahmekriterien unterschiedlich
sind.

## Was A zusätzlich zu B enthält

- AVA1-Management-Methoden für Status, Upload, Installation und Konsole
- Spielebibliothek aus `app.db`, `/user/app` und `/user/appmeta` (nur lesen)
- Dateimanager für `/data`, `/user` und USB: Liste, Kopieren, Verschieben,
  Löschen
- Start/Stop über `sceLncUtilLaunchApp` bzw. `sceSystemServiceKillApp`
- Deinstallation über `sceAppInstUtilAppUnInstall*`
- Cheat-Dateien unter `/data/GoldHEN/cheats/` verwalten (keine Engine)
- Payload-Liste und -Versand über GoldHEN-BinLoader `9090`
- Power-Aktionen und Temperatur, soweit auf der Ziel-Firmware verfügbar
- Saves nur lesen/listen; Backup/Restore bleibt ein eigener Spike
- Anbindung an die PS5Upload-Engine und den bestehenden Client

## Modulkarte (Upstream → PS4)

| Upstream-Datei | Verwendung auf der PS4 |
|---|---|
| `payload/src/main.c`, `runtime.c`, `takeover.c` | Lebenszyklus, Übernahme eines alten Payloads, Aufräumen |
| `payload/src/mgmt_table.def`, `mgmt_rpc.c`, `mgmt_*.c` | RPC-Tabelle und Dispatch; PS5-Methoden klar als „unsupported“ beantworten |
| `payload/src/pkg_info.c`, `payload/installer/` | PKG-Prüfung und Loopback-Installation als Gegenprobe |
| `payload/src/content_db.c`, `payload/include/appdb_scan.h` | SQLite-Reader und Schema-Erkennung für `app.db` |
| `payload/src/webui_server.c`, `webui_auth.c`, `webui_http.c` | Token, Allowlist, Limits, `Range`, `no-store` |
| `payload/src/plog.c`, `notif.c` | Logring und Benachrichtigungen |
| `protocol/ps5upload-ava1/MGMT_METHODS.md`, `app.toml` | Methodenvertrag für die Engine |
| `engine/crates/ps5upload-ava1` | Erwartungen der Engine an Antworten und Jobs |
| `engine/crates/ps5upload-core` | Verhalten von Upload, Installation, Dateien, Bibliothek |
| `client/src` | UI-Vertrag; nur nutzen, wenn die Engine kompatibel antwortet |

**Nicht übernehmen:** `payload/src/vpad.c`, Remote-Play-/Streaming-Teile,
PS5-spezifische Decoder- und Kernel-Pfade, das Release-ELF.

## Meilensteine

| Stufe | Inhalt | Gate |
|---|---|---|
| **A0** | identisch mit B0: Payload-Spike, HTTP, AVA1-Grundverbindung | G0/G0b |
| **A1** | identisch mit B1 plus AVA1-Management für Status, Upload, Install | G1 |
| **A2** | Bibliothek lesen, Dateien, Start/Stop, Deinstallation | **G2** drei Spiele starten/stoppen, keine `app.db`-Korruption, Dateioperationen überleben Neustart |
| **A3** | Cheats-Verwaltung, Payload-Versand, Power, Temperatur, Saves lesen | **G3** 30-Minuten-Soak mit Spielstart, Dateizugriff und UI ohne Absturz |
| **A4** | Engine-/Client-Anbindung | **G4** Engine verbindet sich mit der PS4, Upload + Installation + Bibliotheksliste funktionieren durch den Client; nicht unterstützte PS5-Methoden liefern klare Fehler |

## Engine- und Client-Anbindung

Die Engine spricht AVA1 mit dem Payload auf `:9120` und bietet nach außen HTTP
auf `:19113`. Der Client spricht nur mit der Engine. Damit gibt es zwei Wege:

1. **Unveränderte Engine gegen den PS4-Payload.** Bevorzugt, weil der Client
   dann ohne Fork funktioniert. Voraussetzung: Der Payload beantwortet die
   Methoden, die die Engine tatsächlich aufruft, mit dem erwarteten Schema.
   PS5-only-Methoden (Streaming, `vpad`, Konvertierung) antworten explizit
   `unsupported`, nicht mit erfundenen Daten.
2. **Engine-Fork mit PS4-Profil.** Nur falls die Engine PS5-Annahmen fest
   eingebaut hat. Dann bleibt der Fork ein eigenes Projekt und der Client
   bekommt eine klare Plattformkennung.

Der Client wird nicht ungeprüft übernommen: Seine Navigation und viele
Ansichten setzen PS5-Funktionen voraus. Für A zählt zuerst, dass die
Kernaufgaben über die Engine laufen; die UI kann schrittweise folgen.

**Lizenzgrenze:** Engine und Client stehen unter GPL-3.0; die
Streaming-Bestandteile unter AGPL-3.0. A importiert keine Streaming-Module und
startet keine Capture-/Encoder-Pfade. Wer den Client binaryseitig weitergibt,
prüft die Hinweise der mitgelieferten Teile.

## Abnahmekriterien

- A0–A4 mit dokumentierten Hardware-Gates; jede Aussage nennt Firmware,
  GoldHEN-Version und Datum.
- `app.db` wird ausschließlich lesend geöffnet; Schreiben erst nach eigenem
  Backup- und Transaktionskonzept.
- Nicht unterstützte Methoden liefern `unsupported` mit Methode und Grund.
- Kein Streaming, kein `vpad`, keine PS5-Binaries im Paket.
- `python ps4port/core/tools/hostcheck.py` bleibt grün; neue reine Logik
  bekommt Hosttests, bevor sie auf die Konsole geht.

## Bewusste Nicht-Ziele

- Streaming/„Play“: Die PS4 ist Empfänger für chiaki, nicht Host.
- PS5-Unterstützung: Dafür bleibt PS5Upload zuständig.
- Spiel-Konvertierung, Dumps, Save-Editor, PSN-Integration.
- Internet-Erreichbarkeit ohne VPN; Tailscale läuft höchstens über einen
  PC im Netz.
- Stille Schreibzugriffe auf Systemdatenbanken.

## Risiken

| Risiko | Wirkung | Gegenmaßnahme |
|---|---|---|
| Engine erwartet PS5-spezifische Antworten | hoch | Methoden inventarisieren, `unsupported` sauber beantworten, A4 als eigenes Gate |
| `app.db`-Schreibzugriff beschädigt die Bibliothek | hoch | nur lesen; Schreiben nur mit Backup und Transaktion |
| Start/Stop verweigert Homebrew-Titel | mittel | beide Launch-APIs testen; notfalls Start nur aus dem PS4-Menü |
| Firmwareunterschiede bei BGFT und APIs | hoch | Firmware-Matrix, Testcheckliste, keine Pauschalaussagen |
| Scope Creep durch Client-UI | hoch | A4 erst nach G2/G3; Kernaufgaben vor Optik |
| AGPL-/GPL-Vermischung | hoch | Streaming außerhalb, Hinweise mitführen, keine AGPL-Binaries einbetten |

## Aufwand

3–5 Monate für A0–A4 mit Testkonsole und Erfahrung mit der PS4-SDK. Der
Unterschied zu B liegt nicht im Transfer, sondern in Bibliothek, Start/Stop,
Dateien und der Engine-Kompatibilität.
