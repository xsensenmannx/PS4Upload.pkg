# B0-Stand: SDK und Payload-Link-Spike

**Stand:** 21. November 2026  
**Status:** ps4-payload-sdk eingerichtet; HTTP-Payload 0.5.0 gebaut; AVA1-C-Node
allein und zusammen mit dem HTTP-Payload gelinkt. **Kein Lauf auf einer PS4.**
G0 und G0b sind offen.

## SDK

| Feld | Wert |
|---|---|
| Release | ps4-payload-sdk **v0.9** |
| Download | `https://github.com/ps4-payload-dev/sdk/releases/download/v0.9/ps4-payload-sdk.zip` |
| SHA-256 | `3190e88255a72161e5ce2f22a8ae8f638a2c5a9d1ff1e0209ed692d36b4d9ec5` |
| Installiert | `ps4sdk/ps4-payload-sdk/` (gitignored) |
| Sysroot | `target/include`, `target/lib` |
| Link | `target/lib/crt1.o`, `ldscripts/elf_x86_64.x` |

`python tools/ps4payload.py fetch-sdk` ist idempotent: Ist die SDK vorhanden,
wird nichts neu geladen. Das Archiv `.tmp/ps4-payload-sdk.zip` hat den oben
genannten SHA-256; der Build prüft ihn vor dem Entpacken.

Der Build ruft `clang`/`lld` aus `ziglang` direkt auf. Die Wrapper in
`ps4sdk/ps4-payload-sdk/bin/` sind Symlinks auf eine eigene LLVM-Distribution
und unter Windows nicht nutzbar. Die SDK-Header, `crt1.o` und das Linkerskript
sind dagegen vollständig vorhanden:

```text
python tools/ps4payload.py check
SDK:      OK (.../ps4sdk/ps4-payload-sdk)
zig:      .../ziglang/zig.exe
```

## Build-Ergebnisse

| Artefakt | Befehl | Größe | SHA-256 | Geprüft |
|---|---|---:|---|---|
| HTTP-Payload 0.5.0 | `python tools/ps4payload.py build` | 169.976 | `ef7006bd49135e9b4cef6b6b4d8a335e0d9c763c2600ab470a37a66dfab8b6fc` | ET_DYN x86-64; `/status`, `/upload`, `/files`, `X-Upload-Pin`, `X-Upload-Offset` |
| AVA1-C-Node allein | `python ps4port/tools/ava1_portcheck.py` | 2.036.768 | `aae90a0078bddf0f577268175d4f8d8bcfb4481f2870c83c14cf277b406ba39a` | 25 AVA1-Quellen + Generator + Shim + Monocypher + 3× BLAKE3; Link OK |
| B0-Link-Spike | `python ps4port/tools/b0_link_spike.py` | 2.152.512 | `27c23adbc688123012a376129c1e9860445a2ca85652ee89475e1ca488b0c0ae` | HTTP-Routen und AVA1-Symbole im ELF; DT_NEEDED geprüft |

Der B0-Spike entsteht aus den Objekten des HTTP-Payloads plus den
AVA1-Objekten (ohne den `main.o` des AVA1-Link-Tests). Er ist ein
**Link-Spike**, kein Laufzeitartefakt:

- `payload/ps4/main.cpp` startet weiterhin nur HTTP auf **9180**.
- Es gibt in diesem ELF **keinen Aufruf von `ava1_server_start`** und damit
  keinen AVA1-Listener auf **9120**.
- AVA1-Code und -Symbole sind enthalten, werden aber nicht ausgeführt.

Damit ist bewiesen, dass HTTP-Payload, AVA1, `platform_ps4.c`, emuliertes TLS
und pthread in einem ELF linken. Nicht bewiesen ist, dass sie auf der PS4
zusammen laufen.

## Was ohne PS4 prüfbar ist

- Download und Integrität der SDK (fester Tag, fester SHA-256).
- Übersetzen mit den echten FreeBSD-9-Headern der SDK und den vorgesehenen
  Flags (`--target=x86_64-freebsd`, `-fPIC`, `-fno-stack-protector`,
  `-femulated-tls`, `-O2`).
