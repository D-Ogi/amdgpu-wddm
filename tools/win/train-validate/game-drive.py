"""Run a game session and drive its menu to the measured scene, for a game arm whose benchmark sits in a menu.

The b29 Rise of the Tomb Raider arm (native-caps548) started the game and then waited eleven minutes in the main
menu at 84 C, because nothing selected START BENCHMARK: the session script launches the game and opens the
control channel, and the menu has always been an operator's action. This wrapper starts the session command
as its child and, beside it, does what the operator did:

1. waits until the game runs (`game-control.py <attempt> peek`, one call every `--peek-every` s);
2. reads the screen (`shot:0.5;ocr`, one call every `--every` s, the owner's 10-15 s rule) until the menu item
   shows, points at it, clicks, and holds Enter when the click alone did not leave the menu;
3. checks itself: a shot and a read `--check-after` s later must show the scene, not the menu. It tries once
   more, then fails the arm with `menu not left`;
4. reads the screen on until the result line shows, prints it, confirms the dialog and ends the session.

The score stays the operator's reading in the summary; the line this prints is where to read it from. Exit code:
the session command's own, or 5 when the drive failed (the session is then ended through the channel first).

Usage: game-drive.py --profile rottr --attempt native-caps548 --control <game-control.py> -- <session command>
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

# What the driver looks for, per game profile. The texts are what the lab's OCR read on these screens
# (native-caps270 ocr-002 for the menu, native-caps539 ocr-016 for the result dialog).
DRIVES = {
    "rottr": {
        "menu": "START BENCHMARK",
        # The menu is up while any of these items shows. One alone could be a tip on a loading screen.
        "menu_items": ("START BENCHMARK", "OPTIONS", "QUIT GAME"),
        "result": "Overall score",
        # The profile's apis.d3d12.world_after_confirm_s: the scenes start 46-56 s after the confirm.
        "world_note": "note:world+60",
        "confirm": "hold:1C:300",
    },
}
FAILED = 5


class Channel:
    """One game-control.py call per question, as the operator makes them."""

    def __init__(self, control: list[str], attempt: str, say=print):
        self.control, self.attempt, self.say = control, attempt, say

    def call(self, actions: str, timeout: int = 90) -> str:
        try:
            done = subprocess.run(self.control + [self.attempt, actions], capture_output=True, text=True,
                                  encoding="utf-8", errors="replace", timeout=timeout)
        except subprocess.TimeoutExpired:
            return ""
        return done.stdout + done.stderr

    def running(self) -> bool:
        return "game running: True" in self.call("peek")

    def look(self) -> tuple[list[dict], str]:
        """(the OCR lines of the screen, the shot's path), from one call that takes both."""
        out = self.call("shot:0.5;ocr")
        shot = next((m.group(1) for m in re.finditer(r"shot saved (\S+)", out)), "")
        match = re.search(r"ocr saved (.+?) \(\d+ lines\)", out)
        if not match:
            return [], shot
        try:
            data = json.loads(Path(match.group(1)).read_text(encoding="utf-8"), strict=False)
        except (OSError, ValueError):
            return [], shot
        return list(data.get("lines", [])), shot


def find(lines: list[dict], text: str) -> dict | None:
    want = text.upper()
    return next((line for line in lines if want in str(line.get("t", "")).upper()), None)


def in_menu(lines: list[dict], drive: dict) -> bool:
    return sum(1 for item in drive["menu_items"] if find(lines, item)) >= 2


class Driver:
    def __init__(self, drive: dict, channel: Channel, alive, every: float, peek_every: float,
                 check_after: float, wait_game: float, wait_menu: float, wait_result: float,
                 sleep=time.sleep, clock=time.monotonic, say=print):
        self.drive, self.channel, self.alive = drive, channel, alive
        self.every, self.peek_every, self.check_after = every, peek_every, check_after
        self.wait_game, self.wait_menu, self.wait_result = wait_game, wait_menu, wait_result
        self.sleep, self.clock, self.say = sleep, clock, say
        self.failed = ""

    def fail(self, why: str) -> None:
        self.failed = why
        self.say(f"[drive] FAILED {why}")
        if self.alive():
            self.channel.call("quit")
            self.say("[drive] the session was ended through the control channel")

    def until(self, seconds: float, every: float, question):
        """Ask until the answer is not None, the game ends or the time is up. None on the last two."""
        deadline = self.clock() + seconds
        while self.alive():
            answer = question()
            if answer is not None:
                return answer
            if self.clock() >= deadline:
                return None
            self.sleep(every)
        return None

    def start(self, item: dict) -> None:
        x0, y0, x1, y1 = item["b"]
        x, y = round((x0 + x1) / 2, 3), round((y0 + y1) / 2, 3)
        self.channel.call(f"point:{x}:{y};click:left;wait:2000")
        lines, _ = self.channel.look()
        if in_menu(lines, self.drive):
            # The click selected the item but did not start it: a held Enter does (rottr4, 2026-10-01).
            self.channel.call(self.drive["confirm"])
        self.channel.call(self.drive["world_note"])
        self.say(f"[drive] {self.drive['menu']} at {x},{y}: clicked")

    def left_menu(self) -> bool:
        self.sleep(self.check_after)
        lines, shot = self.channel.look()
        left = not in_menu(lines, self.drive)
        self.say(f"[drive] self-check {shot or 'no shot'}: {'the menu is gone' if left else 'still the menu'}")
        return left

    def run(self) -> None:
        if self.until(self.wait_game, self.peek_every, lambda: True if self.channel.running() else None) is None:
            if self.alive():
                self.fail("the game did not start")
            return
        self.say("[drive] the game runs; reading the screen until the menu shows")

        def menu():
            lines, shot = self.channel.look()
            item = find(lines, self.drive["menu"]) if in_menu(lines, self.drive) else None
            self.say(f"[drive] {shot or 'no shot'}: {'menu' if item else 'not the menu yet'}")
            return item

        for attempt in (1, 2):
            item = self.until(self.wait_menu, self.every, menu)
            if item is None:
                if self.alive():
                    self.fail(f"the main menu never showed {self.drive['menu']}")
                return
            self.start(item)
            if self.left_menu():
                break
            if attempt == 2:
                self.fail("menu not left")
                return
            self.say("[drive] trying once more")

        def result():
            lines, shot = self.channel.look()
            found = find(lines, self.drive["result"])
            self.say(f"[drive] {shot or 'no shot'}: {'result' if found else 'running'}")
            return lines if found else None

        lines = self.until(self.wait_result, self.every, result)
        if lines is None:
            if self.alive():
                self.fail(f"no {self.drive['result']} line within {int(self.wait_result)} s of the start")
            return
        for line in lines:
            self.say(f"[drive] result: {line.get('t', '')}")
        self.channel.call(self.drive["confirm"])
        self.channel.call("quit")
        self.say("[drive] result confirmed, the session ended through the control channel")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--profile", required=True, choices=sorted(DRIVES))
    parser.add_argument("--attempt", required=True)
    parser.add_argument("--control", required=True, help="game-control.py, or a command line for it")
    parser.add_argument("--every", type=float, default=12.0)
    parser.add_argument("--peek-every", type=float, default=20.0)
    parser.add_argument("--check-after", type=float, default=20.0)
    parser.add_argument("--wait-game", type=float, default=600.0)
    parser.add_argument("--wait-menu", type=float, default=300.0)
    parser.add_argument("--wait-result", type=float, default=600.0)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("no session command after --")
    control = [sys.executable, args.control] if args.control.endswith(".py") else args.control.split()
    print(f"[drive] {args.profile} {args.attempt}: this step selects {DRIVES[args.profile]['menu']} itself once "
          f"the menu shows, checks that the menu is gone, and reads the result dialog; shots and reads land in "
          f"the attempt's control directory", flush=True)
    child = subprocess.Popen(command)
    lock = threading.Lock()

    def say(text: str) -> None:
        with lock:
            print(text, flush=True)

    driver = Driver(DRIVES[args.profile], Channel(control, args.attempt, say), lambda: child.poll() is None,
                    args.every, args.peek_every, args.check_after, args.wait_game, args.wait_menu,
                    args.wait_result, say=say)
    thread = threading.Thread(target=driver.run, daemon=True)
    thread.start()
    rc = child.wait()
    thread.join(timeout=120)
    if driver.failed:
        return FAILED
    return rc


if __name__ == "__main__":
    sys.exit(main())
