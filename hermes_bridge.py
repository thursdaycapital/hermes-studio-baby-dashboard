#!/usr/bin/env python3
"""Hermes MCP and CLI bridge for the Quote/0 baby dashboard."""

from __future__ import annotations

import argparse
import datetime as dt
import fcntl
import glob
import http.client
import ipaddress
import json
import os
import select
import socket
import sqlite3
import struct
import sys
import termios
import time
import tty
from pathlib import Path
from typing import Any

SERVER_INFO = {"name": "quote0-baby", "version": "1.2.1"}
DISCOVERY_PORT = 4210
TOKEN_PATH = Path(__file__).with_name(".quote0-token")
DEFAULT_FEED_INTERVAL_MINUTES = 180
KNOWN_DEVICE_HOSTS = [
    "192.168.1.100",
]
DB_PATH = Path(
    os.environ.get(
        "QUOTE0_BABY_DB",
        str(Path(__file__).with_name("baby_history.sqlite3")),
    )
)
_wifi_source: str | None = None


def device_token() -> str:
    """Read the private device token without committing it to source control."""
    token = os.environ.get("QUOTE0_TOKEN", "").strip()
    if not token and TOKEN_PATH.exists():
        token = TOKEN_PATH.read_text(encoding="utf-8").strip()
    if not token:
        raise RuntimeError(
            "请设置 QUOTE0_TOKEN，或在项目目录创建 .quote0-token"
        )
    return token

TOOLS = [
    {
        "name": "baby_log_feed",
        "description": "记录新生儿一次喂奶并刷新桌面墨水屏。未给时间时使用本机当前时间。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "amount_ml": {"type": "integer", "minimum": 0, "maximum": 999},
                "time": {"type": "string", "description": "24小时制 HH:MM，可省略"},
            },
            "required": ["amount_ml"],
        },
    },
    {
        "name": "baby_log_diaper",
        "description": "记录新生儿换尿布并刷新墨水屏。kind 使用 W（尿）、D（便）或 WD（都有）。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "kind": {"type": "string", "enum": ["W", "D", "WD"]},
                "time": {"type": "string", "description": "24小时制 HH:MM，可省略"},
            },
            "required": ["kind"],
        },
    },
    {
        "name": "baby_log_sleep",
        "description": "记录宝宝开始或结束睡眠并刷新墨水屏。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "state": {"type": "string", "enum": ["ON", "OFF"]},
                "time": {"type": "string", "description": "24小时制 HH:MM，可省略"},
            },
            "required": ["state"],
        },
    },
    {
        "name": "baby_set_next",
        "description": "设置下一项宝宝提醒并刷新墨水屏，例如下次喂奶或换尿布。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "time": {"type": "string", "description": "24小时制 HH:MM"},
                "kind": {
                    "type": "string",
                    "enum": ["FEED", "SLEEP", "DIAPER", "MED"],
                },
            },
            "required": ["time", "kind"],
        },
    },
    {
        "name": "baby_set_day",
        "description": "设置宝宝出生后的天数并刷新墨水屏。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "day": {"type": "integer", "minimum": 0, "maximum": 999},
            },
            "required": ["day"],
        },
    },
    {
        "name": "baby_status",
        "description": "读取 Quote/0 墨水屏内保存的宝宝记录状态。",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "baby_today_summary",
        "description": "统计今天的喂奶总量和次数、尿布次数、便便次数及睡眠时长（设备端历史）。",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "baby_recent_history",
        "description": "读取设备端最近的新生儿照护记录（最多 32 条）。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "limit": {"type": "integer", "minimum": 1, "maximum": 32},
            },
        },
    },
    {
        "name": "baby_set_feed_interval",
        "description": "设置喂奶后自动计算下次喂奶提醒的间隔，单位为分钟；写入设备端。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "minutes": {"type": "integer", "minimum": 30, "maximum": 720},
            },
            "required": ["minutes"],
        },
    },
    {
        "name": "baby_set_vaccine_reminder",
        "description": "把指定日期和时间的打预防针提醒推送到宝宝墨水屏。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "date": {
                    "type": "string",
                    "description": "日期，格式 YYYY-MM-DD",
                },
                "time": {"type": "string", "description": "24小时制 HH:MM"},
            },
            "required": ["date", "time"],
        },
    },
    {
        "name": "baby_undo_last",
        "description": "撤销设备端最后一条误记录；不会猜测并覆盖墨水屏内容。",
        "inputSchema": {"type": "object", "properties": {}},
    },
]


def current_time() -> str:
    return dt.datetime.now().astimezone().strftime("%H:%M")


