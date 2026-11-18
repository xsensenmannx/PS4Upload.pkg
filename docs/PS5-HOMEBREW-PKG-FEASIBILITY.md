# PS5-Homebrew-PKG: Machbarkeit und Architektur

**Stand:** 18. November 2026  
**Status:** Browser-Steuerungsebene umgesetzt; PKG und nativer Streaming-Host noch offen  
**Bezug:** PS5Upload-Konzept (Play Together), PS4Upload mit GoldHEN als getrenntes Projekt  
**Firmware-10.01-Detailaudit:** [PS5-FW1001-SDK-TOOLCHAIN-AUDIT.md](PS5-FW1001-SDK-TOOLCHAIN-AUDIT.md)  
**Umsetzung MVP 0:** [`ps5companion/`](../ps5companion/README.md)

> **Umsetzungsstand:** Die browserbasierte Steuerungsebene ist implementiert und
> getestet: Session, Einladungscode/QR, Gast-Slots, Token-Schutz, Bereitschaft und
> Controller-Input im LAN. Das ist **nicht** die unten beschriebene PKG und **nicht**
> der native Streaming-Host. Capture, Video/Audio und `vpad` auf der PS5 bleiben die
> offenen Hardware-Gates; der Simulator täuscht sie nicht vor.

## 1. Kurzfazit

Eine eigene **PS5-Homebrew-PKG ist technisch machbar**. Eine PKG ist aber nur ein Installations- und Startcontainer. Sie löst weder die Videoaufnahme noch das Encoding, das Hosting der Remote-Play-Session oder die Einladungslogik.

Die ehrliche Einordnung:

| Ziel | Bewertung | Begründung |
|---|---|---|
| Homebrew als PKG bauen und auf PS5 10.01 installieren | **Machbar, aber validierungsbedürftig** | PKG-Builder und Fake-Signing existieren; 10.01 ist nicht automatisch getestet. |
| PS5-PKG als Launcher für den vorhandenen PS5Upload-Helper | **Realistisch** | Der Helper bleibt ELF/Payload; die PKG startet oder verwaltet ihn. |
| PS5-PKG als vollständiger Play-Together-Host ohne PC/Handy | **Forschungsprojekt, kurzfristig nicht realistisch** | Aufnahme/Encoding und die Remote-Play-Host-Session müssten auf die Konsole portiert werden. |
| Realistischer MVP | **Hybrid oder PKG-Wrapper** | Konsolenanteil plus PC/Handy-Engine; echte PKG-only-Funktion erst nach technischen Gates. |

**Kernaussage:** Wer heute Play Together nutzen will, ist mit PS5Upload unverändert besser bedient. Wer eine PKG will, sollte zuerst einen **PKG-Wrapper** bauen und parallel die zwei riskanten Fragen klären: native Capture/Encoding auf 10.01 und stabiler Player-2-Controller. Ein PKG-only-Produkt ist erst gerechtfertigt, wenn diese Spikes erfolgreich sind.

## 2. Begriffe, die auseinandergehalten werden müssen

- **PKG:** Installierbares PS5-Paket (App-Container, Fake-Signierung, Installation über kstuff/fpkg-fähige Umgebung). Eine PKG ist kein Streaming-Server.
- **ELF/Payload:** Über den ELF-Loader (typischerweise Port 9021) geladener Code. Kein Home-Screen-Eintrag, kein normaler App-Lebenszyklus. PS5Upload nutzt diesen Weg.
- **Native PS5-App:** Eine App mit App-Ordner, `eboot.bin`/ELF, Assets und PKG-Verpackung. Sie kann eine eigene Oberfläche und einen eigenen Netzwerkdienst enthalten.
- **Play Together:** Gäste spielen als Player 2–4 mit, während der Besitzer auf der Konsole spielt. Die Gäste brauchen Bild, Ton und Eingabe; die Konsole braucht virtuelle Controller.
- **Host:** Die Instanz, die die Remote-Play-Session hält und das Bild an Gäste weiterleitet. Nicht die Konsole allein, nur weil dort das Spiel läuft.

## 3. Ist-Architektur von PS5Upload

Die Quellcode-Prüfung des geklonten Repositories (v12.6.8) ergibt ein klares Bild:

