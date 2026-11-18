# PS5 Firmware 10.01: SDK-, Toolchain- und PKG-Audit

**Stand:** 18. November 2026  
**Ziel:** Prüfen, welche offenen PS5-Homebrew-SDKs und Toolchains für Firmware 10.01 verfügbar sind und ob damit eine native Streaming-/Controller-PKG gebaut werden kann.  
**Ergebnis:** Dokumentation der Lücken und Voraussetzungen, keine Umsetzung.

## 1. Kurzfassung

**Ja, eine PS5-Homebrew-App und ein fake-signiertes PKG lassen sich mit offenen Tools bauen. Nein, eine einzelne native PKG, die auf 10.01 selbst Capture, virtuelle Controller und LAN-Streaming übernimmt, ist damit nicht belegt.**

Die drei Ebenen müssen getrennt werden:

| Ebene | Status für 10.01 | Bewertung |
|---|---|---|
| Exploit und ELF-Loader | Relapse unterstützt 10.01 ausdrücklich | **vorhanden** |
| SDK und Kompilieren | PS5 Payload SDK v0.45 kennt 10.01 explizit | **vorhanden** |
| Native App bauen | Toolkits vorhanden, aber 10.01 nicht getestet | **plausibel, ungeprüft** |
| PKG/fPKG bauen | Packager vorhanden; Paketbau ist firmwareunabhängig | **machbar** |
| PKG auf 10.01 installieren/starten | kstuff listet 10.01, aber es gibt 10.01-Fehlerberichte | **schwächste Stelle** |
| Capture auf 10.01 | Referenzen nur 10.20/10.40 bzw. 13.00/13.60 | **ungeprüft** |
| Virtuelle Controller auf 10.01 | `/dev/hid`-Pfad, 13.00/10.20 getestet | **ungeprüft** |
| Netzwerkdienst aus einer normalen PKG-App | Title-Sandbox kann Binden verweigern | **kritische Lücke** |
| Alles in einer PKG | kein Projekt belegt das | **Forschungsprojekt** |

**Wichtigste Architekturerkenntnis:** Die vorhandenen nativen Streaming-Projekte trennen die **Homebrew-Title-App** von einem **separaten ELF-Host**. Die App ist die Oberfläche; der ELF-Host macht Capture, virtuelle Controller und Netzwerk. Das ist kein Zufall: Ein normaler Title läuft in einer Sandbox und darf nach den bisherigen Beobachtungen nicht ohne Weiteres auf `/dev/hid` zugreifen oder einen LAN-Port binden.

**Realistischer Weg:** Eine PKG als UI/Installer bauen, die einen gebündelten ELF-Host über Port 9021 startet. Das ist eine „PKG-verpackte“ Lösung, aber kein einzelner PKG-Prozess. Wer wirklich „eine PKG, kein ELF“ will, muss zuerst die Title-Elevation für Netzwerk und `/dev/hid` auf 10.01 beweisen. Dafür gibt es derzeit keinen Beleg.

## 2. Bewertungsmaßstab

Eine „native Streaming-/Controller-PKG“ kann Verschiedenes bedeuten:

1. **PKG als Installer/UI:** Eine native App wird als PKG installiert und startet einen vorhandenen Host.
2. **PKG als UI + gebündelter ELF-Host:** Die PKG enthält die Oberfläche und den Host als ELF; der Host wird zur Laufzeit über den ELF-Loader gestartet.
3. **PKG als einziger Prozess:** Die native App selbst captured, erzeugt virtuelle Controller und serviert das Netzwerk.
4. **PKG als Wrapper für PC/Handy:** Die PKG startet nur den vorhandenen PS5Upload-Helper; der Host bleibt auf PC/Handy.

Dieses Audit bewertet alle vier. „Machbar“ heißt im Folgenden: mit offenen, öffentlich verfügbaren Tools baubar. „Auf 10.01 belegt“ heißt: auf genau dieser Firmware reproduziert, nicht nur in einer Versionsliste genannt.

## 3. Was für 10.01 verfügbar ist

### 3.1 Exploit und Laufzeit

