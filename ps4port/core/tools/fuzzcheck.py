#!/usr/bin/env python3
"""Baut den Fuzz-Harness mit zig und faehrt einen kurzen Standalone-Lauf.

Der echte libFuzzer-Lauf mit Coverage und Sanitizern steckt in der CI
(``.github/workflows/payload.yml``, Job ``fuzz``, clang). Auf Windows kennt
zig kein ``-fsanitize=fuzzer``, deshalb baut dieses Skript dasselbe Ziel
(``tests/fuzz_targets.cpp``) mit ``-DPS4U_FUZZ_STANDALONE`` und mutiert
deterministisch die Startfaelle: eine gueltige Minimal-PKG (aus
``pkg_hostcheck --seed``) und zwei Anfragekoepfe. Ein Absturz bricht den Lauf
mit dem Exit-Code des Harness ab.

    python ps4port/core/tools/fuzzcheck.py [iterationen]
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
HARNESS = ROOT / "tests" / "fuzz_targets.cpp"
BUILD_DIR = ROOT / "build" / "fuzzcheck"
DEFAULT_ITERATIONS = 20000


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


def run(command: list[str]) -> int:
    return subprocess.run(command).returncode


def main() -> int:
    zig = find_zig()
    if zig is None:
        print("zig nicht gefunden (pip install ziglang oder ZIG setzen)", file=sys.stderr)
        return 2

    iterations = DEFAULT_ITERATIONS
    if len(sys.argv) > 1:
        iterations = int(sys.argv[1])

    obj_dir = BUILD_DIR / "obj"
    corpus = BUILD_DIR / "corpus"
    shutil.rmtree(BUILD_DIR, ignore_errors=True)
    obj_dir.mkdir(parents=True)
    corpus.mkdir(parents=True)

    include = ["-I", str(ROOT / "include")]
    objects: list[str] = []
    for source in sorted((ROOT / "src").glob("*.c")):
        obj = obj_dir / f"{source.stem}.o"
        code = run([zig, "cc", "-std=c11", "-O1", *include, "-c", "-o", str(obj), str(source)])
        if code != 0:
            return code
        objects.append(str(obj))

    # Gültige PKG als Startfall: derselbe Generator wie im PKG-Hosttest.
    pkgseed = BUILD_DIR / ("pkgseed.exe" if os.name == "nt" else "pkgseed")
    code = run([zig, "c++", "-std=c++17", "-O1", "-I", str(APP_DIR),
                str(PKG_TEST), "-o", str(pkgseed)])
    if code != 0:
        return code
    code = run([str(pkgseed), "--seed", str(corpus)])
    if code != 0:
        return code

    exe = BUILD_DIR / ("fuzz_targets.exe" if os.name == "nt" else "fuzz_targets")
    code = run([zig, "c++", "-std=c++17", "-O1", "-DPS4U_FUZZ_STANDALONE",
                *include, "-I", str(APP_DIR), str(HARNESS), *objects,
                "-o", str(exe)])
    if code != 0:
        return code

    print(f"Fuzz (zig, Standalone): {iterations} Iterationen")
    return run([str(exe), str(iterations), str(corpus / "seed.pkg")])


if __name__ == "__main__":
    raise SystemExit(main())