| Komponente | Ort | Aufgabe |
|---|---|---|
| `payload/` (C, ELF) | PS5 | Residenter Helper, Verwaltung, virtueller Controller (`vpad`), AVA1 auf 9120, Web-UI auf 9180; Installer-Daemon auf 9115. |
| `payload/src/vpad.c` | PS5 | Erzeugt virtuelle Controller für Player 2–4 über `/dev/hid`-IoCTLs. |
| `engine/` (Rust) | PC/Handy | Hält die Remote-Play-Session, verarbeitet Bild/Ton, macht das Play-Together-Hosting und die Einladung. |
| `client/` (Tauri) | PC/Handy | GUI für den Host. |
| `ps5upload-fpkg` | PC | FPKG-Builder für vorbereitete Spielordner (über `ps5upload-engine convert --to pkg`); kein Ersatz für eine native App. |
| `ps5upload-pkg` | PC | Liest/analysiert PKG-Dateien. |

Der entscheidende Beleg steht in `engine/crates/ps5upload-engine/src/join.rs`: Der **Host ist der Computer, auf dem die Remote-Play-Session läuft**. Gäste verbinden sich per Browser/WebSocket; die virtuellen Controller kommen von der Konsole. Die Konsole liefert also den Player-2–4-Input, aber nicht den Host und nicht den Gast-Stream.

Daraus folgt:

1. Eine PKG, die nur den vorhandenen Payload startet, ersetzt den PC/Handy-Engine **nicht**.
2. `vpad` ist bereits konsolenseitig und kann in eine native App übernommen werden.
3. Die Remote-Play-Session, das Gast-Hosting und die Einladung sind heute PC-/Handy-Aufgaben.
4. Ein echter PKG-only-Host müsste diese Aufgaben auf die PS5 verlagern oder vollständig neu implementieren.

## 4. Drei Zielbilder

### A. PKG-Wrapper (MVP 0)

**Idee:** Eine kleine native PS5-App oder ein Installer-Paket, das den vorhandenen Helper startet, Status anzeigt und die Installation/Updates vereinfacht. Der PC/Handy-Engine bleibt der Host.

- **Aufwand:** niedrig bis mittel, ca. 2–4 Wochen.
- **Nutzen:** PKG-Pipeline wird real getestet; kein neues Streaming-Risiko.
- **Grenze:** Kein PKG-only-Play-Together. Ohne PC/Handy passiert kein Gast-Streaming.
- **Empfehlung:** Sinnvoller erster Schritt, wenn das Ziel „eine funktionierende PKG“ und Lernen der Toolchain ist.

### B. Hybrid (realistischer Produktpfad)

**Idee:** Die PS5-App übernimmt Capture, Encoding, lokale Einladung und `vpad`. PC/Handy bleiben nur für optionale Infrastruktur: TURN/Relay, Updates, Diagnose, Erstinstallation. Der Gast nutzt weiterhin einen Browser.

- **Aufwand:** mittel bis hoch, ca. 3–6 Personenmonate für einen engen MVP.
- **Nutzen:** Im LAN ist echtes „PKG-only“-Spielerlebnis möglich; PC/Handy sind nicht im Spielpfad.
- **Grenze:** Internet-Gäste können ohne Relay/TURN scheitern; Firmwareabhängigkeit bleibt.
- **Empfehlung:** Das ist der beste realistische Zielpfad, **wenn** die Capture- und vpad-Gates bestehen.

### C. Echter PKG-only-Host

**Idee:** Alles auf der PS5: Capture, Encoding, HTTP/WebSocket/WebRTC-Server, Einladung, Controller, UI. Kein PC/Handy im Spielbetrieb.

- **Aufwand:** hoch, eher 6–12+ Personenmonate plus laufende Firmware-Pflege.
- **Nutzen:** maximale Unabhängigkeit; kein zweites Gerät.
- **Grenze:** Kein öffentliches PS5-API für den Remote-Play-Host; Capture ist gated; WebRTC/TURN und Multi-Gast-Hosting müssen selbst gebaut werden.
- **Empfehlung:** Als R&D-Projekt behandeln, nicht als „PKG packen“-Aufgabe. Erst nach den Decision Gates in Abschnitt 10 budgetieren.

## 5. Was eine PKG leisten kann – und was nicht

Eine PKG kann:

- eine native App mit Home-Screen-Eintrag installieren,
- eine lokale UI und einen lokalen HTTP-/WebSocket-Dienst starten,
- den vorhandenen Payload/Helper starten oder ersetzen,
- `vpad` und andere konsolenseitige Funktionen integrieren,
- eine lokale Einladung per QR-Code oder Token anzeigen.

Eine PKG kann nicht automatisch:

- die Remote-Play-Session hosten, nur weil sie auf der PS5 läuft,
- Video ohne Capture-/Encoder-Zugriff aufnehmen,
- das PC-/Handy-Engine-Protokoll übernehmen,
- NAT/TURN für Internet-Gäste lösen,
- PS5-System-APIs umgehen, die für Remote Play reserviert sind.

**Wichtig:** Die vorhandene PS5Upload-PKG-/Fake-PKG-Toolchain baut Pakete, aber keinen nativen Streaming-Host. Ein ELF in `.pkg` umzubenennen oder den Payload in eine PKG zu legen, ist kein PKG-only-Host.

## 6. SDK, Toolchain und Hardware

### 6.1 Build-Umgebung

| Baustein | Empfehlung | Zweck |
|---|---|---|
| Host-OS | Windows 11 + WSL2 (Ubuntu 24.04) oder natives Linux | PS5-Toolchain, Build, Debugging. |
| Compiler | LLVM/Clang 18, LLD | PS5-Payload-SDK und native App. |
| Build | CMake, Ninja, Meson, Make | Reproduzierbare Builds. |
| Python | Python 3 + `pyelftools` | SDK-Skripte, ELF-Verarbeitung. |
| Versionskontrolle | Git | PS5Upload-Referenz und eigene Änderungen. |
| PS5-SDK | PS5 Payload SDK (ps5-payload-dev), Stand v0.45 | ELF-Payloads, native Bibliotheken. |
| Native App | PS5-Native-App-Boilerplate bzw. Homebrew-Starter | App-Ordner, `eboot.bin`, PKG-Grundlage. |
| PKG | `ps5upload-fpkg`, LibProsperoPKG (.NET 10) oder ps5-ultrapack | Fake-PKG bauen/prüfen; App-Ordner verpacken. |
| Laufzeit | kstuff/fpkg-fähige Umgebung, ELF-Loader 9021, ggf. ShadowMount+ | Installation und Start auf 10.01. |
| Client | Browser (WebCodecs/MSE, Gamepad API) | Gäste ohne Android-App. |

**Wichtig:** Die genannten Versionen sind eine Planungsgrundlage, keine Garantie für 10.01. Die PKG-Tools müssen auf genau dieser Firmware getestet werden; ps5-ultrapack ist beispielsweise auf 11.60 getestet, nicht auf 10.01.

### 6.2 Hardware

- **PS5 mit Firmware 10.01**, Relapse-tauglich (nach deinem Stand). Das ist die Zielkonsole, kein Beweis für Capture-/PKG-Unterstützung.
- **Mindestens zwei Controller** für Player 2 und lokale Tests.
- **PC** für Build, Installation, Logs und als Vergleichshost.
- **Smartphone/Tablet** als Browser-Gast und für QR-Tests.
- **LAN** (Kabel bevorzugt) für reproduzierbare Latenz; Internet erst nach LAN-MVP.
- **Testspiele** mit lokalem Koop/Shared Screen, damit Player 2 sinnvoll prüfbar ist.
- Optional: Capture-Karte für Vergleichsmessungen, zweite Konsole für RegressionsTests.
- **Nicht nötig:** PS5-Devkit, Android-SDK, PSN-Account für den Gast.

Die PS4 Pro mit GoldHEN bleibt für PS4Upload relevant, aber nicht für die PS5-PKG. PS4- und PS5-Payload-Welten nicht vermischen.

## 7. Realistischer MVP

### MVP 0: PKG-Wrapper (empfohlener erster Schritt)

> Die bereits umgesetzte Browser-Steuerungsebene (`ps5companion/`) ist eine
> Vorstufe für diesen Schritt, nicht der PKG-Wrapper selbst.

**Ziel:** Beweisen, dass eine eigene PS5-App als PKG auf 10.01 installiert und gestartet wird, und den vorhandenen Helper komfortabel starten.