| Werkzeug | Stand | 10.01-Relevanz |
|---|---|---|
| **Relapse** (ntfargo) | öffentlich seit September 2026; Firmware 7.00–13.60 | 10.01 ist in der Firmware-Allowlist enthalten. Liefert Kernelzugriff und den ELF-Loader auf Port 9021. |
| **kstuff-lite 1.11** (EchoStretch) | 20. September 2026 | Firmware 1.00–13.60; FPKG laut Changelog bis 11.40, mit Fix für 11.60. 10.01 liegt im Bereich, ist aber nicht als eigener Test aufgeführt. |
| **ShadowMountPlus** | separat | Montiert Ordner/`.ffpfs`/`.ffpfsc`; laut Nutzerberichten auf 10.01 nutzbar. Kein PKG-Ersatz, aber ein Fallback für native Title-Ordner. |
| **Payload Manager / ELF-Loader** | separat | Startet ELFs und verwaltet Autoload. Der Loader auf 9021 ist Teil des Relapse-Ablaufs. |

Relapse ist der Exploit, nicht das Homebrew-Runtime-Paket. Deshalb ist „10.01 ist jailbreakbar“ **nicht** gleich „10.01 kann fPKGs installieren und starten“. kstuff, FPKG-Patches und ShadowMountPlus sind separate Komponenten und müssen zur Firmware passen.

**10.01-Berichte:** Es gibt mindestens einen öffentlichen Bericht zu einer PS5 Pro auf 10.01, bei dem kstuff-lite 1.11 zu einem Kernel-Panic führte und installierte PS5-fPKGs nicht starteten; mit kstuff-lite 1.07 scheiterten sie mit `CE-100096-6`, während ShadowMountPlus funktionierte. Das ist ein Einzelbericht und kein kontrollierter Test, aber er zeigt genau die Lücke: **Der Exploit kann laufen, während der fPKG-Launch scheitert.**

### 3.2 SDK und Compiler

| Werkzeug | Stand | Ergebnis |
|---|---|---|
| **PS5 Payload SDK** (ps5-payload-dev) | **v0.45**, veröffentlicht 21. Oktober 2026; GPL-3.0 | Offenes SDK für ELF-Payloads; dynamisches Linken, C/C++-Runtime, SCE-Stubs, Kernel-Helfer. |
| **LLVM/Clang 18 + LLD** | Voraussetzung des SDK | Compiler und Linker für PS5-Ziele. |
| **PacBrew-Ports** | separate Paketquelle | Vorkompilierte Bibliotheken (u. a. SDL2, FFmpeg, OpenSSL, curl) für native Apps. Keine Firmware-Garantie. |
| **PS5SDK** (PS5Dev) | ältere, offene Alternative | WIP, überwiegend ELF; laut Projekt keine vollständigen Apps. Kein empfohlener Native-PKG-Pfad. |

**10.01-Beleg im SDK:** `crt/kernel.c` enthält einen expliziten Fall für `0x10010000` (Firmware 10.01) mit eigenen Kernel-Offsets. Das SDK ist damit nicht nur „irgendwie abwärtskompatibel“, sondern kennt die Version. Der Release v0.45 ist über die GitHub-API bestätigt; die gerenderte Release-Liste zeigte zum Prüfzeitpunkt noch ältere Einträge.

Das SDK erzeugt **ELF-Payloads bzw. native Programme**, aber keine PKG. Es ist die Grundlage; die Verpackung ist ein separater Schritt.

### 3.3 Native-App-Toolkits

| Toolkit | Ausgabe | Getestet | PKG? |
|---|---|---|---|
| **ps5-native-app-boilerplate** (blackbearreloaded) | C++20-App-Ordner + ZIP, `eboot.bin`, `sce_sys`, Assets | 6.02 und 12.70; **nicht 10.01** | **nein**; ausdrücklich nur Ordner/ZIP |
| **ps5-homebrew-tools / Native Starter** | App-Ordner + ZIP | laut Projekt lokal gebaut, **nicht auf Konsole installiert/getestet** | **nein** |
| **ProsperoSunshine-App** (`PPSA47989`) | Homebrew-Title-Ordner für `/data/homebrew`, mit gebündeltem Host-ELF | 10.20 und 13.00; **nicht 10.01** | **nein**; Installation über ShadowMountPlus |
| **ps5-homebrew-ui / ps5-opengl** | UI- und Grafikbausteine | projektabhängig | **nein** |

Der Native-App-Boilerplate ist ein guter Beweis, dass eine native App mit Controller-Eingabe, Debug-Logging und sauberem Exit baubar ist. Er ist aber **kein PKG-Builder** und wurde nicht auf 10.01 verifiziert.

### 3.4 PKG-/fPKG-Toolchains

