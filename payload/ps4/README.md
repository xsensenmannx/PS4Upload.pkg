# PS4Upload Payload (PS4)

Dritter Funktionsstand des PS4-Pendants zu PS5Upload: ein ELF-Payload, gebaut
aus dem host-getesteten Kern in [`../../ps4port/core`](../../ps4port/core/README.md),
das auf der Konsole HTTP auf Port **9180** anbietet.

```text
GoldHEN-BinLoader :9090  ──sendet──▶  ps4upload-payload.elf
                                        ├─ GET  /status   JSON
                                        └─ POST /upload   PKG -> /data/ps4upload/
                                                          Header X-Upload-Pin
```

## Bauen

```bash
python tools/ps4payload.py fetch-sdk    # ps4-payload-sdk v0.9 nach ps4sdk/
python tools/ps4payload.py build        # -> dist/PS4Upload-Payload-0.6.2.elf
python tools/ps4payload.py verify dist/PS4Upload-Payload-0.6.2.elf
```

Die SDK wird von einem festen Release-Tag geladen (`v0.9`), nicht von
`releases/latest`; der gepinnte SHA-256 gehoert zu genau diesem Archiv.
`tools/ps4payload.py build` prueft das ELF am Ende selbst (ELF-Header,
Routen im Binary). Der GitHub-Workflow `PS4-Payload` faehrt Hosttests,
Client-Mock, die drei Hosttests zusaetzlich unter ASan/UBSan, Build samt
Reproduzierbarkeitsvergleich, Link-Check und einen 60-Sekunden-Fuzz-Lauf
(libFuzzer ueber Anfragekopf und PKG-Kopf) und legt das ELF als Artefakt ab.

Der kombinierte B0-Link-Spike (dieser HTTP-Payload plus AVA1-C-Node) laesst
sich mit `python ps4port/tools/b0_link_spike.py` bauen. Er startet AVA1 noch
nicht; Stand und Pruefgrenzen stehen in
[`ps4port/B0-STATUS.md`](../../ps4port/B0-STATUS.md).

Der Build nutzt `ziglang` (clang/lld) gegen das Sysroot der ps4-payload-sdk.
Die Wrapper-Skripte der SDK (`bin/orbis-clang`) sind Symlinks auf eine eigene
LLVM-Distribution und funktionieren unter Windows nicht; der Build ruft clang
und lld deshalb direkt mit denselben Flags auf (`-fPIC` + `-pie`, sonst
scheitert der Link an Copy-Relocations der Sony-Stubs).

## P1-1: Installations-Zustandsmaschine und BGFT-Backend (noch kein Aufruf)

Der Ablauf der Installation ist in drei Teile getrennt:

| Datei | Aufgabe |
|---|---|
| [`ps4app/install_core.h`](../../ps4app/install_core.h) | Phasen, Fortschritt, Stillstand, Aufraeumen; ruft nur `init`, `register_task`, `start`, `progress`, `stop`, `unregister` auf. Keine orbis-Header, keine Uhr, keine Sockets - deshalb host-testbar. |
| [`ps4app/install_bgft.h`](../../ps4app/install_bgft.h) | BGFT-Backend: laedt `libSceBgft.sprx` per `sceKernelLoadStartModule` und holt die sechs Funktionen per `sceKernelDlsym`; AppInstUtil-Slot direkt. Die Symbolnamen stehen in [`ps4app/bgft_symbols.h`](../../ps4app/bgft_symbols.h) (orbis-frei), damit `/diagnostics` dieselbe Liste liest. |
| [`ps4app/install.h`](../../ps4app/install.h) | Klebeschicht der App: globaler Zustand, Kernel-Log, Vordergrund-Benutzer. |

```bash
python ps4port/core/tools/hostcheck.py   # 109 Checks fuer install_core.h
python tools/ps4pkg.py fetch-sdk         # OpenOrbis (~150 MB), nur fuer link-check
python tools/ps4payload.py link-check
```

`install_hostcheck.cpp` faehrt mit einem Fake-Backend den Erfolgsfall, fehlgeschlagene Init/Registrierung/Start/Fortschrittsabfrage, gemeldeten
Installationsfehler, Stillstand, Prozentquellen (`length`/`transferred` statt
Total-Felder, `preparing`/`localCopy`), Poll-Drosselung, Uhr-Ruecksprung,
Namensgrenzen und fehlendes Backend - ohne Konsole und ohne Warten.

`link-check` baut dieselbe Einheit gegen die orbis-Header: `install.h` zieht
kein `libSceBgft` mehr (Stand 21.11.2026, 84.048 Bytes, DT_NEEDED nur
`libSceLibcInternal.sprx`, `libSceNet.sprx`, `libkernel_web.sprx`,
`libSceAppInstUtil.sprx`, `libSceUserService.sprx`). Der Gegenversuch
`appinst_link.cpp` ohne orbis-Header linkt `sceAppInstUtilAppInstallPkg`
allein gegen `libSceAppInstUtil` (51.248 Bytes) - der spaetere Fallback ohne
BGFT.

