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
- PS4 mit Jailbreak und laufendem **Remote Package Installer**:
  - [flatz/ps4_remote_pkg_installer](https://github.com/flatz/ps4_remote_pkg_installer) oder
  - [AldySan/ps4_remote_pkg_installer_nextgen](https://github.com/AldySan/ps4_remote_pkg_installer_nextgen)
- PC und PS4 im selben LAN; Port `9898` (PC) muss in der Firewall erlaubt sein
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

## Was es kann

- Netzwerkauswahl: WLAN, Ethernet oder VPN wählen; automatisch das Netz, in dem die PS4 hängt
- PKG-Dateien und ganze Ordner hinzufügen; geteilte Pakete werden als ein Auftrag erkannt
- HTTP-Server mit Range-Support (206/416), damit die PS4 Metadaten und Teile lesen kann
- Installationsauftrag über `POST /api/install` (Typ `direct`, mehrere Teile in Reihenfolge)
- Fortschritt, Geschwindigkeit und Restzeit über `POST /api/get_task_progress`
- Warteschlange: PS4-Installationen laufen nacheinander, Stoppen und erneuter Versuch möglich
- Web-Oberfläche auf Deutsch, keine externen Abhängigkeiten
- API-/UI-Zugriff nur von localhost; im LAN nur PKG-Downloads

## Grenzen

- Kein 1:1-Port von ps5upload: kein PS4-Payload, keine Dateiverwaltung/FTP, keine Spiele-Dumps
  oder Cheats. Nur Senden und Installieren von PKGs.
- Die PS4 braucht einen RPI-kompatiblen Dienst auf Port `12800`. GoldHEN allein reicht nicht,
  solange dort kein RPI mitläuft.
- PS5 wird nicht unterstützt (dafür ist das Original gedacht).
- **Web-UI und `/api` sind nur über localhost erreichbar.** Im LAN liefert der Server
  ausschließlich `/pkg/...` aus, damit die PS4 laden kann. Wer die Oberfläche von einem
  anderen Gerät braucht, muss sie über einen lokalen Tunnel/Proxy auf dem PC öffnen.

## Lizenz

GPL-3.0, siehe [LICENSE](LICENSE). Inspiriert von ps5upload (GPL-3.0) und der
Remote-Package-Installer-API von flatz.