**Umfang:**

- native Minimal-App mit Statusseite,
- Start/Stop des vorhandenen Helpers,
- Anzeige der lokalen IP und des Helper-Status,
- PKG-Build und Installation,
- klare Fehlermeldung, wenn der Helper fehlt.

**Nicht im Umfang:**

- eigenes Streaming,
- eigener Capture/Encoder,
- PKG-only-Gäste,
- Änderungen am PC-/Handy-Engine.

**Dauer:** ca. 2–4 Wochen.  
**Exit:** PKG installiert und startet auf 10.01; Helper ist erreichbar; ein Gast kann weiterhin über den bestehenden PC/Handy-Host spielen.

### MVP 1: Nativer PKG-Host (R&D)

**Ziel:** Eine PS5-App als PKG, die im LAN **einen** Gast ohne PC/Handy im Spielpfad bedient.

**Umfang:**

- native App mit lokaler UI und QR-Einladung,
- Capture auf der PS5,
- H.264-Video und Audio,
- Browser-Gast mit Video, Audio und Eingabe,
- `vpad` für Player 2,
- ein Testspiel, eine Auflösung (720p), 30 fps,
- 30-Minuten-Soak-Test,
- Token-/Session-Schutz, keine offene Management-API.

**Nicht im Umfang:**

- Player 3/4,
- Internet-Gäste ohne Relay,
- 1080p60/4K,
- Party-Chat, Aufnahme, Streaming zu Twitch,
- Komfortfunktionen wie automatische Updates.

**Dauer:** ca. 3–6 Personenmonate, abhängig von Wiederverwendung der Capture-/vpad-Ansätze.  
**Exit:** Ein Gast spielt 30 Minuten stabil im LAN; Player 2 funktioniert in mindestens drei Testspielen; Latenz und Bildqualität sind akzeptabel; die App lässt sich reproduzierbar als PKG bauen und starten.

### MVP 2: Produkt

- 1080p60, mehrere Gäste, bessere UI,
- Internet über TURN/Relay oder Tailscale,
- Update-/Diagnosepfad,
- Firmware-Matrix und Regressionstests,
- rechtliche und sicherheitsrelevante Härtung.

## 8. Was auf PC oder Handy bleiben muss

### MVP 0

| Aufgabe | PS5-PKG | PC/Handy |
|---|---|---|
| Paketinstallation/Start | ja | Build/Installation |
| Helper-Status | ja | optional |
| Remote-Play-Host | nein | **ja** |
| Capture/Encoding | nein | **ja** |
| Einladung/Signaling | nein | **ja** |
| Gast-UI | nein | Browser |

### MVP 1

| Aufgabe | PS5-PKG | PC/Handy |
|---|---|---|
| Capture/Encoding | **ja** | nein |
| `vpad` Player 2 | **ja** | nein |
| Lokale Einladung/Token | **ja** | optional |
| Gast-Stream | **ja** (Browser) | nein |
| Internet-Relay/TURN | nein | optional |
| Build/Installation/Logs | nein | **ja** |
| Updates/Diagnose | optional | optional |

### Langfristig

- **Browser-Gast:** bleibt sinnvoll; keine Android-App nötig.
- **TURN/Relay:** kann ein kleiner Cloud-/PC-Dienst bleiben, wenn Internet-Gäste unterstützt werden sollen.
- **Updates/Logs:** können optional auf PC bleiben; für ein Produkt besser in die App integrieren.
- **Paketbau:** bleibt immer auf PC (Build-Pipeline), nicht auf der Konsole.

**Kurz:** Im Near-Term-MVP bleibt der PC/Handy-Engine der Host. Im realistischen Hybrid-MVP verschwindet er aus dem Spielpfad, bleibt aber für Build, Installation und optionale Netzwerkdienste. Ein vollständig PC-freies Produkt ist erst nach erfolgreichen Gates sinnvoll.

## 9. Phasen und Aufwand

Die Schätzungen sind Größenordnungen für **eine erfahrene Entwicklerin/einen erfahrenen Entwickler mit Testkonsole**, keine Festpreise.

