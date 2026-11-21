# Upstream-Pin: phantomptr/ps5upload v12.6.16

**Stand:** 21. November 2026  
**Zweck:** Referenz für die beiden PS4-Port-Projekte, kein Bestandteil des PS4-Pakets.

## Pin

| Feld | Wert |
|---|---|
| Repository | <https://github.com/phantomptr/ps5upload> |
| Release | `v12.6.16` (20. November 2026) |
| Tag-Objekt | `297d7b9c7cf4215f4ca38430f676867f6a7e274b` (annotiert) |
| Commit | `981e3d770d0cf04c0c3efc7bd1e22e051f275ae1` |
| Version | `12.6.16` |
| Lizenz | GPL-3.0; die aus chiaki-ng übernommenen Streaming-Teile stehen unter AGPL-3.0 |
| Lokaler Snapshot | `upstream-ps5upload/` (in `.gitignore`, wird nicht mitversioniert) |

Der Snapshot lässt sich jederzeit neu erzeugen:

```bash
git clone --depth 1 --branch v12.6.16 https://github.com/phantomptr/ps5upload.git upstream-ps5upload
git -C upstream-ps5upload rev-parse HEAD
# 981e3d770d0cf04c0c3efc7bd1e22e051f275ae1
```

Lokal liegt der Snapshot bereits auf diesem Tag (`git -C upstream-ps5upload
describe --tags` → `v12.6.16`).

## Was übernommen wird

Nur Quellcode und Muster, keine Release-Binaries:

- `payload/` – Payload-Lebenszyklus, Management-Tabelle, Web-UI-Server, Installer, PKG- und `app.db`-Logik.
- `protocol/ps5upload-ava1/` – Methoden- und Profilbeschreibung für AVA1.
- `engine/` – Nachschlagewerk für die RPC-Erwartungen der Engine, nicht als PS4-Binary.
- `client/` – UI-/Protokollvertrag für Option A, nicht als unveränderter Client.

Konkrete Dateien und ihre Zuordnung stehen in
[`option-a-full/README.md`](option-a-full/README.md) und
[`option-b-payload/README.md`](option-b-payload/README.md).

## Was nicht übernommen wird

- **Kein PS5-ELF.** Das Release-Asset `ps5upload-12.6.16.elf` (12.172.192 Bytes,
  SHA-256 `e02f0251c60e3de979e99b37b2f5276f7887a535223f78f35daf73735cea6576`)
  ist ein FreeBSD-x86-64-Binary für die PS5 und läuft nicht auf der PS4.
- **Kein `vpad`, kein Remote-Play-/Streaming-Code.** Die PS4 ist Empfänger für
  chiaki, nicht Host; die AGPL-3.0-Teile bleiben außerhalb dieses Projekts.
- **Keine PS5-spezifischen Sony-APIs** und keine Annahme, dass sie auf der PS4
  existieren.
- **Keine Spiel-PKGs, keine Sony-Dateien, keine Schlüssel.**

## Lizenzhinweise

Wer Code aus `upstream-ps5upload/` übernimmt, führt die GPL-3.0-Hinweise und
die Quellen mit. Eigener Code dieses Projekts steht unter GPL-3.0 (siehe
[`../LICENSE`](../LICENSE)). AVA1 ist ein separates Repository
(<https://github.com/phantomptr/ava1>, Apache-2.0/MIT). Der C-Node ist als
eigene Quelle in [`AVA1.md`](AVA1.md) auf `v1.0.11` gepinnt; dort stehen
Lizenz, PS4-Shim und Portabilitätsprüfung. Er wird nicht mit
`upstream-ps5upload/` vermischt.

## Abgrenzung zum Release-Binary

Die Release-Notes zu `v12.6.16` betreffen die Fensterposition der Desktop-App,
PS5-Kopien bei Netzlaufwerk-Sicherungen und die Ordnerstruktur der
Save-Backups. Keine dieser Änderungen berührt den Payload, das Übertragungs-
protokoll oder eine PS4-relevante Fähigkeit; der Abgleich in
[`../docs/PS4-PARITY-PS5UPLOAD.md`](../docs/PS4-PARITY-PS5UPLOAD.md) bleibt
damit unverändert gültig. Der Pin ist eine **Quellreferenz**, kein Beleg für
Lauffähigkeit – weder auf PS5 noch auf PS4.