| Werkzeug | Technik | 10.01-Status | Grenze |
|---|---|---|---|
| **LibProsperoPKG** (SvenGDK) | .NET 10 / C# 14, GPL-3.0; baut debug-fPKG aus App-Ordner; `ProsperoHomebrewPackager` | Paketbau end-to-end implementiert; **kein 10.01-Konsolentest** | Konsolenakzeptanz hängt von Debug-Modus/Firmware ab; keine Garantie |
| **PS5 UltraPack** (knutwurst) | GPL-3.0; nutzt LibProsperoPkg; baut fPKG aus Ordner/Archiv/Image | laut Projekt auf **11.60** mit kstuff-lite 1.13+ getestet, inkl. Homebrew; **nicht 10.01** | „Welche Firmware ein Paket läuft, hängt vom Jailbreak ab, nicht vom Builder“ |
| **PSVIETHOA-FPKG-Builder** | C#/.NET 10, Avalonia | baut debug-fPKG aus App-Ordner; **kein 10.01-Test** | gleiche Runtime-Abhängigkeit |
| **ps5upload-fpkg** | Rust, im PS5Upload-Repo | für vorbereitete Spielordner/FPKG, nicht für native App-Ordner | kein Ersatz für `ProsperoHomebrewPackager` |

**Wichtig:** Diese Tools bauen ein Paket. Sie installieren es nicht und sie patchen nicht den Kernel. Die Installation/der Start braucht eine passende kstuff-/FPKG-Umgebung. LibProsperoPKG sagt selbst: „On-console installation acceptance is not guaranteed … acceptance depends on console mode and firmware.“

### 3.5 Streaming-/Controller-Referenzen

| Projekt | Architektur | Firmware | Für 10.01 relevant? |
|---|---|---|---|
| **ProsperoSunshine** | Nativer Title (`PPSA47989`) + separater ELF-Host; Avcap2-Capture, Audio, virtuelle DualSense, Sunshine/Moonlight-Protokoll | **10.20 und 13.00 verifiziert**; 10.40 per Sonde; 10.01 ungeprüft | **beste Referenz**, aber kein 10.01-Beleg und kein PKG |
| **Glint** | ELF-Payload; Avcap2 über eine Ptrace-/SceRemotePlay-Injection; HTTP-Streaming | **nur 13.60 getestet** | nein, ohne Portierung |
| **PS5Upload** | ELF-Helper auf PS5; PC/Handy-Engine hält Remote-Play-Session und Hosting; `vpad` auf Konsole | Helper/`vpad` vorhanden; Host ist PC/Handy | Architektur- und Code-Referenz, aber kein nativer PKG-Host |
| **PS5 Moonlight / ProsperoLight** | Moonlight-Client auf PS5 | verschiedene Firmware | Client, kein Host; Videoausgabe-Referenz |
| **PlayBridge / ps5-stream8** | Capture-Referenzen | 13.60/andere | Reverse-Engineering-Referenz, keine 10.01-Garantie |

ProsperoSunshine ist der wichtigste Beleg: Es zeigt, dass ein nativer PS5-Streaming-Host mit virtuellem Controller **prinzipiell** funktioniert. Es zeigt aber auch die Grenze: Die App ist ein Title, der Host ist ein ELF, und die App startet ihn über den Loader auf 9021. Die App selbst macht nicht das privilegierte Streaming.

## 4. Kann man damit eine native Streaming-/Controller-PKG bauen?

### 4.1 Was heute sicher baubar ist

1. **ELF-Host:** Mit PS5 Payload SDK v0.45, Clang 18 und PacBrew-Bibliotheken lässt sich ein ELF für 10.01 kompilieren. Das SDK kennt die Kernel-Offsets.
2. **Native App-Ordner:** Mit dem Native-App-Boilerplate oder einem eigenen C++20-Projekt lässt sich ein Title mit `eboot.bin`, `sce_sys/param.json` und Assets bauen.
3. **fPKG:** Mit LibProsperoPKG/PS5 UltraPack lässt sich aus einem Homebrew-App-Ordner ein fake-signiertes Debug-PKG bauen.
4. **Installation:** Auf einer dafür vorbereiteten Konsole mit kstuff/FPKG-Patches und Debug-Modus ist die Installation prinzipiell vorgesehen.

### 4.2 Was damit nicht belegt ist

Eine **einzelne native PKG**, die auf 10.01:

