# PS4Upload Direkt (PS4-Homebrew)

Native PS4-App, gebaut mit der [OpenOrbis-Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain).

Beim Start erscheint **auf dem Fernseher** eine Oberflaeche (CPU-Framebuffer
ueber `libSceVideoOut`) und die Konsole liefert gleichzeitig auf **Port 8080**
eine Web-Seite an PS5, Handy oder PC im selben LAN/WLAN aus.

```text
+--------------------------- PS4 ---------------------------+
|  PS4UPLOAD DIREKT                           Uptime 12:34   |
|  [ STATUS ]  [ LOG ]  [ NETZ ]                             |
|                                                            |
|  HTTP://192.168.x.y:8080        <- im Browser oeffnen      |
|  SERVER: ONLINE   ANFRAGEN: n   LETZTER CLIENT: ...        |
+-----------------------------------------------------------+
        ^                                   ^
   Fernseher (App-Fenster)         PS5 / Handy / PC im LAN
```

### Bedienung am Controller

| Taste | Wirkung |
|---|---|
| `◀` / `▶` | Reiter wechseln (STATUS, LOG, NETZ, INSTALL) |
| `▲` / `▼` | Log scrollen |
| PS-Taste | zurueck zum PS4-Menue - die App wird dabei pausiert, der Server ist dann nicht mehr erreichbar |

Reiter **STATUS** zeigt Adresse und Live-Zaehler, **LOG** die letzten zwoelf
HTTP-Anfragen mit Zeit, Client und Pfad, **NETZ** alle Verbindungsdaten und
**INSTALL** den Upload- und Installationsfortschritt samt Titel und Content-ID.

Die App bleibt im Vordergrund. Das ist wichtig: Eine PS4-App, die in den
Hintergrund wandert (z. B. weil der PS4-Browser geoeffnet wird), wird vom
System pausiert und hat kein Netzwerk mehr. Deshalb zeigt diese App ihre
Oberflaeche selbst, statt den Browser zu benutzen.

- `main.cpp` – Videoausgabe, HTTP-Server, Live-Status
- `ui.h` – die Oberflaeche (PS4-frei, damit sie sich auf dem PC rendern laesst)
- `vendor/` – `Scene2D`-Helfer aus der OpenOrbis-Toolchain, angepasst: `Init` merkt sich Fehlerstufe und Rueckgabecode, raeumt bei jedem Fehlschlag auf (`cleanupInit`) und gibt Direktspeicher samt Mapping korrekt frei
- `sce_sys/icon0.png` – Icon im PS4-Menue

Titel, Title-ID und Content-ID stehen in [`../tools/ps4pkg.py`](../tools/ps4pkg.py):
`PS4Upload Direkt`, `BREW00090`, `IV0000-BREW00090_00-PS4UPLOAD0000000`.

## Videospeicher

| Groesse | Rechnung |
|---|---|
| Puffer roh | 1920 × 1080 × 4 Byte = 8.294.400 Byte (7,91 MiB) |
| Puffer ausgerichtet | auf 2 MiB aufgerundet = 8.388.608 Byte (8 MiB) |
| Anforderung | 2 × 8 MiB = 16.777.216 Byte (16 MiB, `0x01000000`) |

Frueher stand hier `0xC000000` (192 MiB) – das Zwoelffache des Bedarfs.
`sceKernelAllocateDirectMemory` verlangt 2-MiB-Ausrichtung, deshalb wird aus
`FRAME_BUFFER_ALIGNED_BYTES × FRAME_BUFFER_COUNT` gerechnet.

Jeder Puffer beginnt auf einer 2-MiB-Grenze (`allocateDisplayMem`), nicht
direkt hinter dem vorherigen: Puffer 0 bei 0, Puffer 1 bei 8 MiB. Zwei Puffer
belegen damit exakt die 16 MiB; die Bereichspruefung im Allocator verhindert
einen Ueberlauf ueber die Abbildung hinaus.