def validate_time(value: str) -> str:
    try:
        parsed = dt.datetime.strptime(value, "%H:%M")
    except ValueError as exc:
        raise ValueError("time 必须使用 24 小时制 HH:MM") from exc
    return parsed.strftime("%H:%M")


def validate_date(value: str) -> str:
    try:
        parsed = dt.datetime.strptime(value, "%Y-%m-%d")
    except ValueError as exc:
        raise ValueError("date 必须使用 YYYY-MM-DD，并且是有效日期") from exc
    return parsed.strftime("%Y-%m-%d")


def database() -> sqlite3.Connection:
    DB_PATH.parent.mkdir(parents=True, exist_ok=True)
    connection = sqlite3.connect(DB_PATH)
    connection.row_factory = sqlite3.Row
    connection.executescript(
        """
        CREATE TABLE IF NOT EXISTS events (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            occurred_at TEXT NOT NULL,
            kind TEXT NOT NULL CHECK(kind IN ('FEED','DIAPER','SLEEP')),
            amount_ml INTEGER,
            value TEXT,
            created_at TEXT NOT NULL,
            UNIQUE(occurred_at, kind, amount_ml, value)
        );
        CREATE INDEX IF NOT EXISTS events_occurred_at
            ON events(occurred_at);
        CREATE TABLE IF NOT EXISTS settings (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );
        """
    )
    return connection


def event_datetime(when: str) -> dt.datetime:
    now = dt.datetime.now().astimezone()
    hour, minute = map(int, validate_time(when).split(":"))
    return now.replace(hour=hour, minute=minute, second=0, microsecond=0)


def add_event(kind: str, when: str, amount_ml: int | None = None,
              value: str | None = None) -> None:
    occurred = event_datetime(when).isoformat(timespec="minutes")
    created = dt.datetime.now().astimezone().isoformat(timespec="seconds")
    with database() as connection:
        connection.execute(
            """
            INSERT INTO events
                (occurred_at, kind, amount_ml, value, created_at)
            SELECT ?, ?, ?, ?, ?
            WHERE NOT EXISTS (
                SELECT 1 FROM events
                WHERE occurred_at = ? AND kind = ?
                  AND amount_ml IS ? AND value IS ?
            )
            """,
            (
                occurred, kind, amount_ml, value, created,
                occurred, kind, amount_ml, value,
            ),
        )


def setting_int(key: str, default: int) -> int:
    with database() as connection:
        row = connection.execute(
            "SELECT value FROM settings WHERE key = ?", (key,)
        ).fetchone()
    if not row:
        return default
    try:
        return int(row["value"])
    except (TypeError, ValueError):
        return default


def set_setting(key: str, value: int) -> None:
    with database() as connection:
        connection.execute(
            """
            INSERT INTO settings(key, value) VALUES (?, ?)
            ON CONFLICT(key) DO UPDATE SET value = excluded.value
            """,
            (key, str(value)),
        )


def format_duration(minutes: int) -> str:
    hours, remainder = divmod(max(0, minutes), 60)
    return f"{hours}小时{remainder}分" if hours else f"{remainder}分钟"


def today_summary() -> str:
    now = dt.datetime.now().astimezone()
    start = now.replace(hour=0, minute=0, second=0, microsecond=0)
    end = start + dt.timedelta(days=1)
    with database() as connection:
        rows = connection.execute(
            """
            SELECT * FROM events
            WHERE occurred_at >= ? AND occurred_at < ?
            ORDER BY occurred_at, id
            """,
            (
                start.isoformat(timespec="minutes"),
                end.isoformat(timespec="minutes"),
            ),
        ).fetchall()
        prior_sleep = connection.execute(
            """
            SELECT * FROM events
            WHERE kind = 'SLEEP' AND occurred_at < ?
            ORDER BY occurred_at DESC, id DESC LIMIT 1
            """,
            (start.isoformat(timespec="minutes"),),
        ).fetchone()

    feed_rows = [row for row in rows if row["kind"] == "FEED"]
    diaper_rows = [row for row in rows if row["kind"] == "DIAPER"]
    total_ml = sum(int(row["amount_ml"] or 0) for row in feed_rows)
    poop_count = sum(row["value"] in {"D", "WD"} for row in diaper_rows)

    sleep_start = start if prior_sleep and prior_sleep["value"] == "ON" else None
    sleep_minutes = 0
    for row in (row for row in rows if row["kind"] == "SLEEP"):
        event_at = dt.datetime.fromisoformat(row["occurred_at"])
        if row["value"] == "ON":
            sleep_start = event_at
        elif sleep_start is not None:
            sleep_minutes += int(
                (min(event_at, now, end) - max(sleep_start, start))
                .total_seconds() // 60
            )
            sleep_start = None
    if sleep_start is not None:
        sleep_minutes += int(
            (min(now, end) - max(sleep_start, start)).total_seconds() // 60
        )

    return (
        f"今日统计：喂奶 {len(feed_rows)} 次，共 {total_ml}ML；"
        f"尿布 {len(diaper_rows)} 次，其中便便 {poop_count} 次；"
        f"睡眠 {format_duration(sleep_minutes)}。"
    )


