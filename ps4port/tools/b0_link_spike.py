#!/usr/bin/env python3
"""B0-Link-Spike: HTTP-Payload und AVA1-C-Node in einem ELF.

Der Spike baut den normalen HTTP-Payload und den AVA1-Link-Test und linkt
beide Objektmengen zusammen. AVA1 wird dabei **nicht gestartet**; geprueft
wird nur, dass HTTP-Payload, AVA1, Plattform-Shim und pthread in einem ELF
koexistieren. Das ersetzt weder G0 noch G0b und keinen Lauf auf einer PS4.
"""

from __future__ import annotations

import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import ps4payload  # noqa: E402

AVA1_BUILD = ROOT / "ps4port" / "ava1" / "build"
OUT_DIR = ps4payload.BUILD_DIR / "b0"


def build_http() -> None:
    print("1/4 HTTP-Payload bauen")
    ps4payload.build(ps4payload.DEFAULT_OUT)


def build_ava1() -> None:
    print("2/4 AVA1-C-Node bauen")
    subprocess.run(
        [sys.executable, str(ROOT / "ps4port" / "tools" / "ava1_portcheck.py")],
        cwd=str(ROOT),
        check=True,
    )


def main() -> int:
    ps4payload.require_sdk()
    build_http()
    build_ava1()

    http_objs = sorted((ps4payload.BUILD_DIR / "obj").glob("*.o"))
    ava1_objs = [
        path
        for path in sorted((AVA1_BUILD / "obj").glob("*.o"))
        if path.name != "main.o"
    ]
    if not http_objs or not ava1_objs:
        print("Fehler: Objektdateien fehlen", file=sys.stderr)
        return 1

    if OUT_DIR.exists():
        shutil.rmtree(OUT_DIR)
    OUT_DIR.mkdir(parents=True)
    elf = OUT_DIR / "PS4Upload-B0-spike.elf"

    print("3/4 HTTP + AVA1 linken")
    zig = ps4payload.find_zig()
    ps4payload.link_elf(
        zig,
        elf,
        [*http_objs, *ava1_objs],
        libs=("-lpthread",),
    )

    print("4/4 ELF pruefen")
    if not ps4payload.verify(elf):
        return 1

    data = elf.read_bytes()
    problems: list[str] = []
    for marker in (
        b"ava1_server_start",
        b"ava1_platform_random",
        b"ava1_data_start",
        b"GET /status",
    ):
        if marker not in data:
            problems.append(f"{marker.decode()} fehlt im ELF")

    needed = ps4payload.elf_needed(elf)
    for lib in ("libSceLibcInternal.sprx", "libSceNet.sprx", "libkernel_web.sprx"):
        if lib not in needed:
            problems.append(f"DT_NEEDED ohne {lib}")

    if problems:
        for problem in problems:
            print(f"FEHLER: {problem}", file=sys.stderr)
        return 1

    ps4payload.DEFAULT_OUT.mkdir(parents=True, exist_ok=True)
    target = (
        ps4payload.DEFAULT_OUT
        / f"{ps4payload.NAME}-B0-{ps4payload.payload_version()}.elf"
    )
    shutil.copyfile(elf, target)

    print(f"\nB0-Spike: {target} ({target.stat().st_size} Bytes)")
    print(f"SHA-256: {hashlib.sha256(target.read_bytes()).hexdigest()}")
    print(f"DT_NEEDED: {', '.join(needed)}")
    print("Enthalten: HTTP-Routen und AVA1-Symbole.")
    print("Nicht gestartet: AVA1-Server.")
    print("Nicht geprueft: Laden, Ports, Threads, /data, G0/G0b.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