Offen: ob `sceKernelLoadStartModule` das BGFT-Modul in einen
Payload-Prozess laedt, ist ungeprueft; scheitert es, meldet der Init-Aufruf
den Ladecode statt das ELF unladbar zu machen. Installiert wird noch nichts
(P1-2); `GET /diagnostics` laedt das Modul nur zum Ausprobieren und meldet
die dlsym-Ergebnisse.

## Endpunkte

| Pfad | Antwort |
|---|---|
| `GET /status` | JSON: `version`, `uptime_s`, `install.phase` (`idle`/`upload`), `install.percent`, `free_bytes` |
| `POST /upload?name=datei.pkg` | Nimmt den rohen Body an, prueft PIN, Platz und PKG, schreibt nach `/data/ps4upload/<name>`; `200` mit `{"ok":true,"name":…,"bytes":…,"title_id":…,"content_id":…}` |
| `POST /upload?name=datei.pkg&offset=N` | haengt an eine vorhandene `.part`-Datei an (Resume); `409` mit `offset`, wenn der Rest nicht zu `N` passt; `Content-Length: 0` mit `offset=N` laesst einen vollstaendig geladenen Rest nur noch pruefen und umbenennen; ist `<name>` schon fertig und genau `N` Bytes gross, antwortet er `200` mit `"already": true` |
| `HEAD /upload?name=datei.pkg` | `200` mit `X-Upload-Offset: <Bytes>` — so viele Bytes liegen schon vor; bei einem fertigen Paket steht dort seine Groesse plus `X-Upload-Complete: 1` |
| `DELETE /upload?name=datei.pkg` | loescht Datei und `.part`-Rest; `{"ok":true,"deleted":…}` |
| `GET /files` | JSON: `used`, `quota`, `free_bytes` und `files` mit `name`/`bytes` je Paket; listet sichere `.pkg`-Namen und `.pkg.part`-Reste, hoechstens 64 Eintraege (`500`, wenn die Antwort nicht in 4 KB passt) |
| `GET /diagnostics` | PIN-geschuetzte Selbstauskunft: Platz, Quote, Schreib-/Loeschtest, PIN-Datei, `sceAppInstUtilInitialize` und je BGFT-Funktion das `sceKernelDlsym`-Ergebnis |
| `GET /` | kurzer Text mit den Routen |
| sonst | `404`; falsche Methode `405` |

Fehlerantworten sind JSON mit `{"ok":false,"error":…}`:

| Status | `error` | Ursache |
|---|---|---|
| `400` | `name`, `incomplete`, `not_pkg`, `pkg` | ungueltiger Dateiname; Body kuerzer als `Content-Length` (Rest bleibt fuer Resume, `X-Upload-Offset` nennt die tatsaechliche Groesse der `.part`-Datei); keine PKG-Signatur; PKG-Pruefung fehlgeschlagen (`detail` nennt den Grund) |
| `403` | `pin`, `locked` | falsche PIN; Client nach fuenf Fehlversuchen 60 s gesperrt |
| `409` | `offset` | Resume-Offset passt nicht zur vorhandenen `.part`-Datei |
| `411` | `content_length` | `Content-Length` fehlt, ist keine Zahl oder 0 ohne `offset` |
| `429` | `clients` | PIN-Tabelle (16 Clients) voll |
| `500` | `open`, `rename`, `files`, `diag` | `.part` nicht schreibbar; Umbenennen auf den Zielnamen fehlgeschlagen; Dateiliste oder Diagnose passt nicht in den Antwortpuffer |
| `507` | `no_space`, `quota` | Platz reicht nicht (auch wenn `statvfs` fehlschlaegt); Quote aus `config.txt` wuerde ueberschritten (`used`/`quota` stehen im JSON) |

Die Regeln stammen unveraendert aus
[`ps4app/upload.h`](../../ps4app/upload.h) und
[`ps4app/pin.h`](../../ps4app/pin.h):

- **Reserve:** der Body muss in `(frei − 64 MiB) / 2` passen, weil BGFT beim
  Installieren eine zweite Kopie anlegt (`UPLOAD_BGFT_COPIES`).
- **Timeout:** 30 s ohne Bytes brechen den Upload ab (`UPLOAD_IDLE_TIMEOUT_MS`,
  umgesetzt als `SO_RCVTIMEO`); die Teil-Datei bleibt fuer `--resume` liegen.
