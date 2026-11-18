#!/usr/bin/env python3
"""PS4Upload - send .pkg files from a computer to a jailbroken PS4.

The console pulls the package from this app over HTTP (Range requests are
supported, which the PS4 installer needs) and installs it through the
Remote Package Installer API on port 12800.
"""

from __future__ import annotations

import argparse
import ipaddress
import json
import os
import re
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import webbrowser
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

APP_NAME = "PS4Upload"
VERSION = "0.2.0"
PKG_MAGIC = b"\x7fCNT"
RPI_PORT = 12800
GOLDHEN_PORT = 9090
CHUNK = 256 * 1024
PAYLOAD_MARKER = b"\xb4" * 6
PACKAGE_TYPES = {
    "gd": "PS4GD",
    "gp": "PS4DP",
    "ac": "PS4AC",
    "la": "PS4LA",
    "gde": "PS4GDE",
}

TITLE_ID_RE = re.compile(r"(CUSA\d{5}|PPSA\d{5})", re.IGNORECASE)
SPLIT_RE = re.compile(r"^(?P<base>.+?)_(?P<idx>\d{1,4})\.pkg$", re.IGNORECASE)
SPLIT_DOT_RE = re.compile(r"^(?P<base>.+?)\.pkg\.(?P<idx>\d{1,4})$", re.IGNORECASE)
HEX_RE = re.compile(r"(?<![\w\"'])0x([0-9a-fA-F]+)")

ERROR_HINTS = {
    0x80990033: "Die PS4 konnte die PKG-URL nicht laden. PC-IP, Port und Firewall pruefen.",
}


# --------------------------------------------------------------------------- utils


def lan_ip() -> str:
    """Best local address a console on the same network can reach."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def discover_networks(default_ip: str) -> list[dict]:
    """Local IPv4 addresses with their adapter names (best effort)."""
    found: dict[str, dict] = {}

    def add(name: str, ip: str, prefix: int = 24) -> None:
        if not ip or ip.startswith("127."):
            return
        try:
            ipaddress.ip_address(ip)
        except ValueError:
            return
        label = (name or "Netzwerk").strip()
        entry = found.get(ip)
        if entry is None:
            found[ip] = {"name": label, "ip": ip, "prefix": prefix or 24}
            return
        placeholders = ("Standard", "Hostname", "Netzwerk")
        if entry["name"] in placeholders and label not in placeholders:
            entry["name"] = label
        if prefix and prefix != 24:
            entry["prefix"] = prefix

    add("Standard", default_ip)

    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            add("Hostname", info[4][0])
    except OSError:
        pass

    if os.name == "nt":
        try:
            raw = subprocess.run(["ipconfig"], capture_output=True, timeout=6).stdout
            current = "Netzwerk"
            for line in raw.decode("oem", "replace").splitlines():
                stripped = line.strip()
                if line[:1] not in (" ", "\t") and stripped.endswith(":") and "adapter" in stripped.lower():
                    current = stripped[:-1]
                    index = current.lower().find("adapter")
                    if index >= 0:
                        current = current[index + len("adapter"):].strip(" :-/") or current
                elif "IPv4" in line:
                    match = re.search(r"(\d{1,3}(?:\.\d{1,3}){3})", line)
                    if match:
                        add(current, match.group(1))
        except (OSError, subprocess.SubprocessError):
            pass
    else:
        try:
            completed = subprocess.run(
                ["ip", "-o", "-4", "addr", "show"], capture_output=True, timeout=4, text=True
            )
            for line in completed.stdout.splitlines():
                parts = line.split()
                if "inet" not in parts:
                    continue
                try:
                    name = parts[1].rstrip(":")
                    address = parts[parts.index("inet") + 1].split("/")[0]
                except (IndexError, ValueError):
                    continue
                add(name, address)
        except (OSError, subprocess.SubprocessError):
            pass

    ordered = [entry for ip, entry in found.items() if ip == default_ip]
    ordered += [entry for ip, entry in found.items() if ip != default_ip]
    return ordered


def human_size(n: int) -> str:
    if n <= 0:
        return "0 B"
    units = ("B", "KB", "MB", "GB", "TB")
    value = float(n)
    for unit in units:
        if value < 1024 or unit == units[-1]:
            return f"{value:.0f} {unit}" if unit == "B" else f"{value:.1f} {unit}"
        value /= 1024
    return f"{n} B"


def loads_lenient(text: str) -> dict:
    """RPI answers with bare hex numbers (0x...) which are not valid JSON."""
    cleaned = HEX_RE.sub(lambda m: str(int(m.group(1), 16)), text)
    try:
        value = json.loads(cleaned)
    except ValueError:
        return {"status": "fail", "error": f"Unerwartete Antwort: {text[:200]}"}
    return value if isinstance(value, dict) else {"status": "fail", "error": "Unerwartete Antwort"}


def as_int(value, default: int = 0) -> int:
    try:
        if isinstance(value, bool):
            return int(value)
        if isinstance(value, (int, float)):
            return int(value)
        text = str(value).strip()
        if text.lower().startswith("0x"):
            return int(text, 16)
        return int(float(text))
    except (TypeError, ValueError):
        return default


def is_pkg_file(path: Path) -> bool:
    try:
        with open(path, "rb") as fh:
            return fh.read(4) == PKG_MAGIC
    except OSError:
        return False


def parse_sfo(buf: bytes) -> dict:
    """Minimal PARAM.SFO reader for the keys PS4Upload needs."""
    values: dict[str, str] = {}
    if len(buf) < 0x14 or buf[:4] != b"\0PSF":
        return values
    keys_off = int.from_bytes(buf[8:12], "little")
    data_off = int.from_bytes(buf[12:16], "little")
    count = int.from_bytes(buf[16:20], "little")
    for index in range(min(count, 256)):
        entry = buf[0x14 + index * 16 : 0x14 + (index + 1) * 16]
        if len(entry) < 16:
            break
        key_off = int.from_bytes(entry[0:2], "little")
        fmt = int.from_bytes(entry[2:4], "little")
        length = int.from_bytes(entry[4:8], "little")
        value_off = int.from_bytes(entry[12:16], "little")
        key_start = keys_off + key_off
        key_end = buf.find(b"\0", key_start)
        if key_start >= len(buf) or key_end < 0:
            continue
        key = buf[key_start:key_end].decode("utf-8", "replace")
        raw = buf[data_off + value_off : data_off + value_off + length]
        if fmt == 0x0204:
            values[key] = raw.rstrip(b"\0").decode("utf-8", "replace")
        elif fmt == 0x0404 and len(raw) >= 4:
            values[key] = str(int.from_bytes(raw[:4], "little"))
    return values


def read_pkg_meta(path: Path) -> dict:
    """Read content id, title and BGFT package type from a PS4 PKG."""
    meta = {
        "content_id": "",
        "title": "",
        "title_id": "",
        "category": "",
        "app_ver": "",
        "package_type": "PS4GD",
    }
    try:
        with open(path, "rb") as fh:
            head = fh.read(0x1000)
            if head[:4] != PKG_MAGIC or len(head) < 0x80:
                return meta
            content_id = head[0x40:0x64].split(b"\0")[0].decode("ascii", "ignore").strip()
            meta["content_id"] = content_id
            entry_count = int.from_bytes(head[0x10:0x14], "big")
            table_offset = int.from_bytes(head[0x18:0x1C], "big")
            if 0 < entry_count <= 1024:
                fh.seek(table_offset)
                table = fh.read(entry_count * 0x20)
                sfo_offset = sfo_size = 0
                for index in range(entry_count):
                    entry = table[index * 0x20 : (index + 1) * 0x20]
                    if len(entry) < 0x20:
                        break
                    if int.from_bytes(entry[0:4], "big") == 0x1000:
                        sfo_offset = int.from_bytes(entry[0x10:0x14], "big")
                        sfo_size = int.from_bytes(entry[0x14:0x18], "big")
                        break
                if 0 < sfo_size <= 4 * 1024 * 1024:
                    fh.seek(sfo_offset)
                    values = parse_sfo(fh.read(sfo_size))
                    meta["title"] = values.get("TITLE", "")
                    meta["title_id"] = values.get("TITLE_ID", "")
                    meta["category"] = values.get("CATEGORY", "")
                    meta["app_ver"] = values.get("APP_VER", "")
                    if not meta["content_id"]:
                        meta["content_id"] = values.get("CONTENT_ID", "")
    except OSError:
        return meta
    meta["package_type"] = PACKAGE_TYPES.get(meta["category"].lower(), "PS4GD")
    return meta


def patch_payload(data: bytes, host_ip: str, port: int) -> bytes:
    """Patch the installer payload with this computer's callback address."""
    offset = data.find(PAYLOAD_MARKER)
    if offset < 0:
        raise ValueError("GoldHEN-Payload hat keinen Verbindungs-Marker.")
    address = ipaddress.IPv4Address(host_ip).packed
    patched = bytearray(data)
    patched[offset : offset + 4] = address
    patched[offset + 4 : offset + 6] = struct.pack(">H", port)
    return bytes(patched)


