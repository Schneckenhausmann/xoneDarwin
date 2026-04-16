#!/usr/bin/env python3

import argparse
import curses
import json
import os
import socket
import time
from dataclasses import dataclass, field


BUTTON_BITS = [
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

WIZARD_STEPS = [
    "Press A",
    "Press B",
    "Press X",
    "Press Y",
    "Press LB",
    "Press RB",
    "Press MENU",
    "Press VIEW",
    "Press LT",
    "Press RT",
    "Press DPad UP",
    "Press DPad DOWN",
    "Press DPad LEFT",
    "Press DPad RIGHT",
    "Move LEFT stick UP",
    "Move LEFT stick DOWN",
    "Move LEFT stick LEFT",
    "Move LEFT stick RIGHT",
    "Move RIGHT stick UP",
    "Move RIGHT stick DOWN",
    "Move RIGHT stick LEFT",
    "Move RIGHT stick RIGHT",
]


def parse_addr(value: str):
    if ":" not in value:
        return value, 7947
    host, port = value.rsplit(":", 1)
    return host, int(port)


def decode_buttons(mask: int):
    return [name for bit, name in BUTTON_BITS if mask & bit]


def norm_axis(v: int):
    if v < 0:
        return max(-1.0, v / 32768.0)
    return min(1.0, v / 32767.0)


def normalize_trigger(v: int):
    v &= 0xFFFF
    if v <= 4095:
        return v
    return min(v, 65535 - v)


@dataclass
class InputState:
    buttons: int = 0
    lt: int = 0
    rt: int = 0
    lx: int = 0
    ly: int = 0
    rx: int = 0
    ry: int = 0
    connected: bool = False
    last_update: float = 0.0


@dataclass
class Wizard:
    active: bool = False
    slot: int = 0
    step: int = 0
    results: list = field(default_factory=list)
    last_state: InputState = field(default_factory=InputState)
    last_capture_ts: float = 0.0
    trigger_step: int = -1
    trigger_base_lt: int = 0
    trigger_base_rt: int = 0
    axis_step: int = -1
    axis_base_lx: int = 0
    axis_base_ly: int = 0
    axis_base_rx: int = 0
    axis_base_ry: int = 0


class App:
    def __init__(self, stdscr, host, port, log_path):
        self.stdscr = stdscr
        self.host = host
        self.port = port
        self.log_path = log_path
        self.states = {i: InputState() for i in range(8)}
        self.raw_events = []
        self.wizard = Wizard()
        self.status = "Ready"
        self.running = True

        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind((host, port))
        self.sock.setblocking(False)

    def auto_select_wizard_slot(self):
        if 0 <= self.wizard.slot < 8 and self.states[self.wizard.slot].connected:
            return self.wizard.slot

        best_slot = -1
        best_ts = 0.0
        for idx, state in self.states.items():
            if state.connected and state.last_update > best_ts:
                best_slot = idx
                best_ts = state.last_update

        if best_slot >= 0:
            self.wizard.slot = best_slot
        return self.wizard.slot

    def run(self):
        curses.curs_set(0)
        self.stdscr.nodelay(True)
        self.stdscr.timeout(50)

        while self.running:
            self.poll_udp()
            self.handle_keys()
            self.draw()

    def poll_udp(self):
        while True:
            try:
                data, _ = self.sock.recvfrom(65535)
            except BlockingIOError:
                return

            text = data.decode("utf-8", "replace")
            for line in text.splitlines():
                line = line.strip()
                if not line:
                    continue
                try:
                    evt = json.loads(line)
                except json.JSONDecodeError:
                    continue
                self.raw_events.append({"t": time.time(), "event": evt})
                self.apply_event(evt)

    def apply_event(self, evt):
        t = evt.get("type")
        slot = int(evt.get("slot", 0))
        if slot < 0 or slot >= 8:
            return

        s = self.states[slot]
        now = time.time()

        if t == "connected":
            s.connected = True
            s.last_update = now
            self.status = f"Slot {slot} connected"
            return

        if t == "disconnected":
            self.states[slot] = InputState()
            self.status = f"Slot {slot} disconnected"
            return

        if t == "input":
            prev = InputState(**s.__dict__)
            s.buttons = int(evt.get("buttons", s.buttons))
            s.lt = normalize_trigger(int(evt.get("lt", s.lt)))
            s.rt = normalize_trigger(int(evt.get("rt", s.rt)))
            s.lx = int(evt.get("lx", s.lx))
            s.ly = int(evt.get("ly", s.ly))
            s.rx = int(evt.get("rx", s.rx))
            s.ry = int(evt.get("ry", s.ry))
            s.connected = True
            s.last_update = now
            self.maybe_capture_wizard(prev, s)

    def maybe_capture_wizard(self, prev, cur):
        w = self.wizard
        if not w.active:
            return
        if w.slot < 0 or w.slot >= 8:
            return

        if cur is not self.states[w.slot]:
            return

        now = time.time()
        if now - w.last_capture_ts < 0.35:
            return

        step = WIZARD_STEPS[w.step]
        change = None

        if step in ("Press LT", "Press RT") and w.trigger_step != w.step:
            w.trigger_step = w.step
            w.trigger_base_lt = cur.lt
            w.trigger_base_rt = cur.rt

        stick_steps = {
            "Move LEFT stick UP",
            "Move LEFT stick DOWN",
            "Move LEFT stick LEFT",
            "Move LEFT stick RIGHT",
            "Move RIGHT stick UP",
            "Move RIGHT stick DOWN",
            "Move RIGHT stick LEFT",
            "Move RIGHT stick RIGHT",
        }
        if step in stick_steps and w.axis_step != w.step:
            w.axis_step = w.step
            w.axis_base_lx = cur.lx
            w.axis_base_ly = cur.ly
            w.axis_base_rx = cur.rx
            w.axis_base_ry = cur.ry

        def pressed_edge(bit):
            return (cur.buttons & bit) and not (prev.buttons & bit)

        def trigger_delta(base, value):
            return abs(value - base)

        def trigger_changed(base, value, prev_value):
            return trigger_delta(base, value) >= 60 or abs(value - prev_value) >= 60

        def stick_changed(base, value, prev_value):
            return abs(value - base) >= 1800 or abs(value - prev_value) >= 1800

        def axis_observed(pos_label, neg_label, delta):
            return pos_label if delta >= 0 else neg_label

        if step == "Press A" and pressed_edge(1 << 4):
            change = "button:A"
        elif step == "Press B" and pressed_edge(1 << 5):
            change = "button:B"
        elif step == "Press X" and pressed_edge(1 << 6):
            change = "button:X"
        elif step == "Press Y" and pressed_edge(1 << 7):
            change = "button:Y"
        elif step == "Press LB" and pressed_edge(1 << 12):
            change = "button:LB"
        elif step == "Press RB" and pressed_edge(1 << 13):
            change = "button:RB"
        elif step == "Press MENU" and pressed_edge(1 << 2):
            change = "button:MENU"
        elif step == "Press VIEW" and pressed_edge(1 << 3):
            change = "button:VIEW"
        elif step == "Press LT" and (
            trigger_changed(w.trigger_base_lt, cur.lt, prev.lt)
        ):
            change = "trigger:LT"
        elif step == "Press RT" and (
            trigger_changed(w.trigger_base_rt, cur.rt, prev.rt)
        ):
            change = "trigger:RT"
        elif step == "Press DPad UP" and pressed_edge(1 << 8):
            change = "dpad:UP"
        elif step == "Press DPad DOWN" and pressed_edge(1 << 9):
            change = "dpad:DOWN"
        elif step == "Press DPad LEFT" and pressed_edge(1 << 10):
            change = "dpad:LEFT"
        elif step == "Press DPad RIGHT" and pressed_edge(1 << 11):
            change = "dpad:RIGHT"
        elif step == "Move LEFT stick UP":
            dy = cur.ly - w.axis_base_ly
            if stick_changed(w.axis_base_ly, cur.ly, prev.ly):
                change = axis_observed("stick:LEFT_Y_POS", "stick:LEFT_Y_NEG", dy)
        elif step == "Move LEFT stick DOWN":
            dy = cur.ly - w.axis_base_ly
            if stick_changed(w.axis_base_ly, cur.ly, prev.ly):
                change = axis_observed("stick:LEFT_Y_POS", "stick:LEFT_Y_NEG", dy)
        elif step == "Move LEFT stick LEFT":
            dx = cur.lx - w.axis_base_lx
            if stick_changed(w.axis_base_lx, cur.lx, prev.lx):
                change = axis_observed("stick:LEFT_X_POS", "stick:LEFT_X_NEG", dx)
        elif step == "Move LEFT stick RIGHT":
            dx = cur.lx - w.axis_base_lx
            if stick_changed(w.axis_base_lx, cur.lx, prev.lx):
                change = axis_observed("stick:LEFT_X_POS", "stick:LEFT_X_NEG", dx)
        elif step == "Move RIGHT stick UP":
            dy = cur.ry - w.axis_base_ry
            if stick_changed(w.axis_base_ry, cur.ry, prev.ry):
                change = axis_observed("stick:RIGHT_Y_POS", "stick:RIGHT_Y_NEG", dy)
        elif step == "Move RIGHT stick DOWN":
            dy = cur.ry - w.axis_base_ry
            if stick_changed(w.axis_base_ry, cur.ry, prev.ry):
                change = axis_observed("stick:RIGHT_Y_POS", "stick:RIGHT_Y_NEG", dy)
        elif step == "Move RIGHT stick LEFT":
            dx = cur.rx - w.axis_base_rx
            if stick_changed(w.axis_base_rx, cur.rx, prev.rx):
                change = axis_observed("stick:RIGHT_X_POS", "stick:RIGHT_X_NEG", dx)
        elif step == "Move RIGHT stick RIGHT":
            dx = cur.rx - w.axis_base_rx
            if stick_changed(w.axis_base_rx, cur.rx, prev.rx):
                change = axis_observed("stick:RIGHT_X_POS", "stick:RIGHT_X_NEG", dx)

        if not change:
            return

        w.last_capture_ts = now
        w.results.append({"step": step, "observed": change})
        w.step += 1
        self.status = f"Wizard captured: {change}"
        if w.step >= len(WIZARD_STEPS):
            w.active = False
            self.status = "Wizard complete (press S to save log)"

    def handle_keys(self):
        ch = self.stdscr.getch()
        if ch == -1:
            return

        if ch in (ord("q"), ord("Q")):
            self.running = False
            return

        if ch in (ord("w"), ord("W")):
            slot = self.auto_select_wizard_slot()
            self.wizard = Wizard(active=True, slot=self.wizard.slot)
            self.status = f"Wizard started on slot {slot}"
            return

        if ch in (ord("n"), ord("N")):
            self.skip_wizard_step()
            return

        if ch in (ord("s"), ord("S")):
            self.save_log()
            return

        if ord("1") <= ch <= ord("8"):
            self.wizard.slot = ch - ord("1")
            self.status = f"Selected slot {self.wizard.slot} for wizard"

    def skip_wizard_step(self):
        w = self.wizard
        if not w.active:
            self.status = "Wizard not active"
            return
        if w.step >= len(WIZARD_STEPS):
            self.status = "Wizard already complete"
            return

        step = WIZARD_STEPS[w.step]
        w.results.append({"step": step, "observed": "SKIPPED"})
        w.step += 1
        w.last_capture_ts = time.time()
        self.status = f"Wizard skipped: {step}"
        if w.step >= len(WIZARD_STEPS):
            w.active = False
            self.status = "Wizard complete (press S to save log)"

    def save_log(self):
        os.makedirs(os.path.dirname(self.log_path), exist_ok=True)
        payload = {
            "saved_at": time.time(),
            "listen": f"{self.host}:{self.port}",
            "wizard": {
                "slot": self.wizard.slot,
                "active": self.wizard.active,
                "step": self.wizard.step,
                "results": self.wizard.results,
            },
            "events": self.raw_events[-5000:],
        }
        with open(self.log_path, "w", encoding="utf-8") as f:
            json.dump(payload, f, indent=2)
        self.status = f"Saved log: {self.log_path}"

    def draw_slot(self, row, idx, s: InputState):
        conn = "ON " if s.connected else "OFF"
        btn = "+".join(decode_buttons(s.buttons)) or "-"
        lx = norm_axis(s.lx)
        ly = norm_axis(s.ly)
        rx = norm_axis(s.rx)
        ry = norm_axis(s.ry)
        self.stdscr.addstr(row, 2, f"[{idx}] conn={conn} btn={btn[:44]}")
        self.stdscr.addstr(
            row + 1,
            4,
            f"LT={s.lt:4d} RT={s.rt:4d}  LX={lx:+0.2f} LY={ly:+0.2f}  RX={rx:+0.2f} RY={ry:+0.2f}",
        )

    def draw(self):
        self.stdscr.erase()
        self.stdscr.addstr(
            0, 2, "+--------------------------------------------------------------+"
        )
        self.stdscr.addstr(
            1,
            2,
            "|  Xbox Mapper TUI  :)  q=quit w=wizard n=skip s=save 1..8=slot |",
        )
        self.stdscr.addstr(2, 2, f"|  Listening: {self.host}:{self.port:<47}|")
        self.stdscr.addstr(
            3, 2, "+--------------------------------------------------------------+"
        )

        row = 5
        for i in range(4):
            self.draw_slot(row, i, self.states[i])
            row += 3

        row = 5
        for i in range(4, 8):
            self.draw_slot(row, i, self.states[i])
            row += 3

        wr = 18
        w = self.wizard
        status = "ACTIVE" if w.active else "idle"
        self.stdscr.addstr(
            wr, 2, f"Wizard: {status} slot={w.slot} step={w.step}/{len(WIZARD_STEPS)}"
        )
        if w.active and w.step < len(WIZARD_STEPS):
            self.stdscr.addstr(wr + 1, 4, f"Next: {WIZARD_STEPS[w.step]}")
        self.stdscr.addstr(wr + 2, 2, f"Status: {self.status[:90]}")

        self.stdscr.refresh()


def main():
    parser = argparse.ArgumentParser(
        description="Curses mapper/debugger for xbox_daemon UDP events"
    )
    parser.add_argument("--listen", default="127.0.0.1:7947", help="UDP bind host:port")
    parser.add_argument(
        "--log", default="./mapper_session.json", help="Path for saved mapping log"
    )
    args = parser.parse_args()

    host, port = parse_addr(args.listen)

    def runner(stdscr):
        app = App(stdscr, host, port, args.log)
        app.run()

    curses.wrapper(runner)


if __name__ == "__main__":
    main()