- Linken mit `crt1.o`, `elf_x86_64.x` und den Sony-Stubs.
- ELF-Header (64 Bit, Little-Endian, x86-64, `ET_DYN`), Größe und SHA-256.
- Vorhandensein der HTTP-Routen und AVA1-Marker im Binary.
- `DT_NEEDED` und die Zuordnung der SDK-Stubs.
- Host-Tests des gemeinsamen Kerns (163 Checks), der PKG-Prüfung (47 Checks)
  und des Payload-Clients (21 Checks); sie testen Logik, nicht den Payload.
- Der AVA1-Portabilitätscheck und der kombinierte Link-Spike.

## Was ohne PS4 nicht prüfbar ist

| Frage | Warum nur auf der Konsole |
|---|---|
| Lädt GoldHEN das ELF, und bleibt es nach einem Spielstart erreichbar? | Laderverhalten, Firmware, GoldHEN-Version; Gate **G0** |
| Antwortet `/status` auf 9180 vor und nach dem Spielstart? | echter Payload-Kontext, Netzwerk, Prozess-Lebensdauer |
| Startet AVA1 auf 9120, pairt es und nimmt es einen 1-GB-Transfer wieder auf? | Threads, Sockets, `/data`, Pairing, Journal; Gate **G0b** |
| Lösen sich `pthread_create` & Co. zur Laufzeit auf? | siehe Befund unten |
| Ist `/data/ps4upload` beschreibbar, und liefert `statvfs` sinnvolle Werte? | Mount- und Rechteverhalten der Firmware |
| Funktioniert `O_DIRECT`, und was kostet der Fallback? | Kernel-/Dateisystemverhalten |
| Reserviert `ftruncate` genug Platz, und was passiert bei voller Platte? | keine Blockreservierung; `ENOSPC` kann erst beim Schreiben kommen |
| Wie schnell ist der Transfer, und wie verhält sich der Payload bei Absturz? | echte Hardware, echter Speicher, echter Absturz |

### Befund `-lpthread`

Die SDK-Datei `target/lib/libpthread.so` ist ein Stub mit SONAME
`libkernel_web.sprx`. Im B0-ELF stehen `pthread_create`, `pthread_join`,
`pthread_mutex_*` usw. als **undefinierte dynamische Symbole**;
`DT_NEEDED` nennt nur:

```text
libSceLibcInternal.sprx
libSceNet.sprx
libkernel_web.sprx
```

Der Link ist grün, weil der Stub die Symbole beim Linken sättigt. Ob der
PS4-Lader sie zur Laufzeit aus `libkernel_web` auflöst, ist der erste
G0b-Test. Schlägt das fehl, ist der AVA1-Ansatz auf diesem Weg blockiert;
dann bleibt der Fallback HTTP-Range plus PIN, nicht ein stilles Ignorieren
des Fehlers.

## Nächste Schritte auf Hardware

1. **Vor G0:** HTTP-Payload 0.5.0 per BinLoader senden, `/status` abfragen,
   Firmware und GoldHEN-Version notieren.
2. **G0:** Ein Spiel starten, während der Payload läuft, und `/status` erneut
   abfragen. Ergebnis mit Rückgabecodes dokumentieren.
3. **G0b-Runtime:** Einen B0-Payload bauen, der AVA1 zusätzlich auf 9120
   startet (Identität, Peers und Jobs unter `/data/ps4upload/ava1/`), dann
   `hello`/Pairing und einen abgebrochenen 1-GB-Transfer testen.
4. Erst danach BGFT, Queue und UI aus B1/B2.

Der B0-Link-Spike ist dafür die Vorstufe: Er zeigt, dass die Objekte
zusammenpassen, nicht dass der Node läuft.

## Abgrenzung

- Kein Streaming-, Capture- oder `vpad`-Code; `remoteplay/` bleibt unberührt.
- Keine Aussage über PS4-Lauffähigkeit, Kompatibilität oder Durchsatz.
- Kein Commit und kein Push.