def resource_path(relative: str) -> Path:
    base = Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent))
    return base / relative


def split_info(path: Path):
    for pattern in (SPLIT_RE, SPLIT_DOT_RE):
        m = pattern.match(path.name)
        if m:
            return m.group("base").lower(), int(m.group("idx"))
    return None


def group_files(files: list[Path]) -> list[list[Path]]:
    """Group split packages (_0.pkg, _1.pkg, ...) into one job."""
    buckets: dict[str, dict[int, Path]] = {}
    order: list[str] = []
    for path in files:
        info = split_info(path)
        key = info[0] if info else f"file:{path}"
        if key not in buckets:
            buckets[key] = {}
            order.append(key)
        buckets[key][info[1] if info else 0] = path

    groups: list[list[Path]] = []
    for key in order:
        parts = buckets[key]
        indexes = sorted(parts)
        if len(indexes) > 1 and indexes == list(range(len(indexes))):
            groups.append([parts[i] for i in indexes])
        else:
            groups.extend([[parts[i]] for i in indexes])
    return groups


def job_name(files: list[Path]) -> str:
    if len(files) == 1:
        return files[0].stem
    info = split_info(files[0])
    if info:
        for suffix in (f"_{info[1]}.pkg", f".pkg.{info[1]}"):
            if files[0].name.lower().endswith(suffix.lower()):
                return files[0].name[: -len(suffix)]
    return files[0].stem + f" (+{len(files) - 1} Teile)"


def title_id_of(files: list[Path]) -> str:
    for path in files:
        m = TITLE_ID_RE.search(path.name)
        if m:
            return m.group(1).upper()
    return ""


def parse_range(header: str | None, size: int):
    """Return (start, end) inclusive, None for the whole file, or 'invalid'."""
    if not header:
        return None
    m = re.match(r"^bytes=(\d*)-(\d*)$", header.strip())
    if not m:
        return None
    first, last = m.group(1), m.group(2)
    if not first and not last:
        return None
    if size <= 0:
        return "invalid"
    if not first:
        length = int(last)
        if length <= 0:
            return "invalid"
        start = max(0, size - length)
        end = size - 1
    else:
        start = int(first)
        end = int(last) if last else size - 1
    if start >= size or start > end:
        return "invalid"
    return start, min(end, size - 1)


def is_loopback(host: str) -> bool:
    try:
        address = ipaddress.ip_address(host.split("%", 1)[0])
    except ValueError:
        return False
    if isinstance(address, ipaddress.IPv6Address) and address.ipv4_mapped is not None:
        address = address.ipv4_mapped
    return address.is_loopback


def allowed_for(host: str, path: str) -> bool:
    """Loopback may use UI and API; other clients only fetch package bytes."""
    return is_loopback(host) or path.startswith("/pkg/")


# ----------------------------------------------------------------------------- app


@dataclass
class Job:
    id: int
    files: list[Path]
    name: str
    title_id: str
    size: int
    content_id: str = ""
    package_type: str = "PS4GD"
    pkg_title: str = ""
    mode: str = ""
    state: str = "queued"
    task_id: int | None = None
    transferred: int = 0
    total: int = 0
    percent: float = 0.0
    speed: float = 0.0
    rest_sec: int = 0
    message: str = ""
    error: str = ""

    def to_dict(self) -> dict:
        return {
            "id": self.id,
            "name": self.name,
            "title_id": self.title_id,
            "content_id": self.content_id,
            "package_type": self.package_type,
            "mode": self.mode,
            "files": [p.name for p in self.files],
            "parts": len(self.files),
            "size": self.size,
            "state": self.state,
            "task_id": self.task_id,
            "transferred": self.transferred,
            "total": self.total,
            "percent": round(self.percent, 1),
            "speed": round(self.speed, 1),
            "rest_sec": self.rest_sec,
            "message": self.message,
            "error": self.error,
        }

    def reset_progress(self) -> None:
        self.task_id = None
        self.transferred = 0
        self.total = 0
        self.percent = 0.0
        self.speed = 0.0
        self.rest_sec = 0
        self.message = ""
        self.error = ""
        self.state = "queued"