Scheitert die Videoausgabe, schreibt die App die Stufe ins Kernel-Log, an der
`sceVideoOutOpen`, `sceKernelCreateEqueue`, `sceVideoOutAddFlipEvent`,
`sceKernelAllocateDirectMemory`, `sceKernelMapDirectMemory`,
`sceVideoOutRegisterBuffers` oder `sceVideoOutSetFlipRate` abgebrochen ist –
zusammen mit dem Rueckgabecode gerade dieser einen API, der angeforderten
Groesse und dem gesamten Direktspeicher.

Eine eigene Probe entfaellt bewusst: `Scene2D::Init` oeffnet den Video-Handle
zuerst und gab ihn frueher bei spaeteren Fehlern nicht frei. Ein zweites
`sceVideoOutOpen` haette dann nur den belegten Handle gemeldet und die echte
Ursache verdeckt. Jetzt raeumt `cleanupInit()` jeden Fehlschlag auf – Handle,
Equeue, Mapping, Direktspeicher und Puffer-Array –, sodass die gemeldete
Stufe die tatsaechliche Fehlerstelle ist. Danach laeuft der HTTP-Server
weiter, die Konsole bleibt also ansprechbar, auch wenn das Bild fehlt.

## Endpunkte

| Pfad | Antwort |
|---|---|
| `/` | HTML-Seite mit Upload-Formular und Live-Status |
| `/status` | JSON inkl. `install` (phase, percent, error, title, file, note) |
| `/upload?name=datei.pkg&pin=1234` (POST) | Nimmt den rohen Datei-Body an, prueft die PIN, streamt nach `/data/ps4upload/`, prueft die PKG und startet BGFT; falsche PIN → 403 |
| sonst | 404; unbekannte Methode → 405, fehlende Content-Length → 411, Schreibfehler → 507 |

## Upload-PIN

Beim Start erzeugt die App eine vierstellige PIN aus Zeit- und Adress-Entropie
(kein kryptografischer Zufall – die Sperre ist die eigentliche Bremse). Sie
steht im INSTALL-Reiter gross und im STATUS-Reiter als Zeile `PIN FUER UPLOAD`.
Der Browser muss sie beim Upload mitschicken (`&pin=`), geprueft wird
ausschliesslich auf der PS4.

- Falsche PIN → HTTP 403, Meldung `PIN FALSCH - VERSUCH n` im INSTALL-Reiter
  und `[ps4upload] Upload abgewiesen: falsche PIN (Versuch n)` im Kernel-Log
- Nach fuenf Fehlversuchen ist der Upload eine Minute gesperrt
  (`[ps4upload] Zu viele Fehlversuche - Upload 60 s gesperrt`)
- Ein erfolgreicher Upload setzt Zaehler und Sperre zurueck
- Die PIN taucht weder in `/status` noch in der HTML-Seite auf

## PKG-Empfang und Installation

1. PS5/Handy oeffnet die Seite, waehlt eine `.pkg`, das JavaScript sendet den
   rohen Datei-Body per `POST /upload?name=...`.
2. Der Server liest den Body in 64-KiB-Stuecken und schreibt ihn sofort nach
   `/data/ps4upload/<name>.pkg` - kein RAM-Puffer fuer die ganze Datei.
   Fortschritt steht live im INSTALL-Reiter und in `/status`.
3. Nach dem letzten Byte prueft `pkg.h` Magic `7f434e54`, Header-Groesse,
   Eintragstabelle und `param.sfo` (Titel, Title-ID, Content-ID).
4. `install.h` initialisiert BGFT, registriert einen Task mit
   `file:///data/ps4upload/<name>.pkg`, startet ihn und pollt
   `sceBgftServiceIntDownloadGetProgress`.
5. Die Datei bleibt liegen und wird erst beim naechsten Upload geloescht,
   weil BGFT sie waehrend der Installation noch pruefen koennte.

Grenzen:

- Ein Client gleichzeitig. Waehrend eines Uploads warten weitere Verbindungen,
  bis die Uebertragung fertig ist.
