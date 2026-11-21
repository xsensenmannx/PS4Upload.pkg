#!/usr/bin/env python3
"""Kleiner Client fuer den PS4Upload-Payload auf Port 9180.

Befehle:
    python tools/ps4payload-client.py status --host 192.168.1.50
    python tools/ps4payload-client.py diagnostics --host 192.168.1.50 --pin 4711
    python tools/ps4payload-client.py upload spiel.pkg --host 192.168.1.50 --pin 4711
    python tools/ps4payload-client.py upload spiel.pkg --host 192.168.1.50 --pin 4711 --resume
    python tools/ps4payload-client.py delete spiel.pkg --host 192.168.1.50 --pin 4711

``--resume`` fragt per HEAD den bereits empfangenen Offset ab und schickt nur
den Rest; ohne die Option beginnt der Upload von vorn.
"""

from __future__ import annotations

import argparse
import http.client
import json
import os
import sys
from pathlib import Path
from urllib.parse import quote

CHUNK = 1024 * 1024
DEFAULT_PORT = 9180


class ClientError(RuntimeError):
    pass


def request(host: str, port: int, method: str, path: str, *,
            pin: str | None = None, headers: dict[str, str] | None = None,
            body: bytes | None = None, timeout: float = 30.0) -> tuple[int, dict[str, str], bytes]:
    connection = http.client.HTTPConnection(host, port, timeout=timeout)
    merged = dict(headers or {})
    if pin is not None:
        merged["X-Upload-Pin"] = pin
    try:
        connection.request(method, path, body=body, headers=merged)
        response = connection.getresponse()
        data = response.read()
        return response.status, dict(response.getheaders()), data
    except OSError as exc:
        raise ClientError(f"{host}:{port} nicht erreichbar: {exc}") from exc
    finally:
        connection.close()


def json_body(data: bytes) -> dict:
    try:
        return json.loads(data.decode("utf-8") or "{}")
    except (UnicodeDecodeError, json.JSONDecodeError):
        return {}


def command_status(args: argparse.Namespace) -> int:
    status, _, data = request(args.host, args.port, "GET", "/status")
    if status != 200:
        print(f"HTTP {status}: {data.decode('utf-8', 'replace')}", file=sys.stderr)
        return 1
    print(json.dumps(json_body(data), indent=2, ensure_ascii=False))
    return 0


def human_bytes(value: int) -> str:
    """Bytes als lesbare Groesse; kleine Werte bleiben in Bytes."""
    if value < 1024:
        return f"{value} B"
    size = float(value)
    for unit in ("KiB", "MiB", "GiB", "TiB"):
        size /= 1024.0
        if size < 1024 or unit == "TiB":
            return f"{size:.1f} {unit}"
    return f"{value} B"


def print_diagnostics(payload: dict) -> None:
    """Gibt die Selbstauskunft als Zeilen statt als JSON aus."""
    folder = payload.get("dir") or {}
    quota = int(folder.get("quota") or 0)

    print(f"Payload {payload.get('version', '?')}")
    print("Ordner: " + ("vorhanden" if folder.get("exists") else "fehlt"))
    print(f"  frei:   {human_bytes(int(folder.get('free_bytes') or 0))}")
    print(f"  belegt: {human_bytes(int(folder.get('used_bytes') or 0))}")
    print("  Quote:  " + ("unbegrenzt" if quota == 0 else human_bytes(quota)))

    test = payload.get("write_test") or {}
    print("Schreibtest: "
          + ("ok" if test.get("write") else "schreiben fehlgeschlagen")
          + ", " + ("Datei entfernt" if test.get("delete")
                    else "Datei nicht entfernbar"))
    if test.get("error"):
        print(f"  Fehler: {test['error']}")

    pin = payload.get("pin") or {}
    print("PIN-Datei: " + ("vorhanden" if pin.get("file") else "fehlt")
          + (", gueltig" if pin.get("valid") else ", ungueltig"))

    app_inst = payload.get("app_inst") or {}
    print(f"AppInstUtil: {'ok' if app_inst.get('ok') else 'Fehler'} "
          f"({app_inst.get('result', '?')})")

    bgft = payload.get("bgft") or {}
    if bgft.get("loaded"):
        print(f"BGFT: geladen aus {bgft.get('path')} "
              f"(Handle {bgft.get('result')})")
    else:
        print(f"BGFT: nicht geladen (Code {bgft.get('result', '?')})")
    for entry in bgft.get("symbols") or []:
        name = entry.get("name", "?")
        if entry.get("found"):
            print(f"  {name}: gefunden")
        else:
            print(f"  {name}: fehlt (Code {entry.get('result', '?')})")


