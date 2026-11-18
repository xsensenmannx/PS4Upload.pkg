#!/usr/bin/env python3
"""Baut aus ``ps4app/`` ein installierbares PS4-Fake-PKG und prueft es.

Kette: C-Quellen -> ELF (clang/lld aus dem ``ziglang``-Paket) -> FSELF
(``create-fself``) -> ``param.sfo`` (``PkgTool``) -> ``.gp4`` (``create-gp4``)
-> ``.pkg`` (``PkgTool pkg_build``).

Reproduzierbar: ``SOURCE_DATE_EPOCH`` (sonst der Commit-Zeitpunkt) ersetzt den
Build-Zeitstempel im GP4; gleiche Quellen und Tool-Versionen ergeben damit
dasselbe PKG.

Befehle:
    python tools/ps4pkg.py check
    python tools/ps4pkg.py fetch-sdk
    python tools/ps4pkg.py build [--out DIR]
    python tools/ps4pkg.py verify PFAD/ZUR/DATEI.pkg
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
import tarfile
import time
import urllib.request
from pathlib import Path

import version as version_source

ROOT = Path(__file__).resolve().parent.parent
APP_DIR = ROOT / "ps4app"
SDK_DIR = ROOT / "ps4sdk"
TC_DIR = SDK_DIR / "OpenOrbis" / "PS4Toolchain"
BUILD_DIR = APP_DIR / "build"
DEFAULT_OUT = ROOT / "dist"

TITLE = "PS4Upload Direkt"
TITLE_ID = "BREW00090"
CONTENT_ID = "IV0000-BREW00090_00-PS4UPLOAD0000000"
CATEGORY = "gd"
PAID = "0x3800000000000011"
PKG_NAME = f"PS4Upload-Direkt-{TITLE_ID}.pkg"

TOOLCHAIN_URL = (
    "https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases/download/"
    "v0.5.4/toolchain-llvm-18.tar.gz"
)
TOOLCHAIN_SHA256 = "3c7cd5bb593ca74fa1c13fd59f3938dc0fc07985167f7275063019e63abe4526"
TOOLCHAIN_ARCHIVE = ROOT / ".tmp" / "toolchain-llvm-18.tar.gz"

class BuildError(RuntimeError):
    pass


def current_version() -> tuple[int, int, int]:
    try:
        return version_source.read_version()
    except version_source.VersionError as exc:
        raise BuildError(str(exc)) from exc


def source_date_epoch() -> int:
    """Fixer Zeitstempel fuer reproduzierbare Builds.

    ``SOURCE_DATE_EPOCH`` gewinnt; sonst der Commit-Zeitpunkt des HEADs,
    damit lokaler und CI-Build denselben GP4-Stempel bekommen.
    """
    value = os.environ.get("SOURCE_DATE_EPOCH")
    if value:
        try:
            return int(value)
        except ValueError as exc:
            raise BuildError("SOURCE_DATE_EPOCH ist keine Unix-Zeit") from exc

    try:
        result = subprocess.run(
            ["git", "-C", str(ROOT), "log", "-1", "--format=%ct"],
            capture_output=True,
            text=True,
            check=True,
        )
        return int(result.stdout.strip())
    except (OSError, subprocess.SubprocessError, ValueError):
        return 0


def normalize_gp4(gp4_path: Path, epoch: int) -> str:
    """Ersetzt den Build-Zeitpunkt im GP4 durch einen festen Wert."""
    stamp = time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(epoch))
    data = gp4_path.read_bytes()
    normalized, count = re.subn(
        rb"<volume_ts>[^<]*</volume_ts>",
        f"<volume_ts>{stamp}</volume_ts>".encode("ascii"),
        data,
    )
    if count != 1:
        raise BuildError("volume_ts im GP4 nicht gefunden")
    gp4_path.write_bytes(normalized)
    return stamp


def sfo_entries() -> list[tuple[str, str, int, str]]:
    """SFO-Eintraege; APP_VER und VERSION kommen aus version.txt."""
    app_ver = version_source.format_app_ver(current_version())
    return [
        ("APP_TYPE", "Integer", 4, "1"),
        ("APP_VER", "Utf8", 8, app_ver),
        ("ATTRIBUTE", "Integer", 4, "0"),
        ("CATEGORY", "Utf8", 4, CATEGORY),
        ("CONTENT_ID", "Utf8", 48, CONTENT_ID),
        ("DOWNLOAD_DATA_SIZE", "Integer", 4, "0"),
        ("SYSTEM_VER", "Integer", 4, "0"),
        ("TITLE", "Utf8", 128, TITLE),
        ("TITLE_ID", "Utf8", 12, TITLE_ID),
        ("VERSION", "Utf8", 8, app_ver),
    ]


# --------------------------------------------------------------------------- #
# Hilfsfunktionen
# --------------------------------------------------------------------------- #


def run(cmd: list[str], cwd: Path | None = None, env: dict | None = None) -> str:
    pretty = " ".join(cmd)
    print(f"  $ {pretty}")
    proc = subprocess.run(
        cmd,
        cwd=str(cwd) if cwd else None,
        env=env,
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        detail = (proc.stdout or "") + (proc.stderr or "")
        raise BuildError(f"Befehl fehlgeschlagen ({proc.returncode}): {pretty}\n{detail.strip()}")
    return proc.stdout


def find_zig() -> str:
    try:
        import ziglang
    except ImportError as exc:  # pragma: no cover - Umgebungsfehler
        raise BuildError(
            "Das Python-Paket 'ziglang' fehlt (liefert clang und lld).\n"
            "  python -m pip install ziglang"
        ) from exc

    zig = Path(ziglang.__file__).parent / ("zig.exe" if os.name == "nt" else "zig")
    if not zig.exists():
        found = shutil.which("zig")
        if not found:
            raise BuildError(f"zig wurde nicht gefunden (erwartet: {zig})")
        return found
    return str(zig)


def require_sdk() -> None:
    if not (TC_DIR / "link.x").exists():
        raise BuildError(
            f"OpenOrbis-Toolchain fehlt unter {TC_DIR}.\n"
            "  python tools/ps4pkg.py fetch-sdk"
        )


def tool_path(name: str) -> Path:
    suffix = ".exe" if os.name == "nt" else ""
    path = TC_DIR / "bin" / "windows" / f"{name}{suffix}"
    if not path.exists():
        raise BuildError(f"Tool fehlt: {path}")
    return path


def pkgtool(*args: str, cwd: Path | None = None) -> str:
    dll = TC_DIR / "bin" / "windows" / "PkgTool.Core.dll"
    if not dll.exists():
        raise BuildError(f"PkgTool fehlt: {dll}")
    env = dict(os.environ)
    env.setdefault("DOTNET_ROLL_FORWARD", "LatestMajor")
    return run(["dotnet", str(dll), *args], cwd=cwd, env=env)


def fself_env() -> dict:
    env = dict(os.environ)
    env["OO_PS4_TOOLCHAIN"] = str(TC_DIR)
    return env


# --------------------------------------------------------------------------- #
# Toolchain holen
# --------------------------------------------------------------------------- #


def fetch_sdk() -> None:
    if (TC_DIR / "link.x").exists():
        print(f"Toolchain ist bereits vorhanden: {TC_DIR}")
        return

    TOOLCHAIN_ARCHIVE.parent.mkdir(parents=True, exist_ok=True)
    if not TOOLCHAIN_ARCHIVE.exists():
        print(f"Lade OpenOrbis-Toolchain (~150 MB) nach {TOOLCHAIN_ARCHIVE}")
        with urllib.request.urlopen(TOOLCHAIN_URL) as response, \
                open(TOOLCHAIN_ARCHIVE, "wb") as handle:
            shutil.copyfileobj(response, handle)

    digest = hashlib.sha256(TOOLCHAIN_ARCHIVE.read_bytes()).hexdigest()
    if digest != TOOLCHAIN_SHA256:
        raise BuildError(
            f"SHA-256 passt nicht: {digest}\n"
            f"Erwartet: {TOOLCHAIN_SHA256} - Archiv loeschen und erneut laden."
        )

    print(f"Entpacke nach {SDK_DIR}")
    SDK_DIR.mkdir(parents=True, exist_ok=True)
    with tarfile.open(TOOLCHAIN_ARCHIVE, "r:gz") as archive:
        archive.extractall(SDK_DIR, filter="data")
    print(f"Fertig: {TC_DIR}")


# --------------------------------------------------------------------------- #
# Build
# --------------------------------------------------------------------------- #


def build(out_dir: Path) -> Path:
    require_sdk()
    zig = find_zig()

    parts = current_version()
    if version_source.sync():
        print("version.h aus version.txt neu erzeugt")
    print(
        f"Version {version_source.format_version(parts)} "
        f"(APP_VER {version_source.format_app_ver(parts)}, "
        f"Tag {version_source.format_tag(parts)})"
    )

    sources_c = sorted(APP_DIR.glob("*.c")) + sorted((APP_DIR / "vendor").glob("*.c"))
    sources_cpp = sorted(APP_DIR.glob("*.cpp")) + sorted((APP_DIR / "vendor").glob("*.cpp"))
    if not sources_c and not sources_cpp:
        raise BuildError(f"Keine Quellen in {APP_DIR}")

    if BUILD_DIR.exists():
        shutil.rmtree(BUILD_DIR)
    obj_dir = BUILD_DIR / "obj"
    pkgroot = BUILD_DIR / "pkgroot"
    (pkgroot / "sce_sys").mkdir(parents=True)
    obj_dir.mkdir(parents=True)

    include = TC_DIR / "include"
    print("1/6 Quellen uebersetzen")
    objects: list[str] = []
    for source in sources_c:
        obj = obj_dir / f"{source.stem}.o"
        run([
            zig, "cc", "--target=x86_64-freebsd", "-fPIC", "-funwind-tables",
            "-nostdinc", "-isystem", str(include), "-isystem", str(APP_DIR),
            "-O2", "-c", "-o", str(obj), str(source),
        ])
        objects.append(str(obj))

    for source in sources_cpp:
        obj = obj_dir / f"{source.stem}.o"
        run([
            zig, "c++", "--target=x86_64-freebsd", "-fPIC", "-funwind-tables",
            "-nostdinc", "-nostdinc++", "-isystem", str(include),
            "-isystem", str(include / "c++" / "v1"), "-isystem", str(include / "stb"),
            "-isystem", str(APP_DIR),
            "-O2", "-c", "-o", str(obj), str(source),
        ])
        objects.append(str(obj))

    print("2/6 ELF linken")
    elf = BUILD_DIR / f"{TITLE_ID}.elf"
    run([
        zig, "ld.lld", "-m", "elf_x86_64", "-pie",
        "--script", str(TC_DIR / "link.x"), "--eh-frame-hdr",
        "-o", str(elf), f"-L{TC_DIR / 'lib'}",
        str(TC_DIR / "lib" / "crt1.o"), *objects,
        "-lc", "-lkernel", "-lc++", "-lSceVideoOut", "-lScePad", "-lSceUserService",
        "-lSceBgft",
    ])

    print("3/6 eboot.bin (FSELF) erzeugen")
    eboot = pkgroot / "eboot.bin"
    run([
        str(tool_path("create-fself")), "-in", str(elf),
        "--out", str(BUILD_DIR / f"{TITLE_ID}.oelf"),
        "--eboot", str(eboot), "--paid", PAID,
    ], env=fself_env())

    icon_source = APP_DIR / "sce_sys" / "icon0.png"
    if not icon_source.exists():
        raise BuildError(f"Icon fehlt: {icon_source}")
    shutil.copyfile(icon_source, pkgroot / "sce_sys" / "icon0.png")

    print("4/6 param.sfo schreiben")
    sfo = pkgroot / "sce_sys" / "param.sfo"
    pkgtool("sfo_new", str(sfo))
    for key, kind, maxsize, value in sfo_entries():
        pkgtool(
            "sfo_setentry", str(sfo), key,
            "--type", kind, "--maxsize", str(maxsize), "--value", value,
        )

    print("5/6 GP4-Projekt erzeugen")
    run([
        str(tool_path("create-gp4")), "-out", "pkg.gp4",
        f"--content-id={CONTENT_ID}",
        "--files", "eboot.bin sce_sys/param.sfo sce_sys/icon0.png",
    ], cwd=pkgroot)

    stamp = normalize_gp4(pkgroot / "pkg.gp4", source_date_epoch())
    print(f"GP4-Zeitstempel fixiert: {stamp} UTC")

    print("6/6 PKG bauen")
    pkgtool("pkg_build", "pkg.gp4", ".", cwd=pkgroot)

    built = pkgroot / f"{CONTENT_ID}.pkg"
    if not built.exists():
        raise BuildError(f"PkgTool hat kein Paket erzeugt ({built})")

    out_dir.mkdir(parents=True, exist_ok=True)
    target = out_dir / PKG_NAME
    shutil.copyfile(built, target)
    print(f"\nPKG: {target} ({target.stat().st_size} Bytes)")
    return target


# --------------------------------------------------------------------------- #
# Verify
# --------------------------------------------------------------------------- #


def _u32(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def _u64(data: bytes, offset: int) -> int:
    return struct.unpack_from(">Q", data, offset)[0]


def parse_sfo(data: bytes) -> dict[str, object]:
    if data[:4] != b"\x00PSF":
        raise BuildError("param.sfo hat keine PSF-Signatur")
    key_table, data_table, count = struct.unpack_from("<III", data, 8)
    values: dict[str, object] = {}
    for index in range(count):
        entry = 20 + index * 16
        key_offset, fmt, length, _max_length, data_offset = struct.unpack_from(
            "<HHIII", data, entry
        )
        key = data[key_table + key_offset:data.index(b"\x00", key_table + key_offset)].decode()
        raw = data[data_table + data_offset:data_table + data_offset + length]
        if fmt == 0x0404:
            values[key] = struct.unpack("<I", raw)[0]
        else:
            values[key] = raw.split(b"\x00", 1)[0].decode("utf-8", "replace")
    return values


def verify(pkg_path: Path) -> bool:
    data = pkg_path.read_bytes()
    problems: list[str] = []
    parts = current_version()
    expected_app_ver = version_source.format_app_ver(parts)

    print(f"Datei:        {pkg_path} ({len(data)} Bytes)")
    print(f"SHA-256:      {hashlib.sha256(data).hexdigest()}")
    print(
        f"Version:      {version_source.format_version(parts)} "
        f"(APP_VER {expected_app_ver})"
    )

    if data[:4] != b"\x7fCNT":
        problems.append(f"PKG-Signatur fehlt (gefunden: {data[:4]!r})")
        for problem in problems:
            print(f"FEHLER: {problem}")
        return False

    declared_size = _u64(data, 0x430)
    entry_count = _u32(data, 0x10)
    table_offset = _u32(data, 0x18)
    content_id = data[0x40:0x40 + 36].decode("ascii", "replace")
    pfs_offset = _u64(data, 0x410)
    pfs_size = _u64(data, 0x418)

    print(f"Content-ID:   {content_id}")
    print(f"Eintraege:    {entry_count} (Tabelle bei 0x{table_offset:x})")
    print(f"PFS:          0x{pfs_offset:x} / {pfs_size} Bytes")
    print(f"Groesse:      {declared_size} laut Header")

    if content_id != CONTENT_ID:
        problems.append(f"Content-ID weicht ab: {content_id} != {CONTENT_ID}")
    if declared_size != len(data):
        problems.append(f"Header-Groesse {declared_size} != Dateigroesse {len(data)}")
    if pfs_offset + pfs_size > len(data):
        problems.append("PFS-Bereich liegt ausserhalb der Datei")

    entries: dict[int, tuple[int, int]] = {}
    for index in range(entry_count):
        base = table_offset + index * 32
        entry_id, _name_off, _f1, _f2, offset, size = struct.unpack_from(">IIIIII", data, base)
        entries[entry_id] = (offset, size)

    for needed, label in ((0x1000, "param.sfo"), (0x1200, "icon0.png"), (0x0409, "keystone")):
        if needed not in entries:
            problems.append(f"Eintrag 0x{needed:04x} ({label}) fehlt")
        else:
            offset, size = entries[needed]
            if offset + size > len(data):
                problems.append(f"Eintrag 0x{needed:04x} ({label}) zeigt aus der Datei heraus")

    if 0x1200 in entries:
        offset, size = entries[0x1200]
        if data[offset:offset + 8] != b"\x89PNG\r\n\x1a\n":
            problems.append("icon0.png ist kein PNG")

    if 0x1000 in entries:
        offset, size = entries[0x1000]
        sfo = parse_sfo(data[offset:offset + size])
        print("param.sfo:")
        for key in ("TITLE", "TITLE_ID", "CONTENT_ID", "CATEGORY", "APP_VER", "VERSION"):
            print(f"  {key:<11} {sfo.get(key)}")
        if sfo.get("CONTENT_ID") != CONTENT_ID:
            problems.append("CONTENT_ID in param.sfo weicht ab")
        if sfo.get("TITLE_ID") != TITLE_ID:
            problems.append("TITLE_ID in param.sfo weicht ab")
        if sfo.get("APP_VER") != expected_app_ver:
            problems.append(
                f"APP_VER {sfo.get('APP_VER')!r} != {expected_app_ver} aus version.txt"
            )
        if sfo.get("VERSION") != expected_app_ver:
            problems.append(
                f"VERSION {sfo.get('VERSION')!r} != {expected_app_ver} aus version.txt"
            )

    if problems:
        for problem in problems:
            print(f"FEHLER: {problem}")
        return False

    print("\nOK: Struktur, Eintraege und param.sfo sind stimmig.")
    return True


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #


def check() -> int:
    try:
        parts = current_version()
        print(
            f"Version: {version_source.format_version(parts)} "
            f"(APP_VER {version_source.format_app_ver(parts)}, "
            f"Tag {version_source.format_tag(parts)})"
        )
        header_problems = version_source.check()
        print(f"version.h: {'OK' if not header_problems else header_problems[0]}")
    except BuildError as exc:
        print(f"Version: FEHLT - {exc}")

    zig = None
    try:
        zig = find_zig()
    except BuildError as exc:
        print(f"zig:     FEHLT - {exc}")
    if zig:
        print(f"zig:     {zig}")
    print(f"SDK:     {'OK' if (TC_DIR / 'link.x').exists() else 'FEHLT'} ({TC_DIR})")
    for name in ("create-fself", "create-gp4"):
        path = TC_DIR / "bin" / "windows" / f"{name}.exe"
        print(f"{name}: {'OK' if path.exists() else 'FEHLT'}")
    print(f"PkgTool: {'OK' if (TC_DIR / 'bin' / 'windows' / 'PkgTool.Core.dll').exists() else 'FEHLT'}")
    print(f"App:     {', '.join(sorted(p.name for p in APP_DIR.glob('*.c'))) or 'keine C-Quellen'}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("check", help="Voraussetzungen pruefen")
    sub.add_parser("fetch-sdk", help="OpenOrbis-Toolchain herunterladen und entpacken")

    build_parser = sub.add_parser("build", help="PKG bauen")
    build_parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    build_parser.add_argument("--no-verify", action="store_true")

    verify_parser = sub.add_parser("verify", help="PKG pruefen")
    verify_parser.add_argument("pkg", type=Path)

    args = parser.parse_args(argv)

    try:
        if args.command == "check":
            return check()
        if args.command == "fetch-sdk":
            fetch_sdk()
            return 0
        if args.command == "build":
            target = build(args.out.resolve())
            return 0 if args.no_verify or verify(target) else 1
        if args.command == "verify":
            return 0 if verify(args.pkg.resolve()) else 1
    except BuildError as exc:
        print(f"\nFehler: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