- **PKG-Pruefung:** `ps4app/pkg.h` prueft vor dem Umbenennen Signatur
  (`7f434e54`), Header-Groesse gegen Dateigroesse, `param.sfo` (TITLE,
  TITLE_ID, CONTENT_ID, APP_VER, CATEGORY) und den Content-Type. Die ersten
  vier Bytes werden schon waehrend des Transfers geprueft.
- **Resume:** geschrieben wird immer `<name>.part`; erst nach erfolgreicher
  Pruefung wird umbenannt. Ein Abbruch laesst die `.part`-Datei stehen, `HEAD`
  liefert den Offset, `POST` mit `offset=` macht weiter. Bricht ein Schreibfehler
  mitten im Body ab, meldet die `400`-Antwort in `X-Upload-Offset` die Groesse
  der Datei nach dem Schliessen (nicht die zuletzt angeforderte Byte-Zahl),
  damit der naechste `HEAD` denselben Wert liefert und kein `409` entsteht.
  Die PKG-Signatur wird nur bei `offset=0` waehrend des Transfers geprueft;
  beim Anhaengen waeren die ersten Bytes des Rests kein PKG-Kopf (die
  Gesamtpruefung vor dem Umbenennen bleibt). Ein zweiter Abschluss nach einer
  verlorenen Antwort ist kein Fehler: `HEAD` meldet die fertige Datei samt
  `X-Upload-Complete: 1`, und der erneute `POST` mit `offset=<Groesse>` und
  `Content-Length: 0` antwortet `200` mit `already` statt `409`.
- **PIN:** vier Ziffern, Header `X-Upload-Pin`, Zaehler und Sperre pro
  Client-IP, 16 Clients. `DELETE` verlangt dieselbe PIN.

## PIN

Beim Start liest der Payload `/data/ps4upload/pin.txt`. Fehlt die Datei oder
steht dort keine vierstellige Zahl, erzeugt er eine PIN und schreibt sie
dorthin. Die PIN laesst sich also mit `ps4manage.py` (Reiter **Dateien**) oder
per FTP lesen und aendern; sie ueberlebt einen Neustart.

```text
/data/ps4upload/pin.txt       z. B. "4711"
/data/ps4upload/config.txt    optional, z. B. "quota_bytes=107374182400"
/data/ps4upload/payload.log   Start, Fehler, Uploads (per FTP lesbar)
/data/ps4upload/heartbeat     {"version":…,"uptime_s":…}, nach jedem Upload
```

`config.txt` wird beim Start gelesen und ist optional: `quota_bytes=<Bytes>`
begrenzt alle Pakete und `.part`-Reste im Ordner (`0` oder fehlend heisst
unbegrenzt, `#` beginnt einen Kommentar, eine spaetere Zeile gewinnt). Wuerde
ein Upload die Quote ueberschreiten, antwortet er mit `507` und
`{"ok":false,"error":"quota","used":…,"quota":…}`; `GET /files` zeigt
dieselben Zahlen samt Groesse je Datei. Die Quote ist eine reine Obergrenze,
sie raeumt nichts auf.

## Selbstauskunft: `GET /diagnostics`

PIN-geschuetzt (Header `X-Upload-Pin`) und nur GET; alles wird beim Abruf
gemessen:

| Feld | Inhalt |
|---|---|
| `dir` | `exists`, `free_bytes` (`statvfs`), `used_bytes` (Summe aller Pakete und `.part`-Reste), `quota` (`0` = unbegrenzt) |
| `write_test` | `write` (Testdatei geschrieben und zurueckgelesen), `delete` (wieder entfernt), `error` (`path`/`open`/`write`/`read`/`remove` oder `null`); `write` bleibt `true`, wenn nur das Loeschen scheitert |
| `pin` | `file` (pin.txt lesbar), `valid` (vierstellige Zahl) |
| `app_inst` | `ok` und `result` von `sceAppInstUtilInitialize` |
| `bgft` | `loaded`, `path` (geladener Kandidat), `result` (Ladecode) und `symbols` mit `name`, `result` und `found` je `sceKernelDlsym`-Aufruf |

Die Testdatei heisst `.diagnostics.tmp` im Upload-Ordner und wird sofort
wieder geloescht; sie zaehlt nicht als Paket und nicht in die Quote. Den
Schreibtest faehrt `ps4u_diag_write_test()` aus dem Kern, damit der Hosttest
ihn gegen ein echtes und ein fehlendes Verzeichnis pruefen kann. BGFT wird
beim ersten Abruf geladen - erst unter `/system/common/lib/libSceBgft.sprx`,
dann als `libSceBgft.sprx` - und dann fuer weitere Abrufe wiederverwendet,
weil jeder `sceKernelLoadStartModule`-Aufruf den Referenzzaehler erhoeht. Die abgefragten Namen sind dieselben
sechs aus [`ps4app/bgft_symbols.h`](../../ps4app/bgft_symbols.h), die das
dlsym-Backend benutzt. Eine Installation startet
`/diagnostics` nicht.

