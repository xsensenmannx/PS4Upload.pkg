# PS4-Port von PS5Upload: zwei getrennte Projekte

**Stand:** 21. November 2026  
**Upstream-Pins:** [`phantomptr/ps5upload` v12.6.16](UPSTREAM.md), [`phantomptr/ava1` v1.0.11](AVA1.md)  
**Status:** Pläne, host-getesteter Kern, gebautes Payload-Gerüst (`payload/ps4/`, HTTP `/status`, `/upload`, `/files`), gepinnter AVA1-C-Node und kombinierter B0-Link-Spike; keine Hardwareprüfung

Der Port ist nicht mehr eine Entscheidung, sondern zwei eigenständige Wege.
Beide nutzen denselben gepinnten Quellstand und denselben Payload-Kern, teilen
sich aber weder Meilensteine noch Abnahmekriterien. **Option A ist kein Fork
von B**, sondern erweitert denselben Payload nach bestandener Basis.

## Die beiden Projekte

| | [Option B – Payload zuerst](option-b-payload/README.md) | [Option A – Voller Port](option-a-full/README.md) |
|---|---|---|
| **Ziel** | Upload und Installation im Hintergrund, kleine eigene Web-UI | PS5Upload-Funktionsumfang auf der PS4, inklusive Engine-/Client-Anbindung |
| **Enthält** | Payload, AVA1-Transfer, Queue, PKG-Prüfung, BGFT, Token, Mini-UI, TV-Setup-App | B plus Bibliothek, Dateien, Start/Stop, Deinstallation, Cheats-Verwaltung, Power, Engine-RPC |
| **UI** | mitgelieferte Mini-Web-UI; TV-App nur Start/Status | optional die PS5Upload-Engine mit bestehendem Client oder die eigene UI |
| **Erster Nutzen** | nach G0/G1: PKG hochladen und installieren, während gespielt wird | nach A2: Konsole auch verwalten, nicht nur beliefern |
| **Aufwand (Größenordnung, mit Testkonsole)** | 6–10 Wochen | 3–5 Monate |
| **Hauptrisiko** | G0: überlebt der Payload den Spielstart? | Engine erwartet PS5-Methoden; AGPL-Grenze beim Streaming |
| **Start** | sofort mit G0 | nach B1, auf demselben Payload |

**Empfehlung:** B zuerst. Es bringt den heutigen Wert (Upload + Installation)
auf die PS4 und liefert die Hardware-Antwort, auf der A aufsetzt. A bleibt
trotzdem vollständig beschrieben, damit die Entscheidung nicht erneut gestellt
werden muss.

## Gemeinsame Basis

```text
ps4port/
├── UPSTREAM.md              Pin, Lizenz, was aus PS5Upload übernommen wird
├── AVA1.md                  Pin des AVA1-C-Nodes (v1.0.11) und Portabilität
├── B0-STATUS.md             SDK-/Spike-Stand und Prüfgrenzen
├── ava1/                    PS4-Plattform-Shim für den AVA1-C-Node
├── core/                    host-getesteter Payload-Kern (C11, ohne PS4-Header)
├── option-b-payload/        Projekt B: Umfang, Meilensteine, Gates
└── option-a-full/           Projekt A: Umfang, Meilensteine, Gates, Modulkarte
```

Der gepinnte Quellstand liegt lokal unter `upstream-ps5upload/` (in
`.gitignore`, 280 MB mit Historie) und lässt sich über
[`UPSTREAM.md`](UPSTREAM.md) neu erzeugen. AVA1 ist eine zweite, eigene
Quelle: Sie liegt unter `upstream-ava1/`, ist in [`AVA1.md`](AVA1.md) gepinnt
und wird nicht aus dem PS5Upload-Snapshot bezogen.

## Grenze zwischen A und B

Die Trennung ist eine Schichtgrenze, keine zweite Codebasis:

| Ebene | Gehört zu | Regel |
|---|---|---|
| `core/` | beiden | nur Parsing, JSON, Dateinamen und Quotenlogik, die B und A gleich brauchen; keine UI, kein BGFT, keine A-Methoden |
| `payload/ps4/` | B | der eine Payload; A erweitert ihn, statt ihn zu kopieren |
| A-spezifische Module | A | Bibliothek, Dateimanager, Start/Stop, Cheats, Power; eigener Modulordner und eigener Build-Schalter, damit B sie nicht mitlinkt |

Heute existiert nur der B-Build. A ist ein Plan, kein zweites Artefakt;
solange kein A-Build existiert, darf A-Code nicht unbemerkt im B-Payload
landen. Änderungen an `core/` müssen den Hosttest für beide Projekte grün
halten.

## Regeln für beide Projekte

- **GPL-3.0 einhalten.** Übernommener Code behält seine Hinweise; Quellen und
  Änderungen werden mitgeliefert. AVA1 ist Apache-2.0/MIT und in
  [`AVA1.md`](AVA1.md) gepinnt.
- **Kein Streaming, kein `vpad`, kein PS5-Binary.** Die PS4 bleibt Empfänger
  für chiaki, nicht Host. Die AGPL-3.0-Teile aus PS5Upload bleiben außerhalb.
- **Keine Hardware-Aussage ohne Hardware.** „Gebaut“ und „auf dem PC geprüft“
  sind keine Lauffähigkeitsnachweise.
- **Ports:** AVA1 `9120`, HTTP/UI `9180` (Default Loopback, LAN nur mit
  Host-Allowlist), Zustand unter `/data/ps4upload/`.
- **Sicherheit:** Token im Header, Quelllimits, `no-store`, keine
  Shell-Endpunkte, keine offene Bindung ohne Allowlist.
- **JSON:** `snake_case`, jeder String maskiert, keine geratenen Felder.

## Prüfen

```bash
python ps4port/core/tools/hostcheck.py
python ps4port/tools/ava1_portcheck.py
python ps4port/tools/b0_link_spike.py
```

Das sind die drei Prüfschritte, die heute ohne PS4 laufen. Der zweite
kompiliert und linkt den AVA1-C-Node gegen die ps4-payload-sdk, der dritte
linkt ihn zusammen mit dem HTTP-Payload. Keiner führt den Node aus; alles
Weitere beginnt mit dem G0-Spike aus
[Option B](option-b-payload/README.md#b0--payload-spike-g0).

## Was hier ausdrücklich noch nicht steht

- kein Test auf einer PS4, keine Aussage zu GoldHEN-/Firmware-Kombinationen
  (das ELF aus [`../payload/ps4`](../payload/ps4/README.md) ist gebaut, aber
  nie geladen worden)
- AVA1-C-Node gepinnt und gegen die SDK gelinkt; der B0-Link-Spike enthält
  ihn, startet den Server aber nicht. Nie auf einer PS4 ausgeführt (G0b offen)
- keine BGFT-Installation aus dem Payload
- keine Engine-/Client-Verbindung

Diese Lücken sind die Meilensteine, nicht Fußnoten.
