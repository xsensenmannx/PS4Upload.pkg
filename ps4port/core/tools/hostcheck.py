#!/usr/bin/env python3
"""Baut die host-testbaren Payload-Teile und fuehrt ihre Selbsttests aus.

Drei Programme:

* ``ps4port/core/tests/hostcheck.c`` - Anfragezeile, Header, Query/Offset,
  Routen, Antwortaufbau, Status-JSON, Dateiliste/Quote, Diagnostics-JSON, die
  gemeinsame BGFT-Symbolliste aus ``ps4app/bgft_symbols.h`` und die
  BGFT-Reserve aus ``ps4app/upload.h`` (C11).
* ``payload/ps4/tests/pkg_hostcheck.cpp`` - ``pkg_probe`` aus ``ps4app/pkg.h``
  gegen eine erzeugte Minimal-PKG und gezielt manipulierte Varianten (C++17,
  weil ``pkg.h`` ``PkgInfo`` ohne ``struct`` und Referenzen nutzt).
* ``payload/ps4/tests/install_hostcheck.cpp`` - die Installations-
  Zustandsmaschine aus ``ps4app/install_core.h`` mit einem Fake-Backend.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPO_ROOT = ROOT.parent.parent
APP_DIR = REPO_ROOT / "ps4app"
PKG_TEST = REPO_ROOT / "payload" / "ps4" / "tests" / "pkg_hostcheck.cpp"
INSTALL_TEST = REPO_ROOT / "payload" / "ps4" / "tests" / "install_hostcheck.cpp"


def find_zig() -> str | None:
    configured = os.environ.get("ZIG")
    if configured and Path(configured).exists():
        return configured

    try:
        import ziglang  # type: ignore

        candidate = Path(ziglang.__file__).parent / ("zig.exe" if os.name == "nt" else "zig")
        if candidate.exists():
            return str(candidate)
    except ImportError:
        pass

    return shutil.which("zig")


def run_core_check(zig: str, build_dir: Path) -> int:
    exe = build_dir / ("hostcheck.exe" if os.name == "nt" else "hostcheck")

    compile_cmd = [
        zig,
        "cc",
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I",
        str(ROOT / "include"),
        # upload.h (Reserve-Regeln) liegt im App-Verzeichnis.
        "-I",
        str(APP_DIR),
        # Alle Kernmodule, damit ein neues src/*.c nicht im Runner fehlt.
        *[str(path) for path in sorted((ROOT / "src").glob("*.c"))],
        str(ROOT / "tests" / "hostcheck.c"),
        "-o",
        str(exe),
    ]

    compiled = subprocess.run(compile_cmd)
    if compiled.returncode != 0:
        return compiled.returncode

    return subprocess.run([str(exe)]).returncode


def run_pkg_check(zig: str, build_dir: Path) -> int:
    exe = build_dir / ("pkgcheck.exe" if os.name == "nt" else "pkgcheck")

    compile_cmd = [
        zig,
        "c++",
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I",
        str(APP_DIR),
        str(PKG_TEST),
        "-o",
        str(exe),
    ]

    compiled = subprocess.run(compile_cmd)
    if compiled.returncode != 0:
        return compiled.returncode

    # Frisches Arbeitsverzeichnis, damit kein Rest eines alten Laufs zaehlt.
    work_dir = build_dir / "pkgcheck"
    shutil.rmtree(work_dir, ignore_errors=True)
    work_dir.mkdir(parents=True)
    try:
        return subprocess.run([str(exe), str(work_dir)]).returncode
    finally:
        shutil.rmtree(work_dir, ignore_errors=True)


def run_install_check(zig: str, build_dir: Path) -> int:
    exe = build_dir / ("installcheck.exe" if os.name == "nt" else "installcheck")

    compile_cmd = [
        zig,
        "c++",
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I",
        str(APP_DIR),
        str(INSTALL_TEST),
        "-o",
        str(exe),
    ]

    compiled = subprocess.run(compile_cmd)
    if compiled.returncode != 0:
        return compiled.returncode

    work_dir = build_dir / "installcheck"
    shutil.rmtree(work_dir, ignore_errors=True)
    work_dir.mkdir(parents=True)
    try:
        return subprocess.run([str(exe)], cwd=str(work_dir)).returncode
    finally:
        shutil.rmtree(work_dir, ignore_errors=True)


def main() -> int:
    zig = find_zig()
    if zig is None:
        print("hostcheck: zig nicht gefunden (pip install ziglang oder ZIG setzen)", file=sys.stderr)
        return 2

    build_dir = ROOT / "build"
    build_dir.mkdir(parents=True, exist_ok=True)

    print("Kern (Anfrage, Header, Routen, Status):", flush=True)
    status = run_core_check(zig, build_dir)
    if status != 0:
        return status

    print("\nPKG-Pruefung (pkg.h):", flush=True)
    status = run_pkg_check(zig, build_dir)
    if status != 0:
        return status

    print("\nInstallation (install_core.h, Fake-Backend):", flush=True)
    return run_install_check(zig, build_dir)


if __name__ == "__main__":
    raise SystemExit(main())