Auf Hardware ist der Endpunkt ungeprueft: ob ein Payload-Prozess BGFT laden
darf und was `sceAppInstUtilInitialize` dort liefert, ist genau die offene
Frage, die er beantworten soll.

## Laden und benutzen

Fuer den Alltag gibt es `tools/ps4payload-client.py` (Status, Selbstauskunft,
Upload mit Resume, Loeschen); die `curl`-Aufrufe unten zeigen, was darunter
passiert. `diagnostics` holt `GET /diagnostics` mit PIN und gibt die Antwort
als Zeilen aus (Groessen in KiB/GiB, je BGFT-Symbol gefunden oder Fehlercode):

```bash
python tools/ps4payload-client.py status --host <PS4-IP>
python tools/ps4payload-client.py diagnostics --host <PS4-IP> --pin 4711
python tools/ps4payload-client.py upload spiel.pkg --host <PS4-IP> --pin 4711 --resume
python tools/ps4payload-client.py delete spiel.pkg --host <PS4-IP> --pin 4711
```

1. GoldHEN starten, BinLoader auf Port 9090 aktiv.
2. Payload senden: `ps4manage.py` → **Payloads** (Datei direkt waehlen).
3. PIN aus `/data/ps4upload/pin.txt` lesen.
4. Upload:

```bash
curl -T spiel.pkg -H "X-Upload-Pin: 4711" \
  "http://<PS4-IP>:9180/upload?name=spiel.pkg"

# Abgebrochenen Upload fortsetzen:
curl -sI "http://<PS4-IP>:9180/upload?name=spiel.pkg"   # X-Upload-Offset: N
tail -c +$((N+1)) spiel.pkg | curl -T - -H "X-Upload-Pin: 4711" \
  "http://<PS4-IP>:9180/upload?name=spiel.pkg&offset=$N"

# Aufraeumen:
curl -X DELETE -H "X-Upload-Pin: 4711" \
  "http://<PS4-IP>:9180/upload?name=spiel.pkg"
```

## Grenzen (Stand jetzt)

- Nur ablegen, **nicht installieren**: PKG-Pruefung ja, BGFT nein. Alte
  Pakete loescht nur ein `DELETE`; beim Start wird nichts aufgeraeumt, und die
  Quote aus `config.txt` ist nur eine Grenze, keine Aufraeumautomatik.
- Ein Client zur Zeit: waehrend eines Uploads wartet jede weitere Verbindung,
  und ein haengender Client blockiert auch `/status` bis zum Timeout (30 s).
  Keine Authentifizierung ausser der PIN, kein TLS.
- Im Kernel-Kontext: laeuft weiter, wenn ein Spiel startet (der Grund fuer den
  Payload), aber ein Fehler kann die Konsole mitnehmen. Nur auf eigener,
  jailbreakter Hardware testen.
- **Nicht auf Hardware geprueft.**

## Nur auf der Konsole pruefbar

Auf dem PC sind Build (ELF-Header, Link, Symbole), die Regeln aus
`upload.h`/`pin.h` und die PKG-Pruefung aus `pkg.h` pruefbar. Die Pruefung
laeuft in `tests/pkg_hostcheck.cpp` gegen eine erzeugte Minimal-PKG (Kopf,
Eintragstabelle, `param.sfo`) und 16 gezielt manipulierte Varianten; Start
ueber `python ps4port/core/tools/hostcheck.py`. Offen bleiben:

| Frage | Warum nur dort |
|---|---|
| Startet das ELF ueberhaupt, und bleibt es nach einem Spielstart erreichbar? | Laderverhalten, GoldHEN-Version, Firmware (Gate G0) |
| Ist `/data` im Payload-Kontext beschreibbar und `statvfs` dort sinnvoll? | Mount- und Rechteverhalten der Firmware |
| Liefert `SO_RCVTIMEO` auf der Konsole die erwarteten Fehler? | Kernel-/libc-Verhalten der PS4 |
| Ist der Stack gross genug fuer die beiden 2-KB-Puffer (Anfrage und Blockkopie)? | Thread-Stack des Payloads |
| Was passiert bei fast voller Platte? | echte Belegung, BGFT-Reserve |
| Stimmen PIN-Sperre und Client-IP unter echten LAN-Bedingungen? | echte Peers, NAT, mehrere Geraete |

Die PKG-Pruefung (`ps4app/pkg.h`) steckt im Upload und ist host-getestet; der
naechste Schritt ist BGFT (`ps4app/install.h`) direkt aus dem Payload.