- `libSceAvcap2` öffnet und Video/Audio encodiert,
- virtuelle DualSense-Controller über `/dev/hid` erzeugt,
- einen LAN-HTTP-/WebSocket-Server bindet,
- die bestehende PS5Upload-Einladungslogik hostet,

ist **nicht demonstriert**. Dafür gibt es drei konkrete Gründe:

#### A. Capture ist kein normales App-API

`libSceAvcap2` und der ResourceArbitrator werden von Spielen/Remote Play kontrolliert. Glint lädt die Capture-Funktion innerhalb von `SceRemotePlay` über einen Ptrace-Trampolin; ProsperoSunshine nutzt einen separaten Host-ELF. Beide Wege setzen eine exploitable Konsole und ein privilegiertes Prozessmodell voraus. Ein normaler Title kann nicht einfach `libSceAvcap2` linken und erwarten, dass ihm der Encoder gehört.

Die 10.x-Datensätze sind zudem firmwareabhängig: ProsperoSunshine musste für 10.20/10.40 ein anderes Avcap2-Layout (`record_shift -0x18`, keine Rate-Felder) verwenden. 10.01 ist nicht gemessen; die Ähnlichkeit zu 10.20 ist plausibel, aber kein Beweis.

#### B. Virtuelle Controller sind payload-nah

`vpad` in PS5Upload und `pad.c` in ProsperoSunshine erzeugen virtuelle DualSense-Controller über IoCTLs auf `/dev/hid` (Add/Insert/Delete/Output, 0xA8-Byte-Blöcke). Das ist kein normales `libScePad`-API. In ProsperoSunshine läuft dieser Code im Host-ELF, nicht im Title. Die IoCTL-Nummern und Blocklayouts wurden auf 13.00 gemessen; 10.20 funktioniert in Tests, 10.01 ist ungeprüft.

Ein natives PKG-Title kann lokale Controller lesen; für **Gäste als Player 2–4** braucht es die virtuellen Pads. Ob ein Title auf 10.01 diese Rechte bekommt, ist offen.

#### C. Die Title-Sandbox kann Netzwerk binden verweigern

ProsperoSunshine dokumentiert ausdrücklich: „a title may not listen on the network (bind fails with errno 13), so the host answers there.“ Deshalb läuft der Netzwerkdienst im Host-ELF, nicht in der Title-App. Das ist der stärkste Beleg dafür, dass eine normale PKG-App nicht automatisch ein Streaming-Host sein kann.

Eine PKG kann trotzdem die **Oberfläche, Einladung, Statusanzeige und den Start** des ELF-Hosts übernehmen. Das ist die realistische Arbeitsteilung.

### 4.3 Realistische Architektur

```
PS5 10.01
┌──────────────────────────────────────────────┐
│ PKG: native Title-App                        │
│  - UI, QR/Code, Status, Start/Stop           │
│  - startet gebündeltes ELF über 9021         │
│  - kein Capture, kein /dev/hid, kein LAN-    │
│    Server (wenn Sandbox es verweigert)       │
└──────────────────┬───────────────────────────┘
                   │ ELF-Loader 9021
┌──────────────────▼───────────────────────────┐
│ ELF-Host (privilegiert)                      │
│  - Avcap2 Capture + Audio                    │
│  - vpad (/dev/hid) für Player 2–4            │
│  - HTTP/WebSocket/WebRTC-Server              │
│  - Token/Einladung                           │
└──────────────────┬───────────────────────────┘
                   │ LAN
             Browser-Gast
```

Das ist **PKG-verpackt**, aber nicht **ein einziger PKG-Prozess**. Wer die zweite Definition verlangt, muss zuerst beweisen, dass der Title auf 10.01 Netzwerk und `/dev/hid` darf. Dafür gibt es aktuell keinen Beleg.

## 5. Lücken und Voraussetzungen

### 5.1 Technische Lücken

