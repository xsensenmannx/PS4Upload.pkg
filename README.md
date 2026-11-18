# PS4Upload

Schickt `.pkg`-Dateien vom PC an eine jailbreakte PS4 und startet dort die Installation.
Die PS4 lädt das Paket über HTTP von diesem Rechner; der integrierte Server beherrscht
HTTP-Range-Requests, die der PS4-Installer zwingend braucht.

Das ist der erste, lauffähige PS4-Stand auf Basis der Idee von
[phantomptr/ps5upload](https://github.com/phantomptr/ps5upload). ps5upload selbst ist ein
PS5-Projekt (PS5-Payload, AVA1-Protokoll); sein Payload läuft nicht auf der PS4. Dieser
Stand nutzt stattdessen die auf der PS4 verbreitete Remote-Package-Installer-API.

## Voraussetzungen

- Python 3.10 oder neuer (nur Standardbibliothek, keine Installation nötig)
- PS4 mit Jailbreak und einem Installationsdienst:
  - **Remote Package Installer** (Port `12800`):
    [flatz/ps4_remote_pkg_installer](https://github.com/flatz/ps4_remote_pkg_installer) oder
    [AldySan/ps4_remote_pkg_installer_nextgen](https://github.com/AldySan/ps4_remote_pkg_installer_nextgen)
  - **GoldHEN** mit aktiviertem **BinLoader** (Port `9090`), siehe unten
- PC und PS4 im selben LAN; Port `9898` (PC) muss in der Firewall erlaubt sein.
  Im GoldHEN-Modus zusätzlich die Rückverbindung der Payload erlauben
- PS4-PKG-Dateien (Header `7F 43 4E 54`), auch geteilte Pakete (`_0.pkg`, `_1.pkg`, ...)

## Start

```bash
python ps4upload.py
```

Der Browser öffnet `http://127.0.0.1:9898`. Im Fenster die PS4-IP eintragen, einen
PKG-Pfad (oder einen Ordner) hinzufügen und **Installieren** drücken. Die PS4 lädt die
Datei anschließend selbst herunter und installiert sie.

Dateien können auch direkt mitgegeben werden:

```bash
python ps4upload.py --console 192.168.0.50 "D:\Spiele\CUSA00000.pkg"
```

### Optionen

| Option | Bedeutung |
|---|---|
| `--port 9898` | HTTP-Port, auf dem die PKGs für die PS4 bereitliegen |
| `--host-ip 192.168.0.11` | IP dieses PCs fest vorgeben (überspringt die Netzwerkauswahl) |
| `--list-networks` | Lokale IPv4-Adressen und Adapter anzeigen und beenden |
| `--console 192.168.0.50` | IP der PS4 (sonst im Browser eintragen) |
| `--console-port 12800` | Port des Remote Package Installers |
| `--mode auto\|rpi\|goldhen` | Installationsweg (Standard: `auto`) |
| `--goldhen-port 9090` | Port von GoldHENs BinLoader |
| `--goldhen-payload DATEI` | eigene Installer-Payload (`.bin`) statt der mitgelieferten |
| `--no-browser` | Browser nicht automatisch öffnen |

## Netzwerk auswählen

Standardmäßig nimmt PS4Upload die IP der Standardroute. Bei mehreren Adaptern (WLAN,
Ethernet, VPN) im Fenster unter **PC-Netzwerk** das Netz wählen, in dem die PS4 hängt –
die angezeigte URL ist die, die die PS4 tatsächlich lädt. Ist eine PS4-IP eingetragen und
steht sie in einem anderen /24-Netz, erscheint ein Hinweis.

Auf der Konsole zeigt `python ps4upload.py --list-networks` alle Kandidaten:

```text
192.168.0.11    /24 WLAN
10.0.0.5        /24 Ethernet
```

## GoldHEN statt Remote Package Installer

PS4Upload kann die Installation über GoldHENs **BinLoader** (Port 9090) anstoßen,
ohne dass auf der PS4 der Remote Package Installer laufen muss.

1. GoldHEN starten und in den GoldHEN-Einstellungen den **BinLoader** aktivieren
   (Port 9090).
2. In PS4Upload unter **Installation** den Modus **GoldHEN Payload (BinLoader)** wählen
   oder **Automatisch** lassen: RPI hat Vorrang, sonst wird GoldHEN benutzt.
3. PKG hinzufügen und **Installieren** drücken. PS4Upload schickt die mitgelieferte
   Installer-Payload an den BinLoader; die PS4 verbindet sich zurück und lädt die PKG
   über die angezeigte URL.
4. Den Fortschritt zeigt die PS4 in ihrer Download-/Benachrichtigungsliste. PS4Upload
   kann ihn in diesem Modus nicht auslesen; der Auftrag steht auf „Installation“.

Die Payload liegt unter `goldhen/payload.bin` und stammt aus
[DirectPackageInstaller](https://github.com/marcussacana/DirectPackageInstaller)
(GPL-3.0, Commit `ca7bade`, SHA-256 siehe [goldhen/README.md](goldhen/README.md)).
Sie wird unverändert weitergegeben; mit `--goldhen-payload` lässt sich eine eigene
`.bin` verwenden. Beim Bauen einer EXE die Payload mit
`--add-data "goldhen/payload.bin;goldhen"` einbinden.

**Grenzen des GoldHEN-Modus**

- Nur einzelne PKG-Dateien. Geteilte Pakete (`_0.pkg`, `_1.pkg`) brauchen weiterhin RPI.
- Kein Fortschritt in PS4Upload und kein Stopp der laufenden Installation. Stoppen
  beendet nur das Senden vor der Übergabe an die PS4.
- Der PC muss für die Rückverbindung der Payload erreichbar sein. Windows-Firewall
  beim ersten Lauf erlauben; der Rückruf-Port ist dynamisch.
- Das Senden eines Payloads führt Code auf der Konsole aus. Nur auf eigenem,
  jailbreakten Gerät und auf eigenes Risiko verwenden.

## Was es kann

- Netzwerkauswahl: WLAN, Ethernet oder VPN wählen; automatisch das Netz, in dem die PS4 hängt
- Installation wahlweise über Remote Package Installer oder GoldHEN-BinLoader
- PKG-Dateien und ganze Ordner hinzufügen; geteilte Pakete werden als ein Auftrag erkannt
- HTTP-Server mit Range-Support (206/416), damit die PS4 Metadaten und Teile lesen kann
- Installationsauftrag über `POST /api/install` (Typ `direct`, mehrere Teile in Reihenfolge)
- Fortschritt, Geschwindigkeit und Restzeit über `POST /api/get_task_progress`
- Warteschlange: PS4-Installationen laufen nacheinander, Stoppen und erneuter Versuch möglich
- Web-Oberfläche auf Deutsch, keine externen Abhängigkeiten
- API-/UI-Zugriff nur von localhost; im LAN nur PKG-Downloads

## Grenzen

- Kein 1:1-Port von ps5upload: kein eigener PS4-Payload (nur die gebündelte
  GoldHEN-Installer-Payload), keine Dateiverwaltung/FTP, keine Spiele-Dumps oder Cheats.
  Nur Senden und Installieren von PKGs.
- Der RPI-Modus braucht einen RPI-kompatiblen Dienst auf Port `12800`. Alternativ
  funktioniert der GoldHEN-BinLoader auf Port `9090` ohne RPI (siehe oben), aber nur
  für einzelne PKG-Dateien.
- PS5 wird nicht unterstützt (dafür ist das Original gedacht).
- **Web-UI und `/api` sind nur über localhost erreichbar.** Im LAN liefert der Server
  ausschließlich `/pkg/...` aus, damit die PS4 laden kann. Wer die Oberfläche von einem
  anderen Gerät braucht, muss sie über einen lokalen Tunnel/Proxy auf dem PC öffnen.

## PS5 Companion (separates MVP 0)

Unter [`ps5companion/`](ps5companion/README.md) liegt eine eigenständige,
browserbasierte Steuerungsebene für ein PS5-Play-Together-Setup im LAN:
Session starten, Einladung per Code/QR teilen, Gäste als Player 2–4 zulassen,
Bereitschaft und Controller-Input in der Lobby sehen.

```bash
python run_ps5companion.py
```

Das ist **keine PS5-PKG und kein Streaming-Host**. Capture, Video/Audio und
virtuelle Controller auf der Konsole sind noch nicht implementiert; die
Simulation macht diese Grenze sichtbar. Die technische Einordnung und die
offenen Hardware-Gates stehen in
[`docs/PS5-HOMEBREW-PKG-FEASIBILITY.md`](docs/PS5-HOMEBREW-PKG-FEASIBILITY.md).

## PS4-Homebrew-PKG (`ps4app/`)

`ps4app/` enthaelt eine minimale native PS4-App (C, OpenOrbis-Toolchain).
`tools/ps4pkg.py` baut daraus ein installierbares Fake-PKG:

```bash
python -m pip install ziglang        # clang/lld fuer die PS4-Zielplattform
python tools/ps4pkg.py fetch-sdk     # OpenOrbis-Toolchain (~150 MB) nach ps4sdk/
python tools/ps4pkg.py build         # -> dist/PS4Upload-Direkt-BREW00090.pkg
python tools/ps4pkg.py verify dist/PS4Upload-Direkt-BREW00090.pkg
```

Die Version steht in [`version.txt`](version.txt). `tools/version.py sync`
erzeugt daraus `ps4app/version.h` fuer die UI, der Build schreibt dieselbe
Version als `APP_VER` in `param.sfo`, und der Release-Tag ist `v<version>`
(`python tools/version.py tag`).

Die Kette ist ELF -> FSELF (`create-fself`) -> `param.sfo`/`.gp4` -> fPKG
(`PkgTool` aus LibOrbisPkg). Das Paket ist **kein** Sony-signiertes PKG: es hat
Passcode 0 und braucht eine jailbreakte PS4 mit GoldHEN/HEN. Senden laesst es
sich zum Beispiel mit PS4Upload selbst.

Inhalt der App: Beim Start erscheint **auf dem Fernseher** eine Oberflaeche
(CPU-Framebuffer ueber `libSceVideoOut`) mit der LAN-Adresse und dem Live-Status.
Gleichzeitig liefert die PS4 auf Port 8080 eine Web-Seite an PS5, Handy oder PC
im selben LAN/WLAN - ein PC ist nicht beteiligt.

Die App bleibt bewusst im Vordergrund: Eine PS4-App, die in den Hintergrund
wandert (etwa weil der PS4-Browser geoeffnet wird), wird vom System pausiert und
hat kein Netzwerk mehr - genau deshalb ist ps5upload ein Payload. Diese App
zeigt ihre Oberflaeche daher selbst an, statt den Browser zu benutzen.

Die Seite nimmt inzwischen auch eine PKG an: Der Datei-Body wird nach
`/data/ps4upload/` gestreamt, vor der Installation geprueft (Magic, Groesse,
`param.sfo`) und ueber BGFT installiert. Vor dem Upload muss die vierstellige
PIN eingegeben werden, die auf dem Fernseher steht; falsche Versuche erscheinen
im INSTALL-Reiter und im Kernel-Log, nach fuenf Fehlversuchen ist der Upload
eine Minute gesperrt. Der INSTALL-Reiter auf dem Fernseher
zeigt Upload- und Installationsfortschritt. Ob BGFT eine selbstgebaute,
unsignierte PKG auf Retail-Firmware annimmt, ist ungetestet.

Streaming, Spielsteuerung und Capture sind nicht enthalten. Der Build ist auf
dem PC verifiziert (Kompilierung, Link, PKG-Struktur, PKG-Parser, gerenderte
Oberflaeche); der Lauf auf einer PS4 ist nicht getestet.

## Lizenz

GPL-3.0, siehe [LICENSE](LICENSE). Inspiriert von ps5upload (GPL-3.0) und der
Remote-Package-Installer-API von flatz.
