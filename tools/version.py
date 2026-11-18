#!/usr/bin/env python3
"""Einzige Quelle fuer Version, APP_VER und Release-Tag.

``version.txt`` enthaelt genau eine Zeile ``MAJOR.MINOR.PATCH``. Daraus
werden abgeleitet:

* ``APP_VER`` fuer ``param.sfo``: ``MM.mm`` mit ``MM = MAJOR * 10 + MINOR``
  und ``mm = PATCH`` (die PS4 erwartet fuenf Zeichen, z. B. ``05.01``).
  Die Abbildung ist monoton, solange ``MINOR < 10`` und ``PATCH < 100``.
* ``ps4app/version.h`` mit ``APP_VERSION`` fuer die UI.
* Release-Tag ``vMAJOR.MINOR.PATCH``.

Befehle:
    python tools/version.py show
    python tools/version.py sync
    python tools/version.py check
    python tools/version.py tag
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VERSION_FILE = ROOT / "version.txt"
HEADER_FILE = ROOT / "ps4app" / "version.h"
SEMVER = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")

HEADER_COMMENT = (
    "/* Generiert aus version.txt durch tools/version.py - nicht bearbeiten. */\n"
)


class VersionError(RuntimeError):
    pass


def read_version() -> tuple[int, int, int]:
    if not VERSION_FILE.exists():
        raise VersionError(f"Versionsdatei fehlt: {VERSION_FILE}")

    text = VERSION_FILE.read_text(encoding="utf-8").strip()
    match = SEMVER.match(text)
    if not match:
        raise VersionError(
            f"version.txt muss MAJOR.MINOR.PATCH enthalten, gefunden: {text!r}"
        )

    return (int(match.group(1)), int(match.group(2)), int(match.group(3)))


def format_version(parts: tuple[int, int, int]) -> str:
    major, minor, patch = parts
    return f"{major}.{minor}.{patch}"


def format_app_ver(parts: tuple[int, int, int]) -> str:
    major, minor, patch = parts
    major_minor = major * 10 + minor
    if major_minor > 99 or patch > 99:
        raise VersionError(
            "APP_VER passt nicht ins PS4-Format MM.mm "
            f"(Version {format_version(parts)}); MINOR muss < 10 und PATCH < 100 sein"
        )
    return f"{major_minor:02d}.{patch:02d}"


def format_tag(parts: tuple[int, int, int]) -> str:
    return f"v{format_version(parts)}"


def header_text(parts: tuple[int, int, int]) -> str:
    return (
        HEADER_COMMENT
        + "#pragma once\n\n"
        + f'#define APP_VERSION "{format_version(parts)}"\n'
        + f'#define APP_VER "{format_app_ver(parts)}"\n'
    )


def sync() -> bool:
    """Schreibt version.h neu, wenn sie nicht zu version.txt passt."""
    expected = header_text(read_version())
    current = HEADER_FILE.read_text(encoding="utf-8") if HEADER_FILE.exists() else ""
    if current == expected:
        return False
    HEADER_FILE.write_text(expected, encoding="utf-8")
    return True


def check() -> list[str]:
    problems: list[str] = []

    try:
        parts = read_version()
    except VersionError as exc:
        return [str(exc)]

    try:
        header = HEADER_FILE.read_text(encoding="utf-8")
    except FileNotFoundError:
        header = ""

    if header != header_text(parts):
        problems.append(
            "ps4app/version.h passt nicht zu version.txt "
            "(python tools/version.py sync)"
        )

    return problems


def show() -> None:
    parts = read_version()
    print(f"Version:  {format_version(parts)}")
    print(f"APP_VER:  {format_app_ver(parts)}")
    print(f"Tag:      {format_tag(parts)}")
    problems = check()
    print(f"version.h: {'OK' if not problems else problems[0]}")


def main(argv: list[str]) -> int:
    command = argv[0] if argv else "show"

    try:
        if command == "show":
            show()
            return 0
        if command == "tag":
            print(format_tag(read_version()))
            return 0
        if command == "sync":
            print(
                "version.h aktualisiert"
                if sync()
                else "version.h ist bereits aktuell"
            )
            return 0
        if command == "check":
            problems = check()
            for problem in problems:
                print(f"FEHLER: {problem}")
            if not problems:
                print(
                    f"OK: {format_version(read_version())} "
                    f"(APP_VER {format_app_ver(read_version())})"
                )
            return 0 if not problems else 1
    except VersionError as exc:
        print(f"Fehler: {exc}", file=sys.stderr)
        return 1

    print(f"Unbekannter Befehl: {command}", file=sys.stderr)
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