class App:
    def __init__(
        self,
        host_ip: str,
        port: int,
        console_ip: str,
        console_port: int,
        mode: str = "auto",
        goldhen_port: int = GOLDHEN_PORT,
        goldhen_payload: str = "",
    ):
        self.host_ip = host_ip.strip()
        self.default_ip = lan_ip()
        self.port = port
        self.console_ip = console_ip
        self.console_port = console_port
        self.mode = mode if mode in ("auto", "rpi", "goldhen") else "auto"
        self.goldhen_port = goldhen_port
        self.goldhen_payload = (
            Path(goldhen_payload).expanduser().resolve()
            if goldhen_payload
            else resource_path("goldhen/payload.bin")
        )
        self.lock = threading.RLock()
        self.jobs: dict[int, Job] = {}
        self.next_id = 1
        self.logs: list[str] = []
        self.stop_events: dict[int, threading.Event] = {}
        self.worker_running = False
        self.active_id: int | None = None
        self._probe: dict | None = None
        self._probe_at = 0.0
        self._nets: list[dict] = []
        self._nets_at = 0.0

    # -- logging ---------------------------------------------------------

    def log(self, message: str) -> None:
        line = f"[{time.strftime('%H:%M:%S')}] {message}"
        with self.lock:
            self.logs.append(line)
            del self.logs[:-400]
        print(line, flush=True)

    # -- console ---------------------------------------------------------

    def _tcp_reachable(self, host: str, port: int, timeout: float = 0.5) -> bool:
        try:
            with socket.create_connection((host, port), timeout=timeout):
                return True
        except OSError:
            return False

    def console_info(self) -> dict:
        now = time.time()
        if self._probe and now - self._probe_at < 3.0:
            return self._probe

        ip = self.console_ip.strip()
        info = {
            "ip": ip,
            "port": self.console_port,
            "reachable": False,
            "api": None,
            "rpi": False,
            "goldhen": False,
            "goldhen_port": self.goldhen_port,
        }
        if ip:
            info["rpi"] = self._tcp_reachable(ip, self.console_port, timeout=0.4)
            info["goldhen"] = self._tcp_reachable(ip, self.goldhen_port, timeout=0.4)
            info["reachable"] = info["rpi"] or info["goldhen"]
            if info["rpi"]:
                info["api"] = "legacy"
                try:
                    url = f"http://{ip}:{self.console_port}/api/v2/system"
                    with urllib.request.urlopen(url, timeout=0.7) as resp:
                        if resp.status == 200:
                            info["api"] = "v2"
                except (urllib.error.URLError, OSError, ValueError):
                    pass
        self._probe = info
        self._probe_at = now
        return info

    def set_console(self, ip: str) -> None:
        ip = ip.strip()
        if ip.lower().startswith("http://"):
            ip = ip[7:]
        elif ip.lower().startswith("https://"):
            ip = ip[8:]
        ip = ip.rstrip("/")
        self.console_ip = ip
        self._probe = None
        self._probe_at = 0.0
        self.log(f"PS4-Adresse: {ip or '(leer)'}")

    # -- network ---------------------------------------------------------

    def network_options(self, refresh: bool = False) -> list[dict]:
        now = time.time()
        with self.lock:
            if not refresh and self._nets and now - self._nets_at < 10.0:
                return [dict(item) for item in self._nets]
        nets = discover_networks(self.default_ip)
        with self.lock:
            self._nets = nets
            self._nets_at = now
        return [dict(item) for item in nets]

    def effective_ip(self) -> str:
        if self.host_ip:
            return self.host_ip
        options = self.network_options()
        if self.console_ip:
            try:
                console = ipaddress.ip_address(self.console_ip)
            except ValueError:
                console = None
            if console is not None:
                for option in options:
                    try:
                        subnet = ipaddress.ip_network(
                            f"{option['ip']}/{option['prefix']}", strict=False
                        )
                    except ValueError:
                        continue
                    if console in subnet:
                        return option["ip"]
        return options[0]["ip"] if options else self.default_ip

    def network_state(self) -> dict:
        options = self.network_options()
        if self.host_ip and not any(item["ip"] == self.host_ip for item in options):
            options.insert(0, {"name": "Manuell", "ip": self.host_ip, "prefix": 24})
        effective = self.effective_ip()
        console_match = None
        if self.console_ip:
            try:
                subnet = ipaddress.ip_network(f"{effective}/24", strict=False)
                console_match = ipaddress.ip_address(self.console_ip) in subnet
            except ValueError:
                console_match = None
        return {
            "selected": self.host_ip,
            "ip": effective,
            "url": f"http://{effective}:{self.port}",
            "console_match": console_match,
            "options": [
                {**item, "url": f"http://{item['ip']}:{self.port}"} for item in options
            ],
        }

    def set_network(self, ip: str) -> None:
        ip = ip.strip()
        if ip:
            try:
                ipaddress.ip_address(ip)
            except ValueError:
                raise ValueError("Ungueltige IP-Adresse.") from None
            if not any(item["ip"] == ip for item in self.network_options(refresh=True)):
                raise ValueError("Netzwerk nicht gefunden - bitte die Liste neu laden.")
        self.host_ip = ip
        self.log(f"PC-Netzwerk: {ip or 'automatisch'}")

    def install_mode(self) -> str:
        if self.mode in ("rpi", "goldhen"):
            return self.mode
        info = self.console_info()
        if info["rpi"]:
            return "rpi"
        if info["goldhen"]:
            return "goldhen"
        return "rpi"

    def set_mode(self, mode: str) -> None:
        mode = mode.strip().lower()
        if mode not in ("auto", "rpi", "goldhen"):
            raise ValueError("Unbekannter Modus.")
        self.mode = mode
        self._probe = None
        self._probe_at = 0.0
        self.log(f"Installationsmodus: {mode}")

    def ps4_post(self, path: str, payload: dict, timeout: float = 15.0) -> dict:
        if not self.console_ip:
            return {"status": "fail", "error": "Keine PS4-IP eingetragen."}
        url = f"http://{self.console_ip}:{self.console_port}{path}"
        data = json.dumps(payload).encode("utf-8")
        request = urllib.request.Request(
            url,
            data=data,
            headers={"Content-Type": "application/json", "Connection": "close"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=timeout) as resp:
                return loads_lenient(resp.read().decode("utf-8", "replace"))
        except urllib.error.HTTPError as exc:
            body = exc.read().decode("utf-8", "replace")
            result = loads_lenient(body) if body.strip() else {}
            result.setdefault("status", "fail")
            result.setdefault("error", f"HTTP {exc.code}")
            return result
        except (urllib.error.URLError, OSError) as exc:
            reason = getattr(exc, "reason", exc)
            return {"status": "fail", "error": f"PS4 nicht erreichbar ({reason})"}

    # -- jobs ------------------------------------------------------------

    def add_path(self, raw: str) -> tuple[list[Job], int]:
        raw = raw.strip().strip('"').strip("'")
        if not raw:
            raise ValueError("Bitte einen Pfad angeben.")
        path = Path(os.path.expanduser(raw))
        try:
            path = path.resolve()
        except OSError:
            pass
        if not path.exists():
            raise ValueError(f"Pfad nicht gefunden: {path}")
        if path.is_dir():
            files = sorted(
                f for f in path.iterdir() if f.is_file() and f.suffix.lower() == ".pkg"
            )
            if not files:
                raise ValueError(f"Keine .pkg-Dateien in {path}")
        else:
            files = [path]

        for file in files:
            if file.suffix.lower() != ".pkg":
                raise ValueError(f"Keine .pkg-Datei: {file.name}")
            if not is_pkg_file(file):
                raise ValueError(f"{file.name} ist keine PS4-PKG (Header 7f434e54 fehlt).")

        added: list[Job] = []
        skipped = 0
        with self.lock:
            known = {
                tuple(j.files)
                for j in self.jobs.values()
                if j.state not in ("error", "stopped")
            }
            for group in group_files(files):
                key = tuple(group)
                if key in known:
                    skipped += 1
                    continue
                meta = read_pkg_meta(group[0])
                job = Job(
                    id=self.next_id,
                    files=group,
                    name=job_name(group),
                    title_id=meta["title_id"] or title_id_of(group),
                    size=sum(p.stat().st_size for p in group),
                    content_id=meta["content_id"],
                    package_type=meta["package_type"],
                    pkg_title=meta["title"],
                )
                self.jobs[job.id] = job
                self.next_id += 1
                known.add(key)
                added.append(job)
                self.log(
                    f"Hinzugefuegt: {job.name} ({human_size(job.size)}"
                    + (f", {len(group)} Teile" if len(group) > 1 else "")
                    + ")"
                )
        if not added and not skipped:
            raise ValueError("Nichts hinzugefuegt.")
        return added, skipped

    def job(self, job_id: int) -> Job | None:
        with self.lock:
            return self.jobs.get(job_id)

    def start_job(self, job: Job) -> None:
        with self.lock:
            if job.state not in ("queued", "error", "stopped"):
                raise ValueError("Dieser Auftrag laeuft bereits.")
            job.reset_progress()
        with self.lock:
            if not self.worker_running:
                self.worker_running = True
                threading.Thread(target=self._worker, name="ps4upload-worker", daemon=True).start()
        self.log(f"Eingereiht: {job.name}")

    def stop_job(self, job: Job) -> None:
        with self.lock:
            event = self.stop_events.get(job.id)
            if event is not None:
                event.set()
                self.log(f"Stoppe: {job.name}")
                return
            if job.state == "queued":
                job.state = "stopped"
                job.message = "Gestoppt"
                job.percent = 0.0
                self.log(f"Gestoppt: {job.name}")
                return
            if job.state == "installing" and job.task_id:
                threading.Thread(
                    target=self._stop_task, args=(job,), name="ps4upload-stop", daemon=True
                ).start()

    def _stop_task(self, job: Job) -> None:
        result = self.ps4_post("/api/stop_task", {"task_id": job.task_id}, timeout=8.0)
        if result.get("status") == "success":
            job.state = "stopped"
            job.message = "Installation auf der PS4 gestoppt"
            self.log(f"Task {job.task_id} gestoppt: {job.name}")
        else:
            self.log(f"Stoppen fehlgeschlagen: {result.get('error') or result}")

    def remove_job(self, job: Job) -> None:
        with self.lock:
            if job.state in ("sending", "transferring") or (
                job.state == "installing" and job.mode != "goldhen"
            ):
                raise ValueError("Auftrag laeuft noch - bitte zuerst stoppen.")
            self.jobs.pop(job.id, None)
        self.log(f"Entfernt: {job.name}")

    def _worker(self) -> None:
        try:
            while True:
                with self.lock:
                    job = next((j for j in self.jobs.values() if j.state == "queued"), None)
                    if job is None:
                        return
                    self.active_id = job.id
                self._run(job)
        finally:
            with self.lock:
                self.worker_running = False
                self.active_id = None

    def _pkg_url(self, job: Job, index: int) -> str:
        name = urllib.parse.quote(job.files[index].name)
        host = self.effective_ip()
        return f"http://{host}:{self.port}/pkg/{job.id}/{index}/{name}"

    def _fail(self, job: Job, message: str) -> None:
        job.state = "error"
        job.error = message
        job.message = message
        job.speed = 0.0
        self.log(f"Fehler bei {job.name}: {message}")

    def _run(self, job: Job) -> None:
        if not self.console_ip:
            self._fail(job, "Keine PS4-IP eingetragen.")
            return
        if self.install_mode() == "goldhen":
            self._run_goldhen(job)
        else:
            self._run_rpi(job)

    def _run_goldhen(self, job: Job) -> None:
        stop = threading.Event()
        with self.lock:
            self.stop_events[job.id] = stop
        listener = None
        try:
            if len(job.files) != 1:
                self._fail(
                    job,
                    "Der GoldHEN-Modus unterstuetzt nur einzelne PKG-Dateien. "
                    "Geteilte Pakete brauchen den Remote Package Installer.",
                )
                return
            if not job.content_id:
                self._fail(job, "Content-ID konnte nicht aus der PKG gelesen werden.")
                return
            if not self.goldhen_payload.is_file():
                self._fail(job, f"GoldHEN-Payload fehlt: {self.goldhen_payload}")
                return

            job.mode = "goldhen"
            job.state = "sending"
            job.message = "GoldHEN-Payload wird gesendet ..."
            self.log(f"Sende GoldHEN-Payload an {self.console_ip}:{self.goldhen_port}")

            listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind(("0.0.0.0", 0))
            listener.listen(2)
            listener.settimeout(1.0)
            callback_ip = self.effective_ip()
            callback_port = listener.getsockname()[1]
            patched = patch_payload(
                self.goldhen_payload.read_bytes(), callback_ip, callback_port
            )

            try:
                with socket.create_connection(
                    (self.console_ip, self.goldhen_port), timeout=5.0
                ) as loader:
                    loader.sendall(patched)
                    loader.shutdown(socket.SHUT_WR)
            except OSError as exc:
                self._fail(
                    job,
                    f"GoldHEN BinLoader auf Port {self.goldhen_port} nicht erreichbar ({exc}).",
                )
                return

            job.message = "Warte auf den Installer auf der PS4 ..."
            connection = None
            deadline = time.time() + 20.0
            while time.time() < deadline and not stop.is_set():
                try:
                    candidate, address = listener.accept()
                except socket.timeout:
                    continue
                except OSError:
                    break
                if address[0] in (self.console_ip, callback_ip):
                    connection = candidate
                    break
                candidate.close()

            if stop.is_set():
                job.state = "stopped"
                job.message = "Gestoppt"
                return
            if connection is None:
                self._fail(
                    job,
                    "Die GoldHEN-Payload hat sich nicht zurueckgemeldet - Firewall pruefen.",
                )
                return

            with connection:
                connection.settimeout(10.0)
                connection.sendall(self._goldhen_message(job))

            job.state = "installing"
            job.percent = 0.0
            job.total = job.size
            job.transferred = 0
            job.speed = 0.0
            job.message = "GoldHEN: Installation angestossen - Fortschritt auf der PS4."
            self.log(f"GoldHEN-Auftrag uebergeben: {job.name}")

            listener.settimeout(4.0)
            try:
                exit_socket, address = listener.accept()
                if address[0] in (self.console_ip, callback_ip):
                    exit_socket.sendall(struct.pack("<I", 0))
                exit_socket.close()
            except (socket.timeout, OSError):
                pass
        except (OSError, ValueError) as exc:
            self._fail(job, f"GoldHEN-Fehler: {exc}")
        finally:
            if listener is not None:
                try:
                    listener.close()
                except OSError:
                    pass
            with self.lock:
                self.stop_events.pop(job.id, None)

    def _goldhen_message(self, job: Job) -> bytes:
        def field(text: str) -> bytes:
            raw = text.encode("utf-8", "replace")
            return struct.pack("<I", len(raw)) + raw

        return (
            struct.pack("<I", 1)
            + field(self._pkg_url(job, 0))
            + field(job.pkg_title or job.name)
            + field(job.content_id)
            + field(job.package_type or "PS4GD")
            + struct.pack("<Q", job.size)
            + struct.pack("<I", 0)
        )

    def _run_rpi(self, job: Job) -> None:
        stop = threading.Event()
        with self.lock:
            self.stop_events[job.id] = stop
        try:
            if not self.console_ip:
                self._fail(job, "Keine PS4-IP eingetragen.")
                return
            job.mode = "rpi"
            job.state = "sending"
            job.message = "Installationsauftrag wird an die PS4 gesendet ..."
            urls = [self._pkg_url(job, i) for i in range(len(job.files))]
            self.log(f"Sende Installationsauftrag fuer {job.name} an {self.console_ip}")
            result = self.ps4_post("/api/install", {"type": "direct", "packages": urls}, timeout=20.0)

            if result.get("status") != "success":
                code = result.get("error_code")
                if code is not None:
                    value = as_int(code)
                    hint = ERROR_HINTS.get(value)
                    message = f"PS4-Fehler 0x{value:08X}"
                    if hint:
                        message += f" - {hint}"
                else:
                    message = str(result.get("error") or "Die PS4 hat den Auftrag abgelehnt.")
                self._fail(job, message)
                return

            task_id = as_int(result.get("task_id"), -2)
            if task_id == -1:
                job.state = "done"
                job.percent = 100.0
                job.message = "War auf der PS4 bereits installiert."
                self.log(f"{job.name} ist bereits installiert.")
                return
            if task_id < 0:
                self._fail(job, "Die PS4 hat keine Task-ID zurueckgegeben.")
                return

            job.task_id = task_id
            if result.get("title"):
                job.name = str(result["title"])
            job.state = "transferring"
            job.message = "PS4 laedt die PKG ..."
            self.log(f"Task {task_id} gestartet: {job.name}")

            last_bytes = 0
            last_time = time.time()
            misses = 0

            while not stop.is_set():
                progress = self.ps4_post(
                    "/api/get_task_progress", {"task_id": task_id}, timeout=10.0
                )
                if progress.get("status") != "success":
                    misses += 1
                    if misses >= 3:
                        job.state = "installing"
                        job.percent = 100.0 if job.total and job.transferred >= job.total else job.percent
                        job.message = "Kein Fortschritt abrufbar - die PS4 installiert im Hintergrund."
                        self.log(f"Kein Fortschritt fuer Task {task_id}; Installation laeuft weiter.")
                        return
                    time.sleep(1.0)
                    continue
                misses = 0

                error = as_int(progress.get("error"))
                if error:
                    hint = ERROR_HINTS.get(error)
                    message = f"PS4-Fehler 0x{error:08X}"
                    if hint:
                        message += f" - {hint}"
                    self._fail(job, message)
                    return

                total = as_int(progress.get("length_total"))
                done = as_int(progress.get("transferred_total"))
                if total > 0:
                    job.total = total
                    job.transferred = max(0, min(done, total))
                    job.percent = job.transferred * 100.0 / total

                now = time.time()
                if now - last_time >= 0.5:
                    delta = max(0, done - last_bytes)
                    instant = delta / (now - last_time)
                    job.speed = instant if job.speed <= 0 else job.speed * 0.6 + instant * 0.4
                    last_bytes = done
                    last_time = now
                job.rest_sec = max(0, as_int(progress.get("rest_sec_total")))

                if job.total > 0 and job.transferred >= job.total:
                    job.state = "installing"
                    job.percent = 100.0
                    job.speed = 0.0
                    job.rest_sec = 0
                    job.message = "Download fertig - die PS4 installiert jetzt."
                    self.log(f"Download fertig: {job.name}; PS4 installiert.")
                    return

                time.sleep(1.0)

            self.ps4_post("/api/stop_task", {"task_id": task_id}, timeout=8.0)
            job.state = "stopped"
            job.message = "Gestoppt"
            job.speed = 0.0
            self.log(f"Task {task_id} gestoppt: {job.name}")
        except Exception as exc:  # keep the worker alive
            self._fail(job, f"Unerwarteter Fehler: {exc}")
        finally:
            with self.lock:
                self.stop_events.pop(job.id, None)

    # -- state -----------------------------------------------------------

    def state(self) -> dict:
        with self.lock:
            jobs = [self.jobs[i].to_dict() for i in sorted(self.jobs)]
            logs = list(self.logs[-80:])
            active = self.active_id
        network = self.network_state()
        return {
            "version": VERSION,
            "mode": self.mode,
            "install_mode": self.install_mode(),
            "goldhen_payload": {
                "path": str(self.goldhen_payload),
                "exists": self.goldhen_payload.is_file(),
            },
            "server": {
                "url": network["url"],
                "ip": network["ip"],
                "port": self.port,
            },
            "network": network,
            "console": self.console_info(),
            "active": active,
            "jobs": jobs,
            "logs": logs,
        }


# --------------------------------------------------------------------------- http


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = f"PS4Upload/{VERSION}"

    @property
    def app(self) -> App:
        return self.server.app  # type: ignore[attr-defined]

    def log_message(self, fmt, *args) -> None:  # noqa: A003 - stdlib signature
        pass

    # -- responses -------------------------------------------------------

    def _send_bytes(self, body: bytes, content_type: str, status: int = 200) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _send_json(self, obj: dict, status: int = 200) -> None:
        self._send_bytes(
            json.dumps(obj, ensure_ascii=False).encode("utf-8"),
            "application/json; charset=utf-8",
            status,
        )

    def _send_error_json(self, message: str, status: int) -> None:
        self._send_json({"error": message}, status)

    def _deny_remote(self, path: str) -> None:
        message = "Zugriff nur lokal. Im LAN sind nur /pkg/...-Downloads erlaubt."
        if path.startswith("/api"):
            self._send_error_json(message, 403)
        else:
            self._send_bytes(message.encode("utf-8"), "text/plain; charset=utf-8", 403)

    # -- parsing ---------------------------------------------------------

    def _read_json(self):
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            length = 0
        if length <= 0:
            return {}
        if length > 2_000_000:
            return None
        raw = self.rfile.read(length)
        try:
            body = json.loads(raw.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return None
        return body if isinstance(body, dict) else {}

    # -- routes ----------------------------------------------------------

    def do_GET(self) -> None:  # noqa: N802 - stdlib naming
        try:
            path = urllib.parse.urlsplit(self.path).path
            if not allowed_for(str(self.client_address[0]), path):
                self._deny_remote(path)
                return
            if path == "/":
                self._send_bytes(PAGE.encode("utf-8"), "text/html; charset=utf-8")
            elif path == "/api/state":
                self._send_json(self.app.state())
            elif path.startswith("/pkg/"):
                self._serve_file(path)
            elif path == "/favicon.ico":
                self.send_response(204)
                self.send_header("Content-Length", "0")
                self.end_headers()
            else:
                self._send_error_json("Nicht gefunden", 404)
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as exc:  # pragma: no cover - defensive
            try:
                self._send_error_json(f"Fehler: {exc}", 500)
            except OSError:
                pass

    def do_HEAD(self) -> None:  # noqa: N802 - stdlib naming
        self.do_GET()

    def do_POST(self) -> None:  # noqa: N802 - stdlib naming
        try:
            path = urllib.parse.urlsplit(self.path).path
            if not is_loopback(str(self.client_address[0])):
                self._deny_remote(path)
                return
            body = self._read_json()
            if body is None:
                self._send_error_json("Ungueltiges JSON", 400)
                return

            if path == "/api/ps4":
                ip = str(body.get("ip") or "").strip()
                self.app.set_console(ip)
                self._send_json({"ok": True, "console": self.app.console_info()})
                return

            if path == "/api/network":
                try:
                    self.app.set_network(str(body.get("ip") or ""))
                except ValueError as exc:
                    self._send_error_json(str(exc), 400)
                    return
                self._send_json({"ok": True, "network": self.app.network_state()})
                return

            if path == "/api/mode":
                try:
                    self.app.set_mode(str(body.get("mode") or "auto"))
                except ValueError as exc:
                    self._send_error_json(str(exc), 400)
                    return
                self._send_json({"ok": True, "mode": self.app.mode})
                return

            if path == "/api/add":
                try:
                    added, skipped = self.app.add_path(str(body.get("path") or ""))
                except ValueError as exc:
                    self._send_error_json(str(exc), 400)
                    return
                message = f"{len(added)} Auftrag/Auftraege hinzugefuegt"
                if skipped:
                    message += f", {skipped} bereits vorhanden"
                self._send_json(
                    {"ok": True, "added": [j.to_dict() for j in added], "message": message}
                )
                return

            job_id = as_int(body.get("id"), -1)
            job = self.app.job(job_id)
            if job is None:
                self._send_error_json("Auftrag nicht gefunden", 404)
                return

            if path == "/api/install":
                try:
                    self.app.start_job(job)
                except ValueError as exc:
                    self._send_error_json(str(exc), 400)
                    return
                self._send_json({"ok": True})
                return

            if path == "/api/stop":
                self.app.stop_job(job)
                self._send_json({"ok": True})
                return

            if path == "/api/remove":
                try:
                    self.app.remove_job(job)
                except ValueError as exc:
                    self._send_error_json(str(exc), 400)
                    return
                self._send_json({"ok": True})
                return

            self._send_error_json("Nicht gefunden", 404)
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as exc:  # pragma: no cover - defensive
            try:
                self._send_error_json(f"Fehler: {exc}", 500)
            except OSError:
                pass

    def _serve_file(self, path: str) -> None:
        parts = path.strip("/").split("/")
        if len(parts) != 4:
            self._send_error_json("Nicht gefunden", 404)
            return
        _, raw_id, raw_index, raw_name = parts
        job = self.app.job(as_int(raw_id, -1))
        index = as_int(raw_index, -1)
        if job is None or index < 0 or index >= len(job.files):
            self._send_error_json("Paket nicht gefunden", 404)
            return
        file = job.files[index]
        try:
            size = file.stat().st_size
            handle = open(file, "rb")
        except OSError:
            self._send_error_json("Datei nicht lesbar", 404)
            return

        with handle:
            requested = parse_range(self.headers.get("Range"), size)
            if requested == "invalid":
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{size}")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return

            start, end = requested or (0, max(0, size - 1))
            length = 0 if size == 0 else end - start + 1
            self.send_response(206 if requested else 200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(length))
            if requested:
                self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.end_headers()

            if self.command == "HEAD" or length == 0:
                return
            handle.seek(start)
            remaining = length
            while remaining > 0:
                chunk = handle.read(min(CHUNK, remaining))
                if not chunk:
                    break
                self.wfile.write(chunk)
                remaining -= len(chunk)


PAGE = r"""<!doctype html>
<html lang="de">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>PS4Upload</title>
<style>
:root{
  --bg:#0b0f15; --card:#121a24; --card2:#0f1620; --line:#22303f; --text:#e8eef7;
  --muted:#8fa0b5; --accent:#4d9dff; --ok:#3ddc84; --bad:#ff5f5f; --warn:#ffb020;
}
*{box-sizing:border-box}
html,body{margin:0;min-height:100%;}
body{
  font:15px/1.55 system-ui,-apple-system,"Segoe UI",Roboto,Ubuntu,sans-serif;
  color:var(--text);
  background:
    radial-gradient(900px 480px at 12% -10%, #16314d 0%, transparent 65%),
    radial-gradient(700px 400px at 100% 0%, #1a2436 0%, transparent 60%),
    var(--bg);
}
header{
  max-width:980px;margin:0 auto;padding:28px 20px 8px;
  display:flex;align-items:center;justify-content:space-between;gap:16px;
}
.brand{display:flex;align-items:center;gap:14px}
.logo{
  width:46px;height:46px;border-radius:12px;display:grid;place-items:center;
  font:700 12px/1 ui-monospace,monospace;letter-spacing:.5px;color:#04121f;
  background:linear-gradient(145deg,#7cc0ff,#3b8ff0);
  box-shadow:0 6px 24px rgba(61,140,255,.35);
}
h1{font-size:22px;margin:0;letter-spacing:.2px}
h2{font-size:15px;margin:0;letter-spacing:.3px;text-transform:uppercase;color:#c7d5e6}
.muted{color:var(--muted)}
.small{font-size:12.5px}
.ver{font:12px ui-monospace,monospace;color:var(--muted);border:1px solid var(--line);padding:4px 9px;border-radius:999px}
main{max-width:980px;margin:0 auto;padding:10px 20px 60px;display:grid;gap:16px}
.card{background:linear-gradient(180deg,var(--card),var(--card2));border:1px solid var(--line);border-radius:16px;padding:16px 18px}
.card-head{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:12px}
.row{display:flex;gap:10px}
input{
  flex:1;min-width:0;background:#0a1119;border:1px solid var(--line);color:var(--text);
  border-radius:10px;padding:11px 13px;font:14px/1.4 ui-monospace,monospace;outline:none;
}
input:focus{border-color:var(--accent);box-shadow:0 0 0 3px rgba(77,157,255,.15)}
button{
  background:#1b2836;color:var(--text);border:1px solid var(--line);border-radius:10px;
  padding:10px 15px;font:600 14px/1.2 inherit;cursor:pointer;white-space:nowrap;
}
button:hover{background:#233447}
button.primary{background:linear-gradient(160deg,#5aa8ff,#2f7fe0);border-color:#2f7fe0;color:#03111f}
button.primary:hover{filter:brightness(1.07)}
button.ghost{background:transparent}
button.small{padding:6px 10px;font-size:12.5px}
button:disabled{opacity:.45;cursor:not-allowed}
.dot{width:10px;height:10px;border-radius:50%;background:#3a4656;box-shadow:0 0 0 4px rgba(255,255,255,.03)}
.dot.ok{background:var(--ok);box-shadow:0 0 12px rgba(61,220,132,.6)}
.dot.bad{background:var(--bad);box-shadow:0 0 12px rgba(255,95,95,.5)}
#jobs{display:grid;gap:10px;margin-top:14px}
.job{background:#0c141d;border:1px solid var(--line);border-radius:12px;padding:12px 14px}
.job-top{display:flex;align-items:center;justify-content:space-between;gap:10px}
.job-name{font-weight:600;overflow-wrap:anywhere}
.job-sub{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:4px;color:var(--muted);font-size:12.5px}
.pill{font:11px ui-monospace,monospace;border:1px solid var(--line);border-radius:999px;padding:2px 8px;color:#a9bdd4}
.pill.queued{color:#a9bdd4}
.pill.sending,.pill.transferring{color:var(--accent);border-color:rgba(77,157,255,.45)}
.pill.installing{color:var(--warn);border-color:rgba(255,176,32,.45)}
.pill.done{color:var(--ok);border-color:rgba(61,220,132,.45)}
.pill.error{color:var(--bad);border-color:rgba(255,95,95,.45)}
.pill.stopped{color:#c2a6ff;border-color:rgba(194,166,255,.4)}
.bar{height:7px;background:#0a1018;border:1px solid var(--line);border-radius:999px;overflow:hidden;margin:10px 0 6px}
.bar>i{display:block;height:100%;width:0;background:linear-gradient(90deg,#3b8ff0,#6fc0ff);border-radius:999px;transition:width .4s ease}
.job-foot{display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap}
.job-actions{display:flex;gap:8px;flex-wrap:wrap}
.msg{font-size:12.5px;color:var(--muted)}
.msg.error{color:#ff9c9c}
select{
  background:#0a1119;border:1px solid var(--line);color:var(--text);border-radius:10px;
  padding:9px 11px;font:13.5px/1.4 ui-monospace,monospace;outline:none;max-width:100%;
}
select:focus{border-color:var(--accent);box-shadow:0 0 0 3px rgba(77,157,255,.15)}
.net{display:flex;align-items:center;gap:10px;flex-wrap:wrap;margin-top:12px}
.net code{font:12px ui-monospace,monospace;color:#9fc3ea;background:#0a1119;border:1px solid var(--line);padding:5px 8px;border-radius:7px;overflow-wrap:anywhere}
pre#log{
  margin:0;max-height:240px;overflow:auto;background:#080d13;border:1px solid var(--line);
  border-radius:10px;padding:12px;font:12px/1.6 ui-monospace,monospace;color:#9fb3c9;white-space:pre-wrap;
}
.empty{padding:18px;text-align:center;color:var(--muted);border:1px dashed var(--line);border-radius:12px}
#toast{
  position:fixed;left:50%;bottom:22px;transform:translate(-50%,20px);opacity:0;pointer-events:none;
  background:#17222f;border:1px solid var(--line);color:var(--text);padding:10px 16px;border-radius:10px;
  font-size:13.5px;transition:.25s;max-width:min(90vw,560px);box-shadow:0 10px 30px rgba(0,0,0,.45);
}
#toast.show{opacity:1;transform:translate(-50%,0)}
@media (max-width:560px){
  header{padding-top:20px}
  .row{flex-direction:column}
  .net{align-items:stretch}
  select{width:100%}
  button{width:100%}
}
</style>
</head>
<body>
<header>
  <div class="brand">
    <div class="logo">PKG</div>
    <div>
      <h1>PS4Upload</h1>
      <div class="muted small">Pakete vom PC auf die PS4 schicken</div>
    </div>
  </div>
  <span class="ver">v{{VERSION}}</span>
</header>
<main>
  <section class="card">
    <div class="card-head"><h2>PS4</h2><span id="dot" class="dot"></span></div>
    <form id="connectForm" class="row">
      <input id="ipInput" placeholder="192.168.0.50" inputmode="decimal" spellcheck="false" autocomplete="off">
      <button class="primary" type="submit">Verbinden</button>
    </form>
    <p id="consoleText" class="muted small" style="margin:10px 0 0">Remote Package Installer auf der PS4 starten, dann die IP eintragen.</p>
    <div class="net">
      <span class="muted small">PC-Netzwerk</span>
      <select id="networkSelect" aria-label="Netzwerk fuer die PS4 waehlen"></select>
      <code id="serverUrl"></code>
    </div>
    <div class="net">
      <span class="muted small">Installation</span>
      <select id="modeSelect" aria-label="Installationsmodus waehlen">
        <option value="auto">Automatisch</option>
        <option value="rpi">Remote Package Installer (12800)</option>
        <option value="goldhen">GoldHEN Payload (BinLoader)</option>
      </select>
      <span id="modeHint" class="muted small"></span>
    </div>
    <p id="networkHint" class="muted small" style="margin:8px 0 0"></p>
  </section>

  <section class="card">
    <div class="card-head">
      <h2>Warteschlange</h2>
    </div>
    <form id="addForm" class="row">
      <input id="pathInput" placeholder="D:\Spiele\spiel.pkg oder Ordner" spellcheck="false" autocomplete="off">
      <button type="submit">Hinzufuegen</button>
    </form>
    <p class="muted small" style="margin:10px 0 0">
      Pfad auf diesem PC. Ordner werden nach .pkg durchsucht; geteilte Pakete (_0, _1, ...) werden zusammengefasst.
    </p>
    <div id="jobs"><div class="empty">Noch keine PKG hinzugefuegt.</div></div>
  </section>

  <section class="card">
    <div class="card-head">
      <h2>Protokoll</h2>
      <button id="clearLog" class="ghost small" type="button">Leeren</button>
    </div>
    <pre id="log"></pre>
  </section>
</main>
<div id="toast"></div>
<script>
const LABELS = {queued:"Wartet", sending:"Wird gesendet", transferring:"Uebertragung", installing:"Installation", done:"Fertig", error:"Fehler", stopped:"Gestoppt"};
const ACTIVE = ["sending","transferring","installing"];
let lastLog = "";
let toastTimer = null;

const $ = (sel) => document.querySelector(sel);

function fmtBytes(n){
  n = Number(n) || 0;
  if (n < 1024) return n + " B";
  const units = ["KB","MB","GB","TB"];
  let value = n / 1024, i = 0;
  while (value >= 1024 && i < units.length - 1){ value /= 1024; i++; }
  return value.toFixed(value >= 100 ? 0 : 1) + " " + units[i];
}
function fmtSpeed(bps){
  if (!bps || bps < 1) return "";
  return fmtBytes(bps) + "/s";
}
function fmtEta(sec){
  sec = Math.max(0, Math.round(sec));
  if (sec < 60) return sec + " s";
  const m = Math.floor(sec / 60), s = sec % 60;
  if (m < 60) return m + " min " + s + " s";
  const h = Math.floor(m / 60);
  return h + " h " + (m % 60) + " min";
}
function esc(text){
  return String(text).replace(/[&<>"']/g, (c) => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c]));
}
function toast(message){
  const el = $("#toast");
  el.textContent = message;
  el.classList.add("show");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.classList.remove("show"), 4200);
}

async function api(path, body){
  const options = body ? {method:"POST", headers:{"Content-Type":"application/json"}, body:JSON.stringify(body)} : undefined;
  const response = await fetch(path, options);
  let data = {};
  try { data = await response.json(); } catch (e) { data = {}; }
  if (!response.ok) throw new Error(data.error || ("HTTP " + response.status));
  return data;
}

function jobCard(job){
  const pct = job.state === "done" ? 100 : Math.max(0, Math.min(100, job.percent || 0));
  const label = LABELS[job.state] || job.state;
  let detail = fmtBytes(job.size);
  if (job.parts > 1) detail += " · " + job.parts + " Teile";
  if (job.title_id) detail += " · " + job.title_id;
  if (job.state === "transferring" || job.state === "installing" || job.state === "done"){
    const moved = fmtBytes(job.transferred) + " / " + fmtBytes(job.total || job.size);
    const speed = job.state === "transferring" && job.speed ? " · " + fmtSpeed(job.speed) : "";
    const eta = job.state === "transferring" && job.rest_sec ? " · noch " + fmtEta(job.rest_sec) : "";
    detail += " · " + moved + speed + eta;
  }
  const message = job.message || job.error || "";
  const msgClass = job.state === "error" ? "msg error" : "msg";

  let actions = "";
  if (job.state === "queued" || job.state === "error" || job.state === "stopped"){
    const text = job.state === "queued" ? "Installieren" : "Erneut versuchen";
    actions += '<button class="primary small" data-act="install" data-id="' + job.id + '">' + text + '</button>';
  }
  if ((job.state === "sending" && job.mode === "goldhen") || (ACTIVE.includes(job.state) && job.task_id)){
    actions += '<button class="small" data-act="stop" data-id="' + job.id + '">Stoppen</button>';
  }
  if (!ACTIVE.includes(job.state) || (job.mode === "goldhen" && job.state === "installing")){
    actions += '<button class="ghost small" data-act="remove" data-id="' + job.id + '">Entfernen</button>';
  }

  return '<div class="job">'
    + '<div class="job-top"><div class="job-name">' + esc(job.name) + '</div>'
    + '<span class="pill ' + job.state + '">' + label + '</span></div>'
    + '<div class="job-sub">' + esc(detail) + '</div>'
    + '<div class="bar"><i style="width:' + pct.toFixed(1) + '%"></i></div>'
    + '<div class="job-foot"><div class="' + msgClass + '">' + esc(message) + '</div>'
    + '<div class="job-actions">' + actions + '</div></div>'
    + '</div>';
}

function renderMode(state){
  const select = $("#modeSelect");
  if (document.activeElement !== select) select.value = state.mode || "auto";
  const hint = $("#modeHint");
  const payload = state.goldhen_payload || {};
  if (payload.exists === false && state.mode !== "rpi"){
    hint.textContent = "GoldHEN-Payload fehlt";
    hint.style.color = "var(--warn)";
  } else if (state.mode === "goldhen"){
    hint.textContent = "BinLoader Port " + ((state.console && state.console.goldhen_port) || 9090);
    hint.style.color = "";
  } else {
    hint.textContent = "";
  }
}

function renderNetwork(state){
  const net = state.network || {};
  const select = $("#networkSelect");
  const auto = "Automatisch" + (net.ip ? " (" + net.ip + ")" : "");
  const options = [{name:auto, ip:""}].concat(net.options || []);
  const signature = options.map((o) => o.ip + "|" + o.name).join(",");
  if (select.dataset.signature !== signature){
    select.innerHTML = options.map((o) => {
      const label = o.ip ? (o.name && o.name !== "Standard" ? o.name + " - " + o.ip : o.ip) : o.name;
      return '<option value="' + esc(o.ip) + '">' + esc(label) + '</option>';
    }).join("");
    select.dataset.signature = signature;
  }
  if (document.activeElement !== select) select.value = net.selected || "";
  $("#serverUrl").textContent = net.url || "";
  const hint = $("#networkHint");
  if (net.console_match === false){
    hint.textContent = "PS4 und PC sind nicht im selben /24-Netz - bitte das passende Netzwerk waehlen.";
    hint.style.color = "var(--warn)";
  } else {
    hint.textContent = "";
  }
}

function render(state){
  const dot = $("#dot");
  const text = $("#consoleText");
  const consoleInfo = state.console || {};
  dot.className = "dot " + (consoleInfo.reachable ? "ok" : "bad");
  if (!consoleInfo.ip){
    text.textContent = "Remote Package Installer auf der PS4 starten, dann die IP eintragen.";
  } else   if (consoleInfo.reachable){
    const parts = [];
    if (consoleInfo.rpi) parts.push("RPI " + consoleInfo.port);
    if (consoleInfo.goldhen) parts.push("GoldHEN " + consoleInfo.goldhen_port);
    text.textContent = "Verbunden mit " + consoleInfo.ip + " - " + parts.join(" + ") + " bereit.";
  } else {
    text.textContent = "Keine Verbindung zu " + consoleInfo.ip + " - RPI starten oder GoldHEN-BinLoader aktivieren.";
  }

  const ipInput = $("#ipInput");
  if (document.activeElement !== ipInput) ipInput.value = consoleInfo.ip || "";

  renderNetwork(state);
  renderMode(state);

  const jobs = state.jobs || [];
  $("#jobs").innerHTML = jobs.length
    ? jobs.map(jobCard).join("")
    : '<div class="empty">Noch keine PKG hinzugefuegt.</div>';

  const log = (state.logs || []).join("\n");
  if (log !== lastLog){
    lastLog = log;
    const pre = $("#log");
    pre.textContent = log;
    pre.scrollTop = pre.scrollHeight;
  }
}

async function loop(){
  try {
    render(await api("/api/state"));
  } catch (error) {
    $("#dot").className = "dot bad";
    $("#consoleText").textContent = "Server nicht erreichbar.";
  }
  setTimeout(loop, 1000);
}

$("#connectForm").addEventListener("submit", async (event) => {
  event.preventDefault();
  try {
    await api("/api/ps4", {ip: $("#ipInput").value.trim()});
    toast("PS4-Adresse gespeichert");
  } catch (error){ toast(error.message); }
});

$("#addForm").addEventListener("submit", async (event) => {
  event.preventDefault();
  const input = $("#pathInput");
  try {
    const result = await api("/api/add", {path: input.value});
    input.value = "";
    toast(result.message || "Hinzugefuegt");
  } catch (error){ toast(error.message); }
});

$("#networkSelect").addEventListener("change", async (event) => {
  try {
    await api("/api/network", {ip: event.target.value});
    toast("Netzwerk gespeichert");
  } catch (error){ toast(error.message); }
});

$("#modeSelect").addEventListener("change", async (event) => {
  try {
    await api("/api/mode", {mode: event.target.value});
    toast("Installationsmodus gespeichert");
  } catch (error){ toast(error.message); }
});

$("#jobs").addEventListener("click", async (event) => {
  const button = event.target.closest("button[data-act]");
  if (!button) return;
  try {
    await api("/api/" + button.dataset.act, {id: Number(button.dataset.id)});
  } catch (error){ toast(error.message); }
});

$("#clearLog").addEventListener("click", () => {
  $("#log").textContent = "";
  lastLog = "";
});

loop();
</script>
</body>
</html>
"""

PAGE = PAGE.replace("{{VERSION}}", VERSION)


# --------------------------------------------------------------------------- main


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="ps4upload.py",
        description="PKG-Dateien vom PC an eine jailbreakte PS4 senden und installieren.",
    )
    parser.add_argument("files", nargs="*", help="PKG-Dateien oder Ordner")
    parser.add_argument("--port", type=int, default=9898, help="HTTP-Port fuer die PS4 (Standard: 9898)")
    parser.add_argument("--host", default="0.0.0.0", help="Bind-Adresse (Standard: 0.0.0.0)")
    parser.add_argument("--host-ip", default="", help="IP dieses PCs, wie die PS4 ihn sieht")
    parser.add_argument("--list-networks", action="store_true", help="Lokale IPv4-Netzwerke anzeigen und beenden")
    parser.add_argument("--console", default="", help="IP der PS4")
    parser.add_argument("--console-port", type=int, default=RPI_PORT, help="RPI-Port der PS4 (Standard: 12800)")
    parser.add_argument(
        "--mode", choices=("auto", "rpi", "goldhen"), default="auto",
        help="Installationsweg: auto, rpi oder goldhen (Standard: auto)",
    )
    parser.add_argument("--goldhen-port", type=int, default=GOLDHEN_PORT, help="GoldHEN-BinLoader-Port (Standard: 9090)")
    parser.add_argument("--goldhen-payload", default="", help="Pfad zu einer eigenen GoldHEN-Installer-Payload (.bin)")
    parser.add_argument("--no-browser", action="store_true", help="Browser nicht automatisch oeffnen")
    args = parser.parse_args(argv)

    if args.list_networks:
        for option in discover_networks(lan_ip()):
            print(f"{option['ip']:15} /{option['prefix']:<2} {option['name']}")
        return 0

    app = App(
        host_ip=args.host_ip,
        port=args.port,
        console_ip=args.console.strip(),
        console_port=args.console_port,
        mode=args.mode,
        goldhen_port=args.goldhen_port,
        goldhen_payload=args.goldhen_payload,
    )

    try:
        httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    except OSError as exc:
        print(f"Port {args.port} kann nicht belegt werden: {exc}")
        print("Tipp: anderen Port mit --port waehlen.")
        return 1
    httpd.daemon_threads = True
    httpd.app = app  # type: ignore[attr-defined]

    for raw in args.files:
        try:
            added, _ = app.add_path(raw)
            print(f"{len(added)} Auftrag/Auftraege aus {raw}")
        except ValueError as exc:
            print(f"Fehler: {exc}")

    print()
    print(f"{APP_NAME} {VERSION}")
    print(f"Browser:  http://127.0.0.1:{args.port}")
    print(f"Fuer PS4: http://{app.effective_ip()}:{args.port}")
    print("Die PS4 muss diese Adresse im LAN erreichen koennen (Firewall!).")
    print("Web-UI/API nur lokal; im LAN sind nur /pkg/...-Downloads erlaubt.")
    print(f"Modus:    {app.mode} (GoldHEN-Payload: {'ok' if app.goldhen_payload.is_file() else 'fehlt'})")
    if app.console_ip:
        print(f"PS4:      {app.console_ip}:{app.console_port}")
    else:
        print("PS4:      IP im Browser eintragen")
    print("Beenden mit Strg+C")
    print()

    if not args.no_browser:
        threading.Timer(0.8, lambda: webbrowser.open(f"http://127.0.0.1:{args.port}")).start()

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nBeende ...")
    finally:
        httpd.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