def recent_history(limit: int) -> str:
    with database() as connection:
        rows = connection.execute(
            "SELECT * FROM events ORDER BY occurred_at DESC, id DESC LIMIT ?",
            (limit,),
        ).fetchall()
    if not rows:
        return "还没有本地历史记录。"
    labels = {"FEED": "喂奶", "DIAPER": "尿布", "SLEEP": "睡眠"}
    details = {"W": "尿", "D": "便", "WD": "尿和便", "ON": "开始", "OFF": "结束"}
    output = []
    for row in rows:
        occurred = dt.datetime.fromisoformat(row["occurred_at"])
        detail = (
            f"{row['amount_ml']}ML" if row["kind"] == "FEED"
            else details.get(row["value"], row["value"] or "")
        )
        output.append(
            f"{occurred:%m-%d %H:%M} {labels[row['kind']]} {detail}".rstrip()
        )
    return "\n".join(output)


def undo_last() -> str:
    with database() as connection:
        row = connection.execute(
            "SELECT * FROM events ORDER BY id DESC LIMIT 1"
        ).fetchone()
        if not row:
            return "没有可撤销的本地记录。"
        connection.execute("DELETE FROM events WHERE id = ?", (row["id"],))
    return f"已撤销：{recent_history_line(row)}"


def recent_history_line(row: sqlite3.Row) -> str:
    occurred = dt.datetime.fromisoformat(row["occurred_at"])
    if row["kind"] == "FEED":
        detail = f"{row['amount_ml']}ML"
    else:
        detail = {"W": "尿", "D": "便", "WD": "尿和便",
                  "ON": "开始睡眠", "OFF": "结束睡眠"}.get(
            row["value"], row["value"] or ""
        )
    return f"{occurred:%m-%d %H:%M} {detail}"


def find_port() -> str:
    explicit = os.environ.get("QUOTE0_PORT")
    if explicit:
        return explicit
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        raise RuntimeError("未发现 Quote/0 USB 设备；请连接数据线")
    if len(ports) > 1:
        preferred = [port for port in ports if port.endswith("3101")]
        if preferred:
            return preferred[0]
    return ports[0]


def local_ipv4_addresses() -> list[str]:
    addresses: list[str] = []
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        for _, name in socket.if_nameindex():
            try:
                packed = struct.pack("256s", name.encode("utf-8"))
                result = fcntl.ioctl(probe.fileno(), 0xC0206921, packed)
                value = socket.inet_ntoa(result[20:24])
                address = ipaddress.ip_address(value)
                if address.is_private and not address.is_loopback:
                    addresses.append(value)
            except OSError:
                continue
    finally:
        probe.close()
    return addresses


def discover_wifi(timeout: float = 0.8) -> str | None:
    global _wifi_source
    cached = os.environ.get("QUOTE0_HOST")
    if cached:
        _wifi_source = os.environ.get("QUOTE0_SOURCE")
        return cached

    sockets: list[socket.socket] = []
    try:
        for source in local_ipv4_addresses():
            try:
                sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
                sock.bind((source, 0))
                broadcast = str(
                    ipaddress.ip_network(f"{source}/24", strict=False)
                    .broadcast_address
                )
                sock.sendto(b"QUOTE0_DISCOVER", (broadcast, DISCOVERY_PORT))
                sockets.append(sock)
            except OSError:
                sock.close()
        readable, _, _ = select.select(sockets, [], [], timeout)
        for sock in readable:
            payload, address = sock.recvfrom(128)
            if payload.strip() == b"QUOTE0_BABY":
                _wifi_source = sock.getsockname()[0]
                return address[0]
    except OSError:
        return None
    finally:
        for sock in sockets:
            sock.close()
    # UDP 广播未发现时，回退到已知 IP 直连（避免偶发广播丢失导致工具掉线）
    for fallback in KNOWN_DEVICE_HOSTS:
        try:
            probe = socket.create_connection((fallback, 80), timeout=0.4)
            probe.close()
            _wifi_source = None
            return fallback
        except OSError:
            continue
    return None