| Phase | Inhalt | Dauer |
|---|---|---|
| 0. Feasibility | Toolchain, Hello-World-PKG auf 10.01, Capture-Spike, vpad-Spike | 2–4 Wochen |
| 1. PKG-Wrapper | Native Minimal-App, Helper-Start, Status, PKG-Pipeline | 2–4 Wochen |
| 2. Stream-Kern | Capture, Encoding, Audio, Browser-Client, Latenzmessung | 4–8 Wochen |
| 3. Spielbarkeit | Player 2, Einladung, Token, ein Testspiel, 30-Minuten-Soak | 4–8 Wochen |
| 4. Härtung | Fehlerfälle, Reconnect, Logs, Reproduzierbarkeit, Doku | 4–8 Wochen |

**Gesamt MVP 1:** grob 3–6 Personenmonate.  
**Produktreife:** grob 6–12+ Personenmonate plus laufende Firmware-Pflege.  
**Wichtigster Kostentreiber:** nicht die PKG, sondern Capture/Encoding und die Stabilität über Firmware-/Spielkombinationen.

## 10. Risiken und Gegenmaßnahmen

| Risiko | Wahrscheinlichkeit | Auswirkung | Gegenmaßnahme |
|---|---|---|---|
| Capture/Encoder auf 10.01 nicht nutzbar | hoch | sehr hoch | Frühzeitig Spike; Fallback PC/Handy-Engine; keine Full-Build-Investition vor Gate. |
| `libSceAvcap2`/Remote-Play-Interna firmwareabhängig | hoch | hoch | Nur getestete Firmware unterstützen; klare Matrix; kein Update-Zwang. |
| PKG-Installation/Start auf 10.01 scheitert | mittel | hoch | Hello-World-PKG zuerst; kstuff-/Tool-Versionen dokumentieren; ELF-Fallback. |
| `vpad` funktioniert nicht in allen Spielen | mittel | mittel | Drei Testspiele; Player-2-Fokus; Fallback lokaler Controller. |
| Latenz/Bildqualität unzureichend | hoch | mittel | LAN, 720p30, Hardware-Encoding, Messungen; kein 4K-Versprechen. |
| Internet/NAT komplexer als gedacht | hoch | mittel | Internet aus MVP 1 streichen; TURN/Relay später. |
| Rechtliche/GPL-Probleme | mittel | hoch | GPL-3.0 einhalten, Quellen veröffentlichen, keine Sony-Dateien weitergeben, keine Spiel-PKG. |
| Sicherheitslücke im Gast-/Management-Zugang | mittel | hoch | Token, lokale Bindung, keine offene Admin-API, Rechte pro Gast. |
| Scope Creep (4 Spieler, 4K, Chat) | hoch | mittel | MVP-Grenzen und Gates strikt einhalten. |
| Wartung bricht bei neuer Firmware | hoch | mittel | Hardware-Teststand, CI soweit möglich, dokumentierte Offsets/Patches. |

### Recht und Plattform

- PS5Upload steht unter **GPL-3.0**; eine Weitergabe oder Ableitung muss die Lizenz beachten.
- Fake-Signierung ist für Homebrew gedacht, nicht für raubkopierte Spiele. Keine Spiel-PKG erstellen oder verteilen.
- Jailbreak/Relapse kann gegen Nutzungsbedingungen verstoßen; PSN-/Account-Risiken sind real.
- Keine proprietären Sony-SDK-Dateien in das Projekt aufnehmen.
- Der Gastzugang ist ein Netzwerkdienst: standardmäßig nur LAN, explizite Freigabe, keine offene Admin-Fläche.

## 11. Decision Gates

Keine größere Implementierung vor diesen Gates. Jedes Gate hat einen klaren Fallback.

