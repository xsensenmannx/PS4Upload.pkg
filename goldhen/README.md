# GoldHEN-Installer-Payload

`payload.bin` ist der Installer-Payload aus **DirectPackageInstaller** von
marcussacana (GPL-3.0), Commit `ca7bade66737fede3b0e7bad73b665e0a4d4ff39`
(Datei `DirectPackageInstaller/DirectPackageInstaller/payload`).

- Quelle: https://github.com/marcussacana/DirectPackageInstaller
- SHA-256: `1ca35f9670c2b4ff55a769df52ecccbf534c5e349da85bc5af6da7b8520dcfb0`
- Größe: 16.396 Bytes, Marker `B4 B4 B4 B4 B4 B4` bei Offset 16.384

## Ablauf

PS4Upload patched in den letzten 12 Bytes die IPv4-Adresse des PCs (4 Bytes)
und den Rückruf-Port (2 Bytes, big-endian) hinein, schickt die Datei roh an
GoldHENs BinLoader (Standardport 9090) und wartet auf die Rückverbindung der
Payload. Danach sendet der PC URL, Titel, Content-ID, BGFT-Pakettyp und Größe.

## Warum gebündelt

Ohne Payload kann GoldHEN allein keine PKG von einer URL installieren. Die Payload ist fremder Quellcode (GPL-3.0) aus dem oben genannten Commit und wird
hier unverändert weitergegeben; sie ist kein Bestandteil von PS4Upload.

## Risiko

Das Senden eines Payloads führt Code auf der Konsole aus. Nutzung auf eigenem,
jailbreakten Gerät und auf eigenes Risiko. Bei falscher Firmware oder
beschädigtem Payload kann die Konsole abstürzen. Der RPI-Modus bleibt die
konservativere Alternative.