def transact_wifi(command: str) -> str | None:
    host = discover_wifi()
    if not host:
        return None
    token = device_token()
    connection = http.client.HTTPConnection(
        host, 80, timeout=15,
        source_address=(_wifi_source, 0) if _wifi_source else None,
    )
    try:
        connection.request(
            "POST", "/api/command",
            body=(command.strip() + "\n").encode("ascii"),
            headers={
                "Content-Type": "text/plain",
                "X-Quote0-Token": token,
            },
        )
        response = connection.getresponse()
        payload = json.loads(response.read().decode("utf-8"))
        value = str(payload.get("response") or "")
        if value:
            return value
    except (OSError, ValueError, json.JSONDecodeError):
        return None
    finally:
        connection.close()
    return None


def transact_usb(command: str, timeout: float = 14.0) -> str:
    port = find_port()
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        tty.setraw(fd)
        attrs = termios.tcgetattr(fd)
        attrs[4] = termios.B115200
        attrs[5] = termios.B115200
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        termios.tcflush(fd, termios.TCIOFLUSH)
        os.write(fd, (command.strip() + "\n").encode("ascii"))

        deadline = time.monotonic() + timeout
        received = bytearray()
        while time.monotonic() < deadline:
            readable, _, _ = select.select([fd], [], [], 0.25)
            if not readable:
                continue
            try:
                chunk = os.read(fd, 4096)
            except BlockingIOError:
                continue
            if not chunk:
                continue
            received.extend(chunk)
            text = received.decode("utf-8", errors="replace")
            lines = [line.strip() for line in text.splitlines()]
            if text and not text.endswith(("\n", "\r")):
                lines = lines[:-1]
            terminal = [
                line for line in lines
                if line.startswith(("OK ", "ERR ", "STATE "))
            ]
            if terminal:
                return terminal[-1]
        raise TimeoutError(f"设备在 {timeout:.0f} 秒内没有响应")
    finally:
        os.close(fd)


def transact(command: str, timeout: float = 14.0) -> str:
    wifi_response = transact_wifi(command)
    if wifi_response is not None:
        return wifi_response
    return transact_usb(command, timeout)


def command_for_tool(name: str, arguments: dict[str, Any]) -> str:
    when = validate_time(str(arguments.get("time") or current_time()))
    if name == "baby_log_feed":
        amount = int(arguments["amount_ml"])
        if not 0 <= amount <= 999:
            raise ValueError("amount_ml 必须在 0–999 之间")
        return f"FEED {when} {amount}"
    if name == "baby_log_diaper":
        kind = str(arguments["kind"]).upper()
        if kind not in {"W", "D", "WD"}:
            raise ValueError("kind 必须是 W、D 或 WD")
        return f"DIAPER {when} {kind}"
    if name == "baby_log_sleep":
        state = str(arguments["state"]).upper()
        if state not in {"ON", "OFF"}:
            raise ValueError("state 必须是 ON 或 OFF")
        return f"SLEEP {when} {state}"
    if name == "baby_set_next":
        kind = str(arguments["kind"]).upper()
        if kind not in {"FEED", "SLEEP", "DIAPER", "MED"}:
            raise ValueError("不支持的提醒类型")
        return f"NEXT {when} {kind}"
    if name == "baby_set_day":
        day = int(arguments["day"])
        if not 0 <= day <= 999:
            raise ValueError("day 必须在 0–999 之间")
        return f"DAY {day}"
    if name == "baby_set_vaccine_reminder":
        date = validate_date(str(arguments["date"]))
        return f"REMIND {date[5:]} {when} SHOT"
    if name == "baby_status":
        return "STATUS"
    raise ValueError(f"未知工具：{name}")