| Lücke | Warum sie blockiert | Was sie schließt |
|---|---|---|
| **Kein 10.01-Capture-Test** | Avcap2-Layout, ResourceArbitrator, Encoder-Verhalten können abweichen | Capture-Probe auf 10.01: OpenVideo, Start, 30 min, Audio |
| **Kein 10.01-vpad-Test** | `/dev/hid`-IoCTLs und Blocklayout sind firmwareabhängig | Neutrales Pad add/remove, Player 2 in drei Spielen |
| **Kein 10.01-Native-PKG-Test** | kstuff listet 10.01, aber fPKG-Launch-Berichte sind schlecht | Hello-World-fPKG installieren/starten; Logs sichern |
| **Title-Sandbox** | Netzwerk-Bind und `/dev/hid` können verweigert werden | Test, ob Elevation/Companion-ELF nötig ist |
| **Kein integriertes Projekt** | Capture, vpad, Netzwerk, UI und PKG sind getrennte Bausteine | End-to-End-Prototyp mit einem Gast |
| **Keine PS5-Portierung des PS5Upload-Hosts** | Der Play-Together-Host ist PC/Handy-Engine (Rust/Tauri) | Host-Logik neu implementieren oder portieren |
| **Kein WebRTC/TURN auf der PS5** | Internet-Gäste brauchen Signaling/Relay | LAN-MVP; Internet später |
| **Firmware-Fragilität** | Avcap2/SceRemotePlay, Offsets, 60-Hz-Grenze, H.264-Slices | Pro-Firmware-Testmatrix und Fail-Fast |

### 5.2 Laufzeit- und Installationslücken

- **Relapse** liefert den Exploit und den ELF-Loader, aber nicht automatisch die passenden kstuff-/ShadowMountPlus-Versionen.
- **kstuff-lite 1.11** nennt 1.00–13.60 und fPKG bis 11.40/11.60; 10.01 ist enthalten, aber nicht als eigener Testfall ausgewiesen.
- **10.01-Berichte** zeigen Freeze/„Please wait“, Kernel-Panic und `CE-100096-6` bei PS5-fPKGs; ShadowMountPlus funktionierte in einem Bericht weiter. Das ist anekdotisch, aber ernst zu nehmen.
- **LibProsperoPKG** garantiert die Konsolenakzeptanz nicht; der Debug-Modus und die Firmware entscheiden.
- **PS5 UltraPack** hat Homebrew-fPKG auf 11.60 getestet, nicht auf 10.01.

### 5.3 Rechtliche und organisatorische Voraussetzungen

- PS5 Payload SDK: GPL-3.0; Weitergabe/Änderungen entsprechend lizenzieren.
- LibProsperoPKG, PS5 UltraPack, ProsperoSunshine, Glint, PS5Upload: überwiegend GPL-3.0; Quellen und Notices beachten.
- Keine Sony-SDK-Dateien, keine Spiele-PKGs, keine Keys weitergeben.
- Fake-Signierung nur für eigene Homebrew; keine Raubkopien.
- Jailbreak/Relapse kann gegen ToS verstoßen; PSN-/Account-Risiko.
- Testkonsole, Backup und Bereitschaft zu Datenverlust einplanen.

## 6. Konkrete Voraussetzungen für einen 10.01-Prototypen

### Hardware

- PS5 mit **Firmware 10.01** (Modell notieren; Berichte unterscheiden Pro und Basis).
- Zwei DualSense-Controller (Player 1 und Player 2).
- PC mit Windows 11 + WSL2 oder Linux.
- LAN-Kabel, Router ohne Client-Isolation.
- Smartphone/Tablet als Browser-Gast.
- Testspiele mit lokalem Koop/Shared Screen.
- Optional: USB-Tastatur, Capture-Karte für Vergleichsmessungen.

### Build-Umgebung

- Ubuntu 24.04 (WSL2) oder natives Linux.
- Clang/LLD 18, Make, CMake, Ninja, Python 3, `pyelftools`, Git.
- PS5 Payload SDK **v0.45** unter `/opt/ps5-payload-sdk`, `PS5_PAYLOAD_SDK` gesetzt.
- PacBrew-Pakete nach Bedarf (SDL2, FFmpeg, OpenSSL, curl).
- .NET 10 SDK für LibProsperoPKG.
- Optional Docker für ProsperoSunshine/PS5SD-Builds.

### Konsole

- Relapse erfolgreich ausgeführt; ELF-Loader auf 9021 erreichbar.
- kstuff-lite in der für 10.01 passenden Version; FPKG-Patches aktiv.
- Payload Manager und ShadowMountPlus installiert.
- Debug-Modus/fPKG-Installation verifiziert.
- Genug Speicherplatz; Test-PKG und App-Ordner.

### Build-Ablauf

