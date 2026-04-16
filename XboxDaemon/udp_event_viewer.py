#!/usr/bin/env python3

import argparse
import json
import socket
import sys


BUTTONS = [
    (1 << 0, "SYNC"),
    (1 << 2, "MENU"),
    (1 << 3, "VIEW"),
    (1 << 4, "A"),
    (1 << 5, "B"),
    (1 << 6, "X"),
    (1 << 7, "Y"),
    (1 << 8, "UP"),
    (1 << 9, "DOWN"),
    (1 << 10, "LEFT"),
    (1 << 11, "RIGHT"),
    (1 << 12, "LB"),
    (1 << 13, "RB"),
    (1 << 14, "LS"),
    (1 << 15, "RS"),
]


def decode_buttons(mask: int) -> str:
    names = [name for bit, name in BUTTONS if mask & bit]
    return "+".join(names) if names else "-"


def norm_axis(v: int) -> float:
    if v < 0:
        return max(-1.0, v / 32768.0)
    return min(1.0, v / 32767.0)


def parse_addr(value: str):
    if ":" not in value:
        return value, 7947
    host, port = value.rsplit(":", 1)
    return host, int(port)


def main() -> int:
    parser = argparse.ArgumentParser(description="Listen to xbox_daemon UDP events")
    parser.add_argument(
        "--listen", default="127.0.0.1:7947", help="host:port (default: 127.0.0.1:7947)"
    )
    parser.add_argument("--raw", action="store_true", help="print raw JSON lines")
    args = parser.parse_args()

    host, port = parse_addr(args.listen)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((host, port))
    print(f"[viewer] listening on {host}:{port}")

    while True:
        data, _ = sock.recvfrom(65535)
        text = data.decode("utf-8", "replace")

        for line in text.splitlines():
            line = line.strip()
            if not line:
                continue

            if args.raw:
                print(line)
                continue

            try:
                evt = json.loads(line)
            except json.JSONDecodeError:
                print(f"[viewer] invalid json: {line}")
                continue

            t = evt.get("type")
            if t == "connected":
                print(f"[slot {evt.get('slot')}] connected")
            elif t == "disconnected":
                print(f"[slot {evt.get('slot')}] disconnected")
            elif t == "guide":
                print(
                    f"[slot {evt.get('slot')}] guide={'ON' if evt.get('pressed') else 'OFF'}"
                )
            elif t == "hid_unavailable":
                print("[viewer] hid unavailable on this runtime")
            elif t == "input":
                buttons = int(evt.get("buttons", 0))
                lt = int(evt.get("lt", 0))
                rt = int(evt.get("rt", 0))
                lx = int(evt.get("lx", 0))
                ly = int(evt.get("ly", 0))
                rx = int(evt.get("rx", 0))
                ry = int(evt.get("ry", 0))
                print(
                    f"[slot {evt.get('slot')}] btn={decode_buttons(buttons):<24} "
                    f"LT={lt:4d} RT={rt:4d} "
                    f"LX={norm_axis(lx):+0.2f} LY={norm_axis(ly):+0.2f} "
                    f"RX={norm_axis(rx):+0.2f} RY={norm_axis(ry):+0.2f}"
                )
            else:
                print(f"[viewer] {line}")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\n[viewer] stopped")
        sys.exit(0)