def call_tool(name: str, arguments: dict[str, Any]) -> dict[str, Any]:
    if name == "baby_today_summary":
        response = transact("SUMMARY")
        ok = bool(response) and not response.startswith("ERR")
        return {
            "content": [{"type": "text", "text": response or "设备无响应"}],
            "isError": not ok,
        }
    if name == "baby_recent_history":
        limit = int(arguments.get("limit", 10))
        if not 1 <= limit <= 32:
            raise ValueError("limit 必须在 1–32 之间")
        response = transact(f"HIST {limit}")
        ok = bool(response) and not response.startswith("ERR")
        return {
            "content": [{"type": "text", "text": response or "设备无响应"}],
            "isError": not ok,
        }
    if name == "baby_set_feed_interval":
        minutes = int(arguments["minutes"])
        if not 30 <= minutes <= 720:
            raise ValueError("minutes 必须在 30–720 之间")
        response = transact(f"INTERVAL {minutes}")
        ok = bool(response) and not response.startswith("ERR")
        if ok:
            set_setting("feed_interval_minutes", minutes)
        return {
            "content": [{
                "type": "text",
                "text": response or f"喂奶间隔已设为 {minutes} 分钟",
            }],
            "isError": not ok,
        }
    if name == "baby_undo_last":
        response = transact("UNDO")
        ok = bool(response) and not response.startswith("ERR")
        return {
            "content": [{"type": "text", "text": response or "设备无响应"}],
            "isError": not ok,
        }

    command = command_for_tool(name, arguments)
    response = transact(command)
    ok = response.startswith(("OK ", "STATE "))
    if not ok:
        return {
            "content": [{"type": "text", "text": response}],
            "isError": True,
        }

    when = validate_time(str(arguments.get("time") or current_time()))
    extra = ""
    if name == "baby_log_feed":
        amount = int(arguments["amount_ml"])
        add_event("FEED", when, amount_ml=amount)
        # 设备端固件已自动计算下次喂奶时间（INTERVAL），无需桥接再发 NEXT
        interval = setting_int(
            "feed_interval_minutes", DEFAULT_FEED_INTERVAL_MINUTES
        )
        extra = (
            f"\n已保存到本地历史；下次喂奶由设备按"
            f"{format_duration(interval)}间隔自动计算。"
        )
    elif name == "baby_log_diaper":
        add_event("DIAPER", when, value=str(arguments["kind"]).upper())
        extra = "\n已保存到本地历史。"
    elif name == "baby_log_sleep":
        add_event("SLEEP", when, value=str(arguments["state"]).upper())
        extra = "\n已保存到本地历史。"

    return {
        "content": [{"type": "text", "text": response + extra}],
        "isError": False,
    }


def write_message(payload: dict[str, Any]) -> None:
    sys.stdout.write(json.dumps(payload, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def run_mcp() -> None:
    for raw_line in sys.stdin:
        request: dict[str, Any] = {}
        try:
            request = json.loads(raw_line)
            request_id = request.get("id")
            method = request.get("method")
            if method == "initialize":
                result = {
                    "protocolVersion": "2024-11-05",
                    "capabilities": {"tools": {}},
                    "serverInfo": SERVER_INFO,
                }
            elif method == "ping":
                result = {}
            elif method == "tools/list":
                result = {"tools": TOOLS}
            elif method == "tools/call":
                params = request.get("params") or {}
                result = call_tool(
                    str(params.get("name")),
                    dict(params.get("arguments") or {}),
                )
            elif method and method.startswith("notifications/"):
                continue
            else:
                raise ValueError(f"不支持的 MCP 方法：{method}")
            if request_id is not None:
                write_message({"jsonrpc": "2.0", "id": request_id, "result": result})
        except Exception as exc:
            if request.get("id") is not None:
                write_message({
                    "jsonrpc": "2.0",
                    "id": request.get("id"),
                    "error": {"code": -32000, "message": str(exc)},
                })


def flash_over_wifi(bin_path: str, timeout: float = 120.0) -> str:
    """通过 WiFi 推送固件到 /api/ota 并触发重启。"""
    host = discover_wifi()
    if not host:
        return "ERR 无法发现设备（WiFi）"
    token = device_token()
    with open(bin_path, "rb") as handle:
        payload = handle.read()
    if len(payload) > 0xF00000:
        return "ERR 固件过大"
    connection = http.client.HTTPConnection(
        host, 80, timeout=timeout,
        source_address=(_wifi_source, 0) if _wifi_source else None,
    )
    try:
        connection.request(
            "POST", "/api/ota", body=payload,
            headers={
                "Content-Type": "application/octet-stream",
                "X-Quote0-Token": token,
            },
        )
        response = connection.getresponse()
        body = response.read().decode("utf-8", errors="replace")
        return f"HTTP {response.status}: {body}"
    except (OSError, ValueError) as exc:
        return f"ERR {exc}"
    finally:
        connection.close()


def run_cli() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", help="STATUS 或设备原生命令，或 FLASH <bin路径>")
    parser.add_argument("arguments", nargs="*")
    args = parser.parse_args()
    if args.command.upper() == "FLASH" and args.arguments:
        print(flash_over_wifi(args.arguments[0]))
        return
    print(transact(" ".join([args.command, *args.arguments])))


if __name__ == "__main__":
    if "--mcp" in sys.argv:
        run_mcp()
    else:
        run_cli()
