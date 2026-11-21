"""Prueft tools/ps4payload-client.py gegen einen lokalen Mock des Payloads.

Der Mock antwortet wie der echte Payload (JSON, PIN, HEAD-Offset, 409 mit
echtem Offset, kodierte Namen) und zeichnet die angefragten Pfade auf. Der
Client laeuft als eigener Prozess, damit Exit-Codes und Ausgaben mitgeprueft
werden.

    python tools/tests/check_client.py
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

REPO = Path(__file__).resolve().parents[2]
CLIENT = REPO / "tools" / "ps4payload-client.py"
PIN = "4711"

state = {"parts": {}, "files": {}, "deleted": [], "paths": [], "completed": set()}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def _json(self, status, payload, extra=None):
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        for key, value in (extra or {}).items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        state["paths"].append(self.path)
        if self.path == "/status":
            self._json(200, {"version": "0.3.0", "uptime_s": 5, "free_bytes": 123})
        elif self.path == "/diagnostics":
            if self.headers.get("X-Upload-Pin") != PIN:
                self._json(403, {"ok": False, "error": "pin"})
                return
            self._json(200, {
                "ok": True, "version": "0.6.2",
                "dir": {"exists": True, "free_bytes": 1073741824,
                        "used_bytes": 2048, "quota": 0},
                "write_test": {"write": True, "delete": True, "error": None},
                "pin": {"file": True, "valid": True},
                "app_inst": {"ok": True, "result": 0},
                "bgft": {
                    "loaded": True, "path": "libSceBgft.sprx", "result": 12,
                    "symbols": [
                        {"name": "sceBgftServiceIntInit", "result": 0,
                         "found": True},
                        {"name": "sceBgftServiceIntDownloadStartTask",
                         "result": -1, "found": False},
                    ],
                },
            })
        else:
            self._json(404, {"ok": False})

    def do_HEAD(self):
        state["paths"].append(self.path)
        query = parse_qs(urlparse(self.path).query)
        name = query["name"][0]
        if name in state["completed"]:
            offset = len(state["files"].get(name, b""))
            self.send_response(200)
            self.send_header("Content-Length", "0")
            self.send_header("X-Upload-Offset", str(offset))
            self.send_header("X-Upload-Complete", "1")
            self.end_headers()
            return
        offset = state["parts"].get(name, 0) + state.get("head_bias", 0)
        self.send_response(200)
        self.send_header("Content-Length", "0")
        self.send_header("X-Upload-Offset", str(offset))
        self.end_headers()

    def do_POST(self):
        state["paths"].append(self.path)
        query = parse_qs(urlparse(self.path).query)
        name = query["name"][0]
        offset = int(query.get("offset", ["0"])[0])
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        if self.headers.get("X-Upload-Pin") != PIN:
            self._json(403, {"ok": False, "error": "pin"})
            return
        # Wiederholter Abschluss eines fertigen Pakets: 200 mit already.
        if (length == 0 and offset > 0 and name in state["completed"] and
                offset == len(state["files"].get(name, b""))):
            self._json(200, {"ok": True, "name": name, "bytes": offset,
                             "already": True})
            return
        if offset and state["parts"].get(name, 0) != offset:
            self._json(409, {"ok": False, "error": "offset",
                             "offset": state["parts"].get(name, 0)})
            return
        if offset == 0:
            state["files"][name] = b""
        state["files"][name] = state["files"].get(name, b"") + body
        state["parts"][name] = 0
        self._json(200, {"ok": True, "name": name,
                         "bytes": len(state["files"][name]),
                         "title_id": "BREW00090", "content_id": "X"})

    def do_DELETE(self):
        state["paths"].append(self.path)
        query = parse_qs(urlparse(self.path).query)
        name = query["name"][0]
        if self.headers.get("X-Upload-Pin") != PIN:
            self._json(403, {"ok": False, "error": "pin"})
            return
        state["deleted"].append(name)
        state["files"].pop(name, None)
        self._json(200, {"ok": True, "name": name, "deleted": True})


def run(*args: str, env_extra: dict | None = None) -> subprocess.CompletedProcess:
    env = dict(os.environ)
    env.pop("PS4UPLOAD_PIN", None)
    if env_extra:
        env.update(env_extra)
    return subprocess.run(
        [sys.executable, str(CLIENT), *args],
        capture_output=True, text=True, env=env, timeout=60,
    )


def main() -> int:
    server = HTTPServer(("127.0.0.1", 0), Handler)
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    host = ["--host", "127.0.0.1", "--port", str(port), "--pin", PIN]
    failures = []

    def check(condition, message):
        print(("PASS " if condition else "FAIL ") + message)
        if not condition:
            failures.append(message)

    result = run("status", "--host", "127.0.0.1", "--port", str(port))
    check(result.returncode == 0 and '"free_bytes": 123' in result.stdout,
          "status zeigt JSON")

    state["paths"].clear()
    result = run("diagnostics", *host)
    check(result.returncode == 0 and "/diagnostics" in state["paths"],
          "diagnostics erreicht den Payload")
    check("Payload 0.6.2" in result.stdout, "Version wird angezeigt")
    check("frei:   1.0 GiB" in result.stdout, "freier Platz ist lesbar")
    check("Quote:  unbegrenzt" in result.stdout, "Quote wird als unbegrenzt gezeigt")
    check("Schreibtest: ok, Datei entfernt" in result.stdout,
          "Schreibtest wird zusammengefasst")
    check("BGFT: geladen aus libSceBgft.sprx" in result.stdout,
          "BGFT-Modul wird gemeldet")
    check("sceBgftServiceIntInit: gefunden" in result.stdout,
          "gefundenes Symbol wird gezeigt")
    check("sceBgftServiceIntDownloadStartTask: fehlt (Code -1)" in result.stdout,
          "fehlendes Symbol nennt den Code")

    result = run("diagnostics", "--host", "127.0.0.1", "--port", str(port),
                 "--pin", "0000")
    check(result.returncode == 1 and "HTTP 403: pin" in result.stderr,
          "falsche PIN bei diagnostics gemeldet")

    result = run("diagnostics", "--host", "127.0.0.1", "--port", str(port))
    check(result.returncode == 2, "fehlender PIN bricht diagnostics ab")

    with tempfile.TemporaryDirectory() as temp:
        path = Path(temp) / "spiel.pkg"
        path.write_bytes(b"A" * 300_000)

        result = run("upload", str(path), *host)
        check(result.returncode == 0, "Upload ohne Resume")
        check(state["files"].get("spiel.pkg") == b"A" * 300_000, "Body vollstaendig")

        state["files"]["spiel.pkg"] = b"A" * 120_000
        state["parts"]["spiel.pkg"] = 120_000
        result = run("upload", str(path), *host, "--resume")
        check(result.returncode == 0, "Upload mit Resume")
        check(state["files"].get("spiel.pkg") == b"A" * 300_000, "Rest korrekt angehaengt")
        check("Setze bei 120000" in result.stdout, "Offset wird angezeigt")

        result = run("upload", str(path), "--host", "127.0.0.1",
                     "--port", str(port), "--pin", "0000")
        check(result.returncode == 1 and "pin" in result.stderr, "falsche PIN gemeldet")

        result = run("delete", "spiel.pkg", *host)
        check(result.returncode == 0 and "spiel.pkg" in state["deleted"],
              "DELETE erreicht den Payload")

        result = run("upload", str(path), "--host", "127.0.0.1",
                     "--port", str(port))
        check(result.returncode == 2, "fehlender PIN bricht ab")

        spaced = Path(temp) / "My Game.pkg"
        spaced.write_bytes(b"B" * 200_000)
        state["paths"].clear()
        result = run("upload", str(spaced), *host)
        check(result.returncode == 0, "Upload mit Leerzeichen im Namen")
        check(state["files"].get("My Game.pkg") == b"B" * 200_000,
              "Leerzeichen-Name kommt unverfaelscht an")
        check("/upload?name=My%20Game.pkg" in state["paths"],
              "POST-Name ist kodiert")

        state["files"]["My Game.pkg"] = b"B" * 50_000
        state["parts"]["My Game.pkg"] = 50_000
        state["paths"].clear()
        result = run("upload", str(spaced), *host, "--resume")
        check(result.returncode == 0, "Resume mit Leerzeichen im Namen")
        check(state["files"].get("My Game.pkg") == b"B" * 200_000,
              "Resume-Rest korrekt angehaengt")
        check("/upload?name=My%20Game.pkg" in state["paths"],
              "HEAD-Name ist kodiert")
        check("/upload?name=My%20Game.pkg&offset=50000" in state["paths"],
              "POST-Offset mit kodiertem Namen")

        # Kopf meldet mehr als die .part enthaelt: der Payload antwortet 409.
        state["parts"]["My Game.pkg"] = 50_000
        state["head_bias"] = 1000
        result = run("upload", str(spaced), *host, "--resume")
        state["head_bias"] = 0
        check(result.returncode == 2 and "50000" in result.stderr,
              "409 nennt den echten Offset und endet mit 2")

        # Alles schon uebertragen: kein Body mehr, nur noch Pruefung.
        state["files"]["spiel.pkg"] = b"A" * 300_000
        state["parts"]["spiel.pkg"] = 300_000
        result = run("upload", str(path), *host, "--resume")
        check(result.returncode == 0, "Resume mit 0 offenen Bytes")
        check("schon vollstaendig" in result.stdout, "Hinweis auf fertige Datei")

        # Fertiges Paket ohne .part-Rest: HEAD nennt Groesse und Complete,
        # der wiederholte Abschluss wird mit 200 beantwortet.
        state["files"]["spiel.pkg"] = b"A" * 300_000
        state["parts"].pop("spiel.pkg", None)
        state["completed"].add("spiel.pkg")
        state["paths"].clear()
        result = run("upload", str(path), *host, "--resume")
        check(result.returncode == 0, "Resume auf fertiges Paket ist Erfolg")
        check("300000" in result.stdout, "HEAD nennt die Groesse der fertigen Datei")
        check("/upload?name=spiel.pkg&offset=300000" in state["paths"],
              "Abschluss geht mit offset=Groesse")

        state["paths"].clear()
        result = run("delete", "My Game.pkg", *host)
        check(result.returncode == 0 and "My Game.pkg" in state["deleted"],
              "DELETE mit Leerzeichen erreicht den Payload")
        check("/upload?name=My%20Game.pkg" in state["paths"],
              "DELETE-Name ist kodiert")

    server.shutdown()
    print(f"\n{len(failures)} Fehler")
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