1. **ELF bauen:** `make` mit `PS5_PAYLOAD_SDK`; `prospero-clang` verwenden.
2. **Native App bauen:** App-Ordner mit `eboot.bin`, `sce_sys/param.json`, Icon/Assets.
3. **App testen:** Zuerst als Ordner über ShadowMountPlus, nicht als PKG.
4. **fPKG bauen:** `ProsperoHomebrewPackager` oder PS5 UltraPack; fake-signieren.
5. **fPKG installieren:** Package Installer/kstuff; Start und Logs prüfen.
6. **Host ELF bündeln:** App startet den Host über 9021; Host macht Capture/vpad/Netzwerk.
7. **End-to-End:** Browser-Gast, Player 2, 720p30, 30 Minuten.

## 7. Prüfplan und Decision Gates

Keine größere Implementierung vor diesen Gates. Jedes Gate hat einen klaren Fallback.

| Gate | Test | Bestanden wenn | Fallback |
|---|---|---|---|
| **G0: SDK/ELF** | `runtime_probe.elf` oder Hello-World-ELF auf 10.01 | Lädt, läuft, Logs zeigen 10.01-Offsets | SDK/Version prüfen; kein Build |
| **G1: Native Title** | App-Ordner über ShadowMountPlus starten | App startet und zeigt UI | Kein nativer Title; ELF-only |
| **G2: fPKG** | Hello-World-fPKG installieren und starten | Installation und Start ohne Panic/`CE-100096-6` | Ordner/ShadowMountPlus statt PKG |
| **G3: Capture** | Avcap2 OpenVideo/Start, 30 min, 720p30 | Stabiles Bild/Audio, keine Crashes | PC/Handy-Engine bleibt Host |
| **G4: vpad** | Neutrales Pad add/remove; Player 2 in 3 Spielen | Eingaben stabil, 30 min | Kein Koop-MVP |
| **G5: Netzwerk** | Title bindet LAN-Port; sonst Host-ELF | Host erreichbar, Token geschützt | ELF-Host zwingend |
| **G6: End-to-End** | PKG-UI + Host-ELF + Browser-Gast | 30 min, Reconnect, ein Gast | Hybrid/PC-Fallback |

**Stop-Regel:** Wenn G2 oder G3 auf 10.01 scheitert, ist eine „PKG-only“-Streaming-App nicht gerechtfertigt. Dann ist der ehrliche Weg: PKG als Wrapper/UI plus ELF-Host oder PC/Handy-Engine.

## 8. Aufwand (grobe Größenordnung)

| Arbeitspaket | Aufwand | Anmerkung |
|---|---|---|
| SDK-/ELF-/PKG-Probes auf 10.01 | 1–3 Wochen | Hardware und Logs nötig; keine Garantie |
| Native UI-PKG + Host-ELF-Start | 4–8 Wochen | wenn Host/ELF wiederverwendet wird |
| 10.01-Capture-/vpad-Portierung | 2–6 Wochen | abhängig von Avcap2-Layout und IoCTLs |
| End-to-End LAN-MVP, ein Gast | 4–8 Wochen | 720p30, ein Spiel, 30-min-Soak |
| Einzelprozess-PKG-Host | 3–6+ Monate | Forschungsprojekt; Title-Elevation ungeklärt |

Das sind keine Festpreise. Der größte Unsicherheitsfaktor ist nicht der PKG-Builder, sondern **Capture plus vpad plus Sandbox** auf genau 10.01.

## 9. Empfehlung

1. **Nicht mit „alles in einer PKG“ beginnen.** Zuerst G0–G2 testen: SDK-ELF, nativer Title, fPKG-Launch auf 10.01.
2. **PKG als UI/Installer planen, Host als ELF.** Das entspricht dem funktionierenden Muster von ProsperoSunshine und umgeht die Title-Sandbox.
3. **Capture und vpad getrennt proben.** 10.01 ist nicht 10.20; die Ähnlichkeit ist eine Hypothese, kein Nachweis.
4. **Wenn fPKG auf 10.01 unzuverlässig bleibt:** nativen Title-Ordner über ShadowMountPlus als Fallback akzeptieren. Das ist keine PKG, aber ein lauffähiger Homebrew-Pfad.
5. **Wenn „PKG-only“ ohne ELF zwingend ist:** zuerst Title-Elevation für Netzwerk und `/dev/hid` beweisen. Gelingt das nicht, ist die Anforderung mit dem heutigen offenen Stack nicht erfüllbar.
6. **PS5Upload-Host nicht einfach portieren.** Der Play-Together-Host ist PC/Handy-Engine; die Einladungs-/Session-Logik muss für die PS5 neu implementiert oder separat portiert werden.

