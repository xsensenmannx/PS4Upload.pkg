#!/usr/bin/env python3
"""Kompiliert und linkt den gepinnten AVA1-C-Node gegen die ps4-payload-sdk.

Der Check beweist Header, Compilerflags und Link-Symbole des C-Nodes mit dem
PS4-Plattform-Shim. Er führt den Node nicht aus und ersetzt keinen Test auf
einer PS4; G0b bleibt ein Hardware-Gate.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPO = ROOT.parent
AVA1 = REPO / "upstream-ava1"
SDK = REPO / "ps4sdk" / "ps4-payload-sdk"
GLUE = ROOT / "ava1"
BUILD = GLUE / "build"

PIN_COMMIT = "deee3bbce1e65be48b71f20fe09cefd10431b906"

# platform_posix.c ist der Host-Node, platform_ps5.c der PS5-Node. Die
# AVX2-Variante bleibt draussen; der PS4-Jaguar hat kein AVX2.
SKIP = {"platform_posix.c", "platform_ps5.c", "ava1_chacha_avx2.c"}

DEFS = [
    "-DAVA1_AEAD_PORTABLE",
    "-DBLAKE3_NO_SSE2",
    "-DBLAKE3_NO_SSE41",
    "-DBLAKE3_NO_AVX2",
    "-DBLAKE3_NO_AVX512",
    "-DBLAKE3_USE_NEON=0",
]


class BuildError(RuntimeError):
    pass


def find_zig() -> str:
    configured = os.environ.get("ZIG")
    if configured and Path(configured).exists():
        return configured

    try:
        import ziglang  # type: ignore

        candidate = Path(ziglang.__file__).parent / (
            "zig.exe" if os.name == "nt" else "zig"
        )
        if candidate.exists():
            return str(candidate)
    except ImportError:
        pass

    found = shutil.which("zig")
    if found:
        return found
    raise BuildError("zig nicht gefunden (pip install ziglang oder ZIG setzen)")


def run(cmd: list[str], what: str) -> None:
    proc = subprocess.run(
        [str(part) for part in cmd], capture_output=True, text=True
    )
    if proc.returncode != 0:
        detail = ((proc.stdout or "") + (proc.stderr or "")).strip()
        raise BuildError(f"{what} fehlgeschlagen ({proc.returncode})\n{detail}")


def check_pin() -> None:
    if not AVA1.exists():
        raise BuildError(
            f"AVA1-Snapshot fehlt unter {AVA1}.\n"
            "  git clone --depth 1 --branch v1.0.11 "
            "https://github.com/phantomptr/ava1.git upstream-ava1"
        )
    if not (AVA1 / ".git").exists():
        print("Hinweis: kein .git im AVA1-Snapshot; Commit-Pin wird nicht geprueft.")
        return
    try:
        proc = subprocess.run(
            ["git", "-C", str(AVA1), "rev-parse", "HEAD"],
            capture_output=True,
            text=True,
        )
    except OSError:
        print("Hinweis: git nicht gefunden; Commit-Pin wird nicht geprueft.")
        return
    if proc.returncode != 0:
        print("Hinweis: Commit-Pin konnte nicht gelesen werden.")
        return
    head = proc.stdout.strip()
    if head != PIN_COMMIT:
        raise BuildError(
            f"AVA1-Snapshot ist nicht auf dem Pin: {head}\n"
            f"Erwartet: {PIN_COMMIT} (v1.0.11)"
        )


def check_sdk() -> None:
    needed = [
        SDK / "target" / "include",
        SDK / "target" / "lib" / "crt1.o",
        SDK / "ldscripts" / "elf_x86_64.x",
    ]
    for path in needed:
        if not path.exists():
            raise BuildError(
                f"ps4-payload-sdk fehlt ({path}).\n"
                "  python tools/ps4payload.py fetch-sdk"
            )


def main() -> int:
    zig = find_zig()
    check_pin()
    check_sdk()

    sources = [
        path
        for path in sorted((AVA1 / "c").glob("*.c"))
        if path.name not in SKIP
    ]
    sources.append(AVA1 / "c" / "gen" / "ava1_gen.c")
    sources.append(GLUE / "platform_ps4.c")

    third_party = [
        AVA1 / "c" / "third_party" / "monocypher" / "monocypher.c",
        AVA1 / "c" / "third_party" / "blake3" / "blake3.c",
        AVA1 / "c" / "third_party" / "blake3" / "blake3_dispatch.c",
        AVA1 / "c" / "third_party" / "blake3" / "blake3_portable.c",
    ]

    for path in [*sources, *third_party]:
        if not path.exists():
            raise BuildError(f"Quelle fehlt: {path}")

    shutil.rmtree(BUILD, ignore_errors=True)
    obj_dir = BUILD / "obj"
    obj_dir.mkdir(parents=True)

    common = [
        zig,
        "cc",
        "--target=x86_64-freebsd",
        "-nostdinc",
        "-isystem",
        str(SDK / "target" / "include"),
        "-I",
        str(AVA1 / "c"),
        "-I",
        str(AVA1 / "c" / "gen"),
        "-I",
        str(AVA1 / "c" / "third_party" / "monocypher"),
        "-I",
        str(AVA1 / "c" / "third_party" / "blake3"),
        *DEFS,
        "-std=gnu11",
        "-O2",
        "-fPIC",
        "-fno-stack-protector",
        "-femulated-tls",
    ]

    print(f"AVA1: {AVA1} (v1.0.11 / {PIN_COMMIT[:12]})")
    print(f"SDK:  {SDK}")
    print(f"1/3 {len(sources) + len(third_party)} Quellen uebersetzen")

    objects: list[Path] = []
    for source in sources:
        obj = obj_dir / f"{source.stem}.o"
        run(
            [*common, "-Wall", "-Wextra", "-Werror", "-c", "-o", obj, source],
            f"Kompilieren von {source.name}",
        )
        objects.append(obj)

    # Third-party code wird wie im Upstream-Build nicht mit -Werror geprueft.
    for source in third_party:
        obj = obj_dir / f"{source.stem}.o"
        run(
            [*common, "-w", "-c", "-o", obj, source],
            f"Kompilieren von {source.name}",
        )
        objects.append(obj)

    main_c = BUILD / "main.c"
    main_c.write_text(
        '#include "ava1_server.h"\n'
        "int main(void) {\n"
        "    ava1_server_cfg_t cfg = {0};\n"
        "    cfg.port = 9120;\n"
        "    return ava1_server_start(&cfg);\n"
        "}\n",
        encoding="utf-8",
    )
    main_obj = obj_dir / "main.o"
    run(
        [*common, "-Wall", "-Wextra", "-Werror", "-c", "-o", main_obj, main_c],
        "Kompilieren des Link-Tests",
    )
    objects.append(main_obj)

    print("2/3 ELF linken")
    elf = BUILD / "ava1-node.elf"
    lib = SDK / "target" / "lib"
    run(
        [
            zig,
            "ld.lld",
            "-m",
            "elf_x86_64",
            "-pie",
            "--script",
            SDK / "ldscripts" / "elf_x86_64.x",
            "-o",
            elf,
            lib / "crt1.o",
            *sorted(objects),
            f"-L{lib}",
            "-lc",
            "-lSceLibcInternal",
            "-lSceNet",
            "-lpthread",
            "-lkernel_web",
        ],
        "Linken",
    )

    print("3/3 ELF und Symbole pruefen")
    data = elf.read_bytes()
    problems: list[str] = []
    if data[:4] != b"\x7fELF":
        problems.append("keine ELF-Datei")
    else:
        e_type, e_machine = struct.unpack_from("<HH", data, 16)
        if data[4] != 2:
            problems.append("kein 64-Bit-ELF")
        if data[5] != 1:
            problems.append("kein Little-Endian-ELF")
        if e_machine != 62:
            problems.append(f"keine x86-64-Maschine ({e_machine})")
        if e_type not in (2, 3):
            problems.append(f"unerwarteter ELF-Typ {e_type}")
    for symbol in (b"ava1_server_start", b"ava1_platform_random"):
        if symbol not in data:
            problems.append(f"{symbol.decode()} fehlt")

    if problems:
        raise BuildError("ELF-Pruefung: " + "; ".join(problems))

    print(f"\nELF:     {elf} ({len(data)} Bytes, ET_DYN x86-64)")
    print(f"SHA-256: {hashlib.sha256(data).hexdigest()}")
    print(
        f"Quellen: {len(sources)} + {len(third_party)} Third-Party + main "
        "(25 AVA1, Generator, Shim, Monocypher, 3x BLAKE3)"
    )
    print("Flags:   -femulated-tls, -DAVA1_AEAD_PORTABLE, BLAKE3 portable")
    print("Link:    libc, SceLibcInternal, SceNet, pthread, kernel_web")
    print("\nOK: C-Node kompiliert und linkt gegen die PS4-SDK.")
    print("Nicht geprueft: Laufzeit, Sockets/pthreads, /data, Durchsatz, Resume.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except BuildError as exc:
        print(f"\nFehler: {exc}", file=sys.stderr)
        raise SystemExit(1)
