#!/usr/bin/env python3
"""Baut den PS4-Payload aus ``payload/ps4/`` mit der ps4-payload-sdk.

Kette: C-Quellen (Payload + ``ps4port/core``) -> ELF (clang/lld aus dem
``ziglang``-Paket) gegen das Sysroot der SDK, Link mit ``crt1.o`` und dem
Linkerskript der SDK.

Die Wrapper der SDK (``bin/orbis-clang``) sind Symlinks auf eine eigene
LLVM-Distribution; unter Windows fehlen sie. Der Build ruft clang und lld
deshalb direkt mit denselben Flags auf.

Befehle:
    python tools/ps4payload.py check
    python tools/ps4payload.py fetch-sdk
    python tools/ps4payload.py build [--out DIR]
    python tools/ps4payload.py verify PFAD/ZUR/DATEI.elf
    python tools/ps4payload.py link-check   # P1-Spike, braucht OpenOrbis
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PAYLOAD_DIR = ROOT / "payload" / "ps4"
CORE_DIR = ROOT / "ps4port" / "core"
SDK_DIR = ROOT / "ps4sdk" / "ps4-payload-sdk"
# OpenOrbis-Toolchain (kommt von tools/ps4pkg.py fetch-sdk); liefert die
# orbis-Header und die Stubs fuer libSceBgft/libSceUserService.
OPENORBIS_DIR = ROOT / "ps4sdk" / "OpenOrbis" / "PS4Toolchain"
BUILD_DIR = PAYLOAD_DIR / "build"
DEFAULT_OUT = ROOT / "dist"

NAME = "PS4Upload-Payload"
VERSION_RE = re.compile(r'^#define\s+PAYLOAD_VERSION\s+"([^"]+)"', re.MULTILINE)

# Festes Release, kein "latest": der Hash unten gehoert zu genau diesem Tag.
SDK_TAG = "v0.9"
SDK_URL = (
    "https://github.com/ps4-payload-dev/sdk/releases/download/"
    f"{SDK_TAG}/ps4-payload-sdk.zip"
)
SDK_SHA256 = "3190e88255a72161e5ce2f22a8ae8f638a2c5a9d1ff1e0209ed692d36b4d9ec5"
SDK_ZIP = ROOT / ".tmp" / "ps4-payload-sdk.zip"


class BuildError(RuntimeError):
    pass


def run(cmd: list[str], cwd: Path | None = None) -> None:
    pretty = " ".join(str(part) for part in cmd)
    print(f"  $ {pretty}")
    proc = subprocess.run(
        [str(part) for part in cmd],
        cwd=str(cwd) if cwd else None,
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        detail = ((proc.stdout or "") + (proc.stderr or "")).strip()
        raise BuildError(f"Befehl fehlgeschlagen ({proc.returncode}): {pretty}\n{detail}")


def payload_version() -> str:
    """Liest PAYLOAD_VERSION aus main.cpp - eine Quelle fuer Binary und Dateiname."""
    match = VERSION_RE.search((PAYLOAD_DIR / "main.cpp").read_text(encoding="utf-8"))
    if match is None:
        raise BuildError("PAYLOAD_VERSION fehlt in payload/ps4/main.cpp")
    return match.group(1)


def find_zig() -> str:
    try:
        import ziglang
    except ImportError as exc:  # pragma: no cover - Umgebungsfehler
        raise BuildError(
            "Das Python-Paket 'ziglang' fehlt (liefert clang und lld).\n"
            "  python -m pip install ziglang"
        ) from exc

    zig = Path(ziglang.__file__).parent / ("zig.exe" if os.name == "nt" else "zig")
    if zig.exists():
        return str(zig)
    found = shutil.which("zig")
    if not found:
        raise BuildError(f"zig wurde nicht gefunden (erwartet: {zig})")
    return found


def sdk_ready() -> bool:
    return (SDK_DIR / "target" / "lib" / "crt1.o").exists() and (
        SDK_DIR / "ldscripts" / "elf_x86_64.x"
    ).exists()


def require_sdk() -> None:
    if not sdk_ready():
        raise BuildError(
            f"ps4-payload-sdk fehlt unter {SDK_DIR}.\n"
            "  python tools/ps4payload.py fetch-sdk"
        )


def fetch_sdk() -> None:
    if sdk_ready():
        print(f"SDK ist bereits vorhanden: {SDK_DIR}")
        return

    SDK_ZIP.parent.mkdir(parents=True, exist_ok=True)
    if not SDK_ZIP.exists():
        print(f"Lade ps4-payload-sdk {SDK_TAG} (~6 MB) nach {SDK_ZIP}")
        with urllib.request.urlopen(SDK_URL) as response, open(SDK_ZIP, "wb") as handle:
            shutil.copyfileobj(response, handle)

    digest = hashlib.sha256(SDK_ZIP.read_bytes()).hexdigest()
    if digest != SDK_SHA256:
        raise BuildError(
            f"SHA-256 passt nicht: {digest}\n"
            f"Erwartet: {SDK_SHA256} ({SDK_TAG})\n"
            "Archiv loeschen (.tmp/ps4-payload-sdk.zip) und erneut laden; "
            "ein anderes SDK-Release braucht SDK_TAG und SDK_SHA256 zusammen."
        )

    print(f"Entpacke nach {SDK_DIR.parent}")
    SDK_DIR.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(SDK_ZIP) as archive:
        archive.extractall(SDK_DIR.parent)
    if not sdk_ready():
        raise BuildError(f"SDK-Struktur unerwartet: {SDK_DIR}")


def include_flags() -> list[str]:
    sdk_include = SDK_DIR / "target" / "include"
    return [
        "-nostdinc",
        "-isystem",
        str(sdk_include),
        "-I",
        str(CORE_DIR / "include"),
        "-I",
        str(PAYLOAD_DIR),
        "-I",
        str(ROOT / "ps4app"),
    ]


def link_elf(zig: str, elf: Path, objects: list[Path],
             lib_dirs: tuple[Path, ...] = (),
             libs: tuple[str, ...] = ()) -> None:
    """Linkt wie der Payload-Build; zusaetzliche Stub-Bibliotheken optional."""
    run(
        [
            zig,
            "ld.lld",
            "-m",
            "elf_x86_64",
            "-pie",
            "--script",
            SDK_DIR / "ldscripts" / "elf_x86_64.x",
            "-o",
            elf,
            SDK_DIR / "target" / "lib" / "crt1.o",
            *objects,
            f"-L{SDK_DIR / 'target' / 'lib'}",
            *(f"-L{path}" for path in lib_dirs),
            "-lc",
            "-lSceLibcInternal",
            "-lSceNet",
            "-lkernel_web",
            # /diagnostics ruft sceAppInstUtilInitialize direkt auf.
            "-lSceAppInstUtil",
            *libs,
        ]
    )


def build(out_dir: Path) -> Path:
    require_sdk()
    zig = find_zig()

    sources_c = sorted((CORE_DIR / "src").glob("*.c"))
    sources_cpp = sorted(PAYLOAD_DIR.glob("*.cpp"))
    if not sources_cpp:
        raise BuildError(f"Keine C++-Quellen in {PAYLOAD_DIR}")

    if BUILD_DIR.exists():
        shutil.rmtree(BUILD_DIR)
    obj_dir = BUILD_DIR / "obj"
    obj_dir.mkdir(parents=True)

    print(f"1/3 Quellen uebersetzen ({len(sources_c) + len(sources_cpp)})")
    common_flags = [
        "--target=x86_64-freebsd",
        *include_flags(),
        "-fPIC",
        "-fno-stack-protector",
        "-femulated-tls",
        "-O2",
    ]
    objects: list[Path] = []
    for source in sources_c:
        obj = obj_dir / f"{source.stem}.o"
        run([zig, "cc", *common_flags, "-c", "-o", obj, source])
        objects.append(obj)

    # Der Payload-Einstieg ist C++ und bindet ps4app/upload.h und ps4app/pin.h
    # ein; ohne Ausnahmen/RTTI braucht er keine libc++ zur Laufzeit.
    for source in sources_cpp:
        obj = obj_dir / f"{source.stem}.o"
        run([
            zig, "c++", *common_flags,
            "-fno-exceptions", "-fno-rtti", "-std=c++17",
            "-c", "-o", obj, source,
        ])
        objects.append(obj)

    print("2/3 ELF linken")
    elf = BUILD_DIR / f"{NAME}.elf"
    link_elf(zig, elf, objects)

    print("3/3 nach dist kopieren")
    out_dir.mkdir(parents=True, exist_ok=True)
    target = out_dir / f"{NAME}-{payload_version()}.elf"
    shutil.copyfile(elf, target)
    print(f"\nELF: {target} ({target.stat().st_size} Bytes)")
    return target


def elf_needed(path: Path) -> list[str]:
    """Liest DT_NEEDED aus dem Programm-Header/der Dynamik-Sektion."""
    data = path.read_bytes()
    e_phoff, = struct.unpack_from("<Q", data, 0x20)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 0x36)
    loads: list[tuple[int, int, int]] = []
    dynamic: tuple[int, int] | None = None

    for index in range(e_phnum):
        base = e_phoff + index * e_phentsize
        p_type, = struct.unpack_from("<I", data, base)
        p_offset, p_vaddr, _, p_filesz = struct.unpack_from("<QQQQ", data, base + 8)
        if p_type == 1:
            loads.append((p_vaddr, p_offset, p_filesz))
        elif p_type == 2:
            dynamic = (p_offset, p_filesz)

    if dynamic is None:
        return []

    def to_offset(vaddr: int) -> int:
        for start, offset, size in loads:
            if start <= vaddr < start + size:
                return offset + (vaddr - start)
        raise BuildError(f"{path}: Adresse {vaddr:#x} liegt in keinem Segment")

    offset, size = dynamic
    entries = [struct.unpack_from("<QQ", data, offset + i * 16)
               for i in range(size // 16)]
    strtab = next((value for tag, value in entries if tag == 5), 0)
    start = to_offset(strtab)

    needed: list[str] = []
    for tag, value in entries:
        if tag != 1:
            continue
        end = data.index(b"\0", start + value)
        needed.append(data[start + value:end].decode("ascii", "replace"))
    return needed


def link_check() -> int:
    """P1-Spike: nur Uebersetzen und Linken, kein Lauf auf der Konsole."""
    require_sdk()
    bgft_lib = OPENORBIS_DIR / "lib" / "libSceBgft.so"
    bgft_header = OPENORBIS_DIR / "include" / "orbis" / "Bgft.h"
    if not bgft_lib.exists() or not bgft_header.exists():
        raise BuildError(
            f"OpenOrbis-Toolchain fehlt unter {OPENORBIS_DIR}.\n"
            "  python tools/ps4pkg.py fetch-sdk"
        )

    zig = find_zig()
    out_dir = BUILD_DIR / "linkcheck"
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    common = [
        "c++",
        "--target=x86_64-freebsd",
        *include_flags(),
        "-fPIC",
        "-fno-stack-protector",
        "-femulated-tls",
        "-O2",
        "-fno-exceptions",
        "-fno-rtti",
        "-std=c++17",
    ]

    print("1/4 install.h mit orbis-Headern uebersetzen")
    bgft_obj = out_dir / "install_link.o"
    run([zig, *common, "-I", OPENORBIS_DIR / "include", "-c", "-o", bgft_obj,
         PAYLOAD_DIR / "tests" / "install_link.cpp"])

    print("2/4 ohne -lSceBgft linken (BGFT kommt per dlsym)")
    bgft_elf = out_dir / "install_link.elf"
    link_elf(zig, bgft_elf, [bgft_obj], (OPENORBIS_DIR / "lib",),
             ("-lSceAppInstUtil", "-lSceUserService"))

    print("3/4 Fallback ohne orbis-Header uebersetzen")
    fallback_obj = out_dir / "appinst_link.o"
    run([zig, *common, "-c", "-o", fallback_obj,
         PAYLOAD_DIR / "tests" / "appinst_link.cpp"])

    print("4/4 Fallback gegen libSceAppInstUtil linken")
    fallback_elf = out_dir / "appinst_link.elf"
    link_elf(zig, fallback_elf, [fallback_obj], (), ("-lSceAppInstUtil",))

    bgft_needed = elf_needed(bgft_elf)
    fallback_needed = elf_needed(fallback_elf)
    problems: list[str] = []

    for name in ("libSceAppInstUtil.sprx", "libSceUserService.sprx"):
        if name not in bgft_needed:
            problems.append(f"install_link.elf: DT_NEEDED ohne {name}")
    if any("Bgft" in name for name in bgft_needed):
        problems.append("install_link.elf: linkt BGFT noch fest")
    if not any("libkernel_web" in name for name in bgft_needed):
        problems.append("install_link.elf: libkernel_web fehlt (dlsym)")
    if "libSceAppInstUtil.sprx" not in fallback_needed:
        problems.append("appinst_link.elf: DT_NEEDED ohne libSceAppInstUtil.sprx")
    if any("Bgft" in name for name in fallback_needed):
        problems.append("appinst_link.elf: zieht BGFT mit")

    print(f"\ninstall.h + dlsym-Backend:  {bgft_elf} ({bgft_elf.stat().st_size} Bytes)")
    print(f"  DT_NEEDED: {', '.join(bgft_needed)}")
    print(f"Fallback ohne BGFT-Header: {fallback_elf} "
          f"({fallback_elf.stat().st_size} Bytes)")
    print(f"  DT_NEEDED: {', '.join(fallback_needed)}")

    if problems:
        for problem in problems:
            print(f"FEHLER: {problem}")
        return 1

    print("\nOK: beide Wege uebersetzen und linken. install.h zieht kein")
    print("libSceBgft mehr (dlsym); der Fallback braucht nur libSceAppInstUtil.")
    return 0


def verify(elf_path: Path) -> bool:
    data = elf_path.read_bytes()
    problems: list[str] = []

    print(f"Datei:   {elf_path} ({len(data)} Bytes)")
    print(f"SHA-256: {hashlib.sha256(data).hexdigest()}")

    if data[:4] != b"\x7fELF":
        problems.append("keine ELF-Datei")
    else:
        ei_class = data[4]
        ei_data = data[5]
        e_type, e_machine = struct.unpack_from("<HH", data, 16)
        types = {1: "REL", 2: "EXEC", 3: "DYN", 4: "CORE"}
        print(f"ELF:     class={ei_class} data={ei_data} "
              f"type={types.get(e_type, e_type)} machine={e_machine}")
        if ei_class != 2:
            problems.append("kein 64-Bit-ELF")
        if ei_data != 1:
            problems.append("kein Little-Endian-ELF")
        if e_machine != 62:
            problems.append(f"keine x86-64-Maschine ({e_machine})")
        if e_type not in (2, 3):
            problems.append(f"unerwarteter ELF-Typ {e_type}")

    for route in (b"/status", b"/upload", b"/files", b"/diagnostics",
                  b"X-Upload-Pin", b"X-Upload-Offset", b"X-Upload-Complete"):
        if route not in data:
            problems.append(f"{route.decode()} fehlt im Binary")

    if problems:
        for problem in problems:
            print(f"FEHLER: {problem}")
        return False

    print("\nOK: ELF-Header und Routen sind stimmig.")
    return True


def check() -> int:
    print(f"Payload:  {PAYLOAD_DIR}")
    print(f"Kern:     {CORE_DIR}")
    print(f"SDK:      {'OK' if sdk_ready() else 'FEHLT'} ({SDK_DIR})")
    try:
        print(f"zig:      {find_zig()}")
    except BuildError as exc:
        print(f"zig:      FEHLT - {exc}")
    sources = sorted(p.name for p in PAYLOAD_DIR.glob("*.cpp"))
    print(f"App:      {', '.join(sources) or 'keine C++-Quellen'}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("check", help="Voraussetzungen pruefen")
    sub.add_parser("fetch-sdk", help="ps4-payload-sdk herunterladen und entpacken")

    build_parser = sub.add_parser("build", help="Payload-ELF bauen")
    build_parser.add_argument("--out", type=Path, default=DEFAULT_OUT)

    verify_parser = sub.add_parser("verify", help="ELF pruefen")
    verify_parser.add_argument("elf", type=Path)

    sub.add_parser(
        "link-check",
        help="P1-Spike: ps4app/install.h gegen die OpenOrbis-Stubs linken",
    )

    args = parser.parse_args(argv)

    try:
        if args.command == "check":
            return check()
        if args.command == "fetch-sdk":
            fetch_sdk()
            return 0
        if args.command == "build":
            target = build(args.out.resolve())
            return 0 if verify(target) else 1
        if args.command == "verify":
            return 0 if verify(args.elf.resolve()) else 1
        if args.command == "link-check":
            return link_check()
    except BuildError as exc:
        print(f"\nFehler: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