- Die PIN steht nur auf dem Fernseher und wird serverseitig geprueft. Sie
  schuetzt gegen spontane Uploads aus dem WLAN, ist aber kein TLS: wer den
  Traffic mitschneidet, sieht sie im Klartext.
- BGFT prueft auf Retail-Firmware die Paketsignatur. Selbstgebaute
  Homebrew-PKGs sind nicht signiert; ob der Task angenommen wird, ist offen.
  Schlaegt er fehl, stehen Stufe und Rueckgabecode im Kernel-Log.
- Uploads laufen nur im Vordergrund. Die BGFT-Installation selbst laeuft im
  Systemdienst weiter, auch wenn die App pausiert wird.
- Bricht die Verbindung ab, wird die Teil-Datei geloescht - kein Resume.

## Bauen

```bash
python -m pip install ziglang
python tools/ps4pkg.py fetch-sdk
python tools/ps4pkg.py build
```

Ergebnis: `dist/PS4Upload-Direkt-BREW00090.pkg`.

## Versionierung

`version.txt` im Repo-Wurzelverzeichnis ist die einzige Quelle:

- `python tools/version.py sync` schreibt `ps4app/version.h`; die UI zeigt
  daraus `APP_VERSION` unten rechts
- `python tools/ps4pkg.py build` uebernimmt dieselbe Version als `APP_VER`
  und `VERSION` in `param.sfo`
- `python tools/version.py tag` gibt den Release-Tag aus (`v0.5.1`)

`APP_VER` hat das PS4-Format `MM.mm` mit `MM = MAJOR*10 + MINOR` und
`mm = PATCH`: `0.5.1` wird zu `05.01`. Die Abbildung ist monoton, solange
`MINOR < 10` und `PATCH < 100` bleiben. `verify` meldet einen Fehler, wenn
Paket und `version.txt` auseinanderlaufen.

## Auf der Konsole

1. Paket installieren (GoldHEN/HEN vorausgesetzt), z. B. mit PS4Upload.
2. App starten: Die Oberflaeche erscheint, oben steht die LAN-Adresse.
3. Diese Adresse auf der PS5 im selben LAN/WLAN im Browser oeffnen.

Die angezeigte Adresse wird beim Start per UDP-Socket ermittelt (es wird nichts
gesendet). Falls sie nicht stimmt, steht die echte IP in den PS4-Einstellungen
unter Netzwerk → Verbindungsstatus.

## Vorschau der Oberflaeche

`ui.h` haengt nicht an der PS4. `tools/ui_preview/render_main.cpp` zeichnet
dieselbe Oberflaeche mit `zig c++ -target x86_64-windows-gnu` in eine Bitmap,
damit sich das Layout ohne Konsole pruefen laesst:

```bash
ZIG=$(python -c "import ziglang,os;print(os.path.join(os.path.dirname(ziglang.__file__),'zig.exe'))")
mkdir -p .tmp/render
"$ZIG" c++ -target x86_64-windows-gnu -std=c++17 -O2 \
  -I ps4sdk/OpenOrbis/PS4Toolchain/include/stb -I ps4app \
  -o .tmp/render/render.exe tools/ui_preview/render_main.cpp
./.tmp/render/render.exe      # schreibt .tmp/render/ui.bmp
```

Das gerenderte Ergebnis liegt als
[`../docs/ps4-ui-preview.png`](../docs/ps4-ui-preview.png),
[`../docs/ps4-ui-log-preview.png`](../docs/ps4-ui-log-preview.png),
[`../docs/ps4-ui-net-preview.png`](../docs/ps4-ui-net-preview.png) und
[`../docs/ps4-ui-install-preview.png`](../docs/ps4-ui-install-preview.png) bei.

## Grenzen

Kein Streaming, keine Spielsteuerung, kein Capture, keine Einladungen. Die App
ist die Netzwerk- und Oberflaechenbasis: Konsole sichtbar im Netz, Status am
Fernseher. Der Build ist auf dem PC verifiziert (Kompilierung, Link,
PKG-Struktur); der Lauf auf echter Hardware wurde nicht getestet.