def command_diagnostics(args: argparse.Namespace) -> int:
    status, _, data = request(args.host, args.port, "GET", "/diagnostics",
                              pin=args.pin)
    payload = json_body(data)
    if status != 200:
        error = payload.get("error") or data.decode("utf-8", "replace").strip()
        print(f"HTTP {status}: {error}", file=sys.stderr)
        if payload.get("error") == "pin":
            print("PIN aus /data/ps4upload/pin.txt pruefen.", file=sys.stderr)
        return 1
    if not payload:
        print("Antwort ist kein JSON: " + data.decode("utf-8", "replace"),
              file=sys.stderr)
        return 1
    print_diagnostics(payload)
    return 0


def upload_path(name: str, offset: int = 0) -> str:
    """Pfad fuer HEAD/POST/DELETE; der Name darf Leerzeichen enthalten."""
    path = f"/upload?name={quote(name, safe='')}"
    if offset:
        path += f"&offset={offset}"
    return path


def remote_offset(host: str, port: int, name: str) -> int:
    status, headers, _ = request(host, port, "HEAD", upload_path(name))
    if status != 200:
        raise ClientError(f"HEAD /upload antwortet HTTP {status}")
    return int(headers.get("X-Upload-Offset", "0") or 0)


def command_delete(args: argparse.Namespace) -> int:
    status, _, data = request(args.host, args.port, "DELETE",
                              upload_path(args.name), pin=args.pin)
    print(json.dumps(json_body(data), indent=2, ensure_ascii=False))
    return 0 if status == 200 else 1


def command_upload(args: argparse.Namespace) -> int:
    path = Path(args.file)
    size = path.stat().st_size
    name = args.name or path.name
    offset = remote_offset(args.host, args.port, name) if args.resume else 0

    if offset > size:
        raise ClientError(
            f"auf der Konsole liegen {offset} Bytes, die Datei hat nur {size}"
        )
    if offset:
        print(f"Setze bei {offset} von {size} Bytes fort")

    remaining = size - offset
    if remaining == 0:
        print("Datei liegt schon vollstaendig vor; der Payload prueft und benennt nur noch um")
    query = upload_path(name, offset)

    connection = http.client.HTTPConnection(args.host, args.port, timeout=120.0)
    try:
        connection.putrequest("POST", query)
        connection.putheader("Content-Length", str(remaining))
        connection.putheader("X-Upload-Pin", args.pin)
        connection.endheaders()
        with path.open("rb") as handle:
            handle.seek(offset)
            sent = 0
            while sent < remaining:
                block = handle.read(min(CHUNK, remaining - sent))
                if not block:
                    raise ClientError("Datei endet vor der erwarteten Groesse")
                connection.send(block)
                sent += len(block)
                print(f"\r{sent}/{remaining} Bytes", end="", flush=True)
            print()
        response = connection.getresponse()
        data = response.read()
        headers = dict(response.getheaders())
    except OSError as exc:
        raise ClientError(f"Verbindung abgebrochen: {exc}") from exc
    finally:
        connection.close()

    payload = json_body(data)
    if response.status == 200:
        print(f"Fertig: {payload.get('name', name)} "
              f"({payload.get('bytes', size)} Bytes, "
              f"{payload.get('title_id', '?')})")
        return 0

    error = payload.get("error", data.decode("utf-8", "replace"))
    if "offset" in payload:
        error += f" (auf der Konsole liegen {payload['offset']} Bytes)"
    resume_at = headers.get("X-Upload-Offset")
    if resume_at:
        error += f" (neu: {resume_at})"
    print(f"HTTP {response.status}: {error}", file=sys.stderr)
    if payload.get("error") in ("incomplete", "offset"):
        print("Mit --resume erneut starten, um beim echten Offset fortzusetzen.",
              file=sys.stderr)
        return 2
    return 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--host", required=True, help="IP der PS4")
    common.add_argument("--port", type=int, default=DEFAULT_PORT)
    common.add_argument("--pin", default=os.environ.get("PS4UPLOAD_PIN"),
                        help="PIN aus /data/ps4upload/pin.txt (oder PS4UPLOAD_PIN)")

    sub = parser.add_subparsers(dest="command", required=True)
    status = sub.add_parser("status", parents=[common], help="JSON von /status")
    status.set_defaults(func=command_status)

    diagnostics = sub.add_parser("diagnostics", parents=[common],
                                 help="Selbstauskunft aus /diagnostics")
    diagnostics.set_defaults(func=command_diagnostics)

    upload = sub.add_parser("upload", parents=[common], help="PKG hochladen")
    upload.add_argument("file")
    upload.add_argument("--name", help="Zielname (Default: Dateiname)")
    upload.add_argument("--resume", action="store_true",
                        help="an einer vorhandenen .part-Datei fortsetzen")
    upload.set_defaults(func=command_upload)

    delete = sub.add_parser("delete", parents=[common], help="Paket loeschen")
    delete.add_argument("name")
    delete.set_defaults(func=command_delete)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.command in ("upload", "delete", "diagnostics") and not args.pin:
        print("PIN fehlt (--pin oder PS4UPLOAD_PIN)", file=sys.stderr)
        return 2
    try:
        return args.func(args)
    except ClientError as exc:
        print(f"Fehler: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