## 10. Quellen

### Exploit und Runtime

- Relapse-Exploit (ntfargo): <https://github.com/ntfargo/Relapse-Exploit>  
  Firmware 7.00–13.60; ELF-Loader auf 9021; Stabilitätshinweise.
- Relapse-Firmware-Allowlist (Mirror): <https://github.com/ps5xploit/relapse>  
  Explizite Liste enthält 10.01; 10.60/11.40 fehlen.
- kstuff-lite 1.11: <https://github.com/EchoStretch/kstuff-lite/releases/tag/v1.11>  
  Firmware 1.00–13.60; FPKG bis 11.40, Fix für 11.60.
- 10.01-/FPKG-Bericht (anekdotisch): <https://github.com/EchoStretch/kstuff-lite/issues/83>

### SDK und Native Apps

- PS5 Payload SDK: <https://github.com/ps5-payload-dev/sdk>  
  GPL-3.0; ELF-Payloads; Clang 18.
- SDK v0.45: <https://github.com/ps5-payload-dev/sdk/releases/tag/v0.45>  
  Veröffentlicht 21. Oktober 2026.
- SDK-Kernel-Offsets: <https://github.com/ps5-payload-dev/sdk/blob/master/crt/kernel.c>  
  Expliziter Fall `0x10010000` für 10.01.
- PS5 Native App Boilerplate: <https://github.com/blackbearreloaded/ps5-native-app-boilerplate>  
  Ordner/ZIP, getestet 6.02/12.70, kein PKG.
- PS5 Homebrew Tools Native Starter: <https://github.com/ShaneBreazeale/ps5-homebrew-tools/blob/main/docs/NATIVE_APP.md>  
  App-Ordner/ZIP; laut Projekt nicht auf Konsole installiert/getestet.
- PS5SDK (PS5Dev): <https://github.com/PS5Dev/PS5SDK>  
  WIP, ELF, keine vollständigen Apps.

### PKG-Toolchains

- LibProsperoPKG: <https://github.com/SvenGDK/LibProsperoPKG>  
  .NET 10, GPL-3.0; `ProsperoHomebrewPackager`; debug-fPKG.
- LibProsperoPKG-Status: <https://github.com/SvenGDK/LibProsperoPKG/blob/main/docs/implementation-status.md>  
  Homebrew-Paketierung end-to-end implementiert; Konsolenakzeptanz nicht garantiert.
- PS5 UltraPack: <https://github.com/knutwurst/ps5-ultrapack>  
  GPL-3.0; Homebrew-/Spiel-fPKG; getestet auf 11.60 mit kstuff-lite 1.13+.
- PSVIETHOA-FPKG-Builder: <https://github.com/thanhsondev/PSVIETHOA-FPKG-Builder>  
  .NET/Avalonia; debug-fPKG aus App-Ordner.

### Streaming und Controller

- ProsperoSunshine: <https://github.com/Darkatek7/ProsperoSunshine>  
  Nativer Host; Avcap2, Audio, virtuelle DualSense; 10.20/13.00 getestet.
- ProsperoSunshine-App: <https://github.com/Darkatek7/ProsperoSunshine/blob/main/app/README.md>  
  Title `PPSA47989`, Installation nach `/data/homebrew`, Host-ELF über 9021; Title darf ggf. nicht Netzwerk binden.
- ProsperoSunshine-Firmware-Porting: <https://github.com/Darkatek7/ProsperoSunshine/blob/main/docs/firmware-porting.md>  
  10.20/10.40-Datensätze; 10.01 nicht verifiziert; vpad-IoCTLs auf 13.00 gemessen.
- Glint: <https://github.com/MiladRTX/Glint>  
  ELF-Payload; Avcap2; nur 13.60 getestet.
- PS5Upload: <https://github.com/phantomptr/ps5upload>  
  `engine/crates/ps5upload-engine/src/join.rs`: Host ist der PC/Handy-Engine; `payload/src/vpad.c`: `/dev/hid`-vpad.

**Hinweis:** Alle Firmware-/Tool-Aussagen sind Stand 18. November 2026. Ein 10.01-Test auf der eigenen Konsole ersetzt keine allgemeine Kompatibilitätsgarantie; die genannten Projekte testen unterschiedliche Firmware-/Modellkombinationen.