| Gate | Frage | Bestanden wenn | Fallback |
|---|---|---|---|
| **G0: PKG** | Lässt sich eine minimale Homebrew-App als PKG auf 10.01 installieren und starten? | Reproduction auf einer frisch gebooteten Konsole; Start über Home-Screen. | ELF-Payload statt PKG; keine PKG-Versprechen. |
| **G1: Capture** | Liefert die PS5 auf 10.01 30 Minuten lang stabiles Bild/ Audio? | 720p30, keine Crashes, akzeptable Qualität. | PC/Handy-Engine bleibt Host; Hybrid nur mit PC. |
| **G2: vpad** | Funktioniert Player 2 in mindestens drei lokalen Koop-Spielen? | Eingaben stabil, keine Desyncs, 30 Minuten. | Kein Koop-MVP; zurück zu PS5Upload/PC. |
| **G3: Stream** | Browser-Gast sieht/hört/spielt mit akzeptabler Latenz? | LAN, 30 Minuten, Reconnect ohne Neustart. | Nativer Client oder PC/Handy-Streaming. |
| **G4: Invite/Security** | Einladung, Token und Zugriff sind sauber begrenzt? | Kein offener Admin-Zugriff; Gast nur nach Freigabe. | Einladung/Signaling auf PC/Handy. |
| **G5: Soak** | Läuft der PKG-only-Host 30 Minuten mit einem Gast? | Keine Speicherlecks/Crashes; kontrolliertes Ende. | Hybrid statt PKG-only. |

**Wenn G0 oder G1 fehlschlägt, ist ein PKG-only-MVP nicht gerechtfertigt.** Dann ist der ehrliche Weg: PKG-Wrapper plus bestehende PC/Handy-Engine.

## 12. Empfehlung

1. **Wenn das Ziel „jetzt spielen“ ist:** PS5Upload unverändert nutzen. Eine PKG fügt in dieser Phase keinen funktionalen Wert hinzu.
2. **Wenn das Ziel „eigene PS5-Homebrew-PKG lernen“ ist:** MVP 0 bauen: minimale native App, Helper-Start, Status, PKG-Installation. Das ist ein überschaubares, ehrliches Teilprojekt.
3. **Wenn das Ziel „PKG-only Play Together“ ist:** Zuerst Phase 0 finanzieren, nicht die ganze App. Die zwei kritischen Spikes sind Capture/Encoding auf 10.01 und `vpad` mit Player 2. Erst danach MVP 1 planen.
4. **Architektur nicht verwässern:** Die PKG darf nicht als „fertig“ gelten, solange der PC/Handy-Engine den Host macht. Das muss in UI und Doku klar stehen.
5. **PS4Upload nicht mit PS5-PKG vermischen.** Die PS4-Pro-/GoldHEN-Lösung bleibt ein separates Produkt.

**Empfohlene Entscheidung:** Mit **MVP 0 + Phase-0-Spikes** starten. Danach anhand von G0–G2 entscheiden, ob ein Hybrid-MVP oder ein PKG-only-R&D-Projekt weiterverfolgt wird. Kein Full-Build vor diesen Gates.

## 13. Quellen und Referenzen

- PS5Upload-Repository: <https://github.com/phantomptr/ps5upload>
  - `payload/README.md`: ELF-Helper, AVA1 auf 9120, Web-UI 9180, Installer 9115, SDK-Build.
  - `payload/src/vpad.c`: virtuelle Controller über `/dev/hid`.
  - `engine/crates/ps5upload-engine/src/join.rs`: Host ist der Computer mit der Remote-Play-Session.
  - `ps5upload-fpkg`: Fake-PKG-Erstellung, u. a. `convert --to pkg`.
- PS5 Payload SDK: <https://github.com/ps5-payload-dev/sdk>
- PS5-Native-App-Boilerplate und Homebrew-Starter: als Grundlage für App-Ordner/`eboot.bin` prüfen.
- LibProsperoPKG (C#/.NET 10): PKG/GP5/PFS bauen und inspizieren; Fake-Signierung.
- ps5-ultrapack: Fake-PKG-Toolchain für Windows/macOS; auf 11.60 getestet, **nicht** auf 10.01.
- Glint (PS5-Payload): nutzt `libSceAvcap2` über eine SceRemotePlay-Injection; laut Projekt auf 13.60 getestet, nicht auf 10.01. Wichtiger Referenzpfad für Capture, aber firmwareabhängig und kein öffentliches API.
- PS5-Moonlight: Referenz für Videoausgabe aus einem Payload (`sceVideoOutRegisterBuffers2`).
- kstuff/fpkg-enable, ELF-Loader 9021, ShadowMount+: Laufzeit-/Installationswerkzeuge; Versionen für 10.01 separat validieren.

**Versionshinweis:** Alle Tool- und Firmware-Aussagen sind Stand November 2026. Vor der Umsetzung müssen die konkreten Versionen auf der vorhandenen PS5 10.01 reproduziert werden.
