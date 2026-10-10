"""Run a game session and drive its menu to the measured scene, for a game arm whose benchmark sits in a menu.

The b29 Rise of the Tomb Raider arm (native-caps548) started the game and then waited eleven minutes in the main
menu at 84 C, because nothing selected START BENCHMARK: the session script launches the game and opens the
control channel, and the menu has always been an operator's action. This wrapper starts the session command
as its child and, beside it, does what the operator did:

1. waits until the game runs (`game-control.py <attempt> peek`, one call every `--peek-every` s);
2. reads the screen (`shot:0.5;ocr`, one call every `--every` s, the owner's 10-15 s rule) until the menu item
   shows, moves the menu's highlight onto it with the arrow keys, reads the screen again to see the highlight
   there, and only then presses Enter;
3. checks itself: a shot and a read `--check-after` s later must show neither the menu nor a submenu (a BACK
   button: the wrong item opened). From a submenu it presses Esc. It tries once more, then fails the arm with
   `menu not left`;
4. reads the screen on until the result line shows, prints it, confirms the dialog and ends the session.

The arrow keys, not the mouse (b29 native-caps550): a click at the centre of the START BENCHMARK box that the OCR
found left the game's highlight on CREDITS, one item up, the Enter after it opened the credits, and the self-check
of that version took the credits for the benchmark because the menu was gone. The game draws its highlighted item
about 1.3 times wider, which the OCR boxes show, so the driver reads where the highlight is instead of assuming
where a click put it.

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
        # Down and Up arrows (extended scan codes 50 and 48), Esc, and the text of a submenu's way back.
        "down": "tapx:50",
        "up": "tapx:48",
        "back": "tap:01",
        "submenu": "BACK",
    },
}
FAILED = 5
# A menu item drawn this much wider per character than the column's median is the highlighted one (550: the
# highlighted MARKETPLACE and CREDITS read 1.39 and 1.27 times the median).
HIGHLIGHT_RATIO = 1.18


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


def menu_column(lines: list[dict], target: dict) -> list[dict]:
    """The menu's items, top to bottom: capital-letter lines whose left edge is the target item's."""
    left = target["b"][0]
    column = [line for line in lines if re.fullmatch(r"[A-Z][A-Z ]{2,}", str(line.get("t", "")).strip())
              and abs(line["b"][0] - left) <= 0.03]
    return sorted(column, key=lambda line: line["b"][1])


def per_char(line: dict) -> float:
    return (line["b"][2] - line["b"][0]) / len(line["t"].strip())


def highlighted(column: list[dict]) -> dict | None:
    """The item drawn wider than the others (the game's highlight), or None when no item stands out."""
    if len(column) < 3:
        return None
    widths = sorted(per_char(line) for line in column)
    best = max(column, key=per_char)
    return best if per_char(best) >= HIGHLIGHT_RATIO * widths[len(widths) // 2] else None


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

    def select(self, lines: list[dict]) -> bool:
        """Moves the highlight onto the menu item with the arrow keys. True once a read shows it there."""
        want = self.drive["menu"]
        for _ in range(4):
            target = find(lines, want)
            column = menu_column(lines, target) if target else []
            names = [line["t"].strip().upper() for line in column]
            if want not in names:
                self.say(f"[drive] {want} is not in the menu column of the read: {names}")
                return False
            lit = highlighted(column)
            if lit is not None and lit["t"].strip().upper() == want:
                self.say(f"[drive] the highlight is on {want}")
                return True
            if lit is None:
                keys = [self.drive["down"]]         # the first arrow key makes the highlight show
                where = "no highlight in the read"
            else:
                steps = names.index(want) - names.index(lit["t"].strip().upper())
                keys = [self.drive["down"] if steps > 0 else self.drive["up"]] * abs(steps)
                where = f"the highlight is on {lit['t'].strip()}"
            self.say(f"[drive] {where}: {len(keys)} x {keys[0]}")
            self.channel.call(";".join(f"{key};wait:300" for key in keys))
            lines, _ = self.channel.look()
            if not in_menu(lines, self.drive):
                self.say("[drive] the menu went away under the arrow keys")
                return False
        return False

    def start(self, lines: list[dict]) -> bool:
        if not self.select(lines):
            return False
        self.channel.call(self.drive["confirm"])
        self.channel.call(self.drive["world_note"])
        self.say(f"[drive] {self.drive['menu']}: Enter on the highlighted item")
        return True

    def left_menu(self) -> bool:
        self.sleep(self.check_after)
        lines, shot = self.channel.look()
        if find(lines, self.drive["submenu"]) and not in_menu(lines, self.drive):
            self.say(f"[drive] self-check {shot or 'no shot'}: a submenu ({self.drive['submenu']}), not the scene; "
                     f"Esc back to the menu")
            self.channel.call(self.drive["back"] + ";wait:2000")
            return False
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
            found = in_menu(lines, self.drive) and find(lines, self.drive["menu"]) is not None
            self.say(f"[drive] {shot or 'no shot'}: {'menu' if found else 'not the menu yet'}")
            return lines if found else None

        for attempt in (1, 2):
            lines = self.until(self.wait_menu, self.every, menu)
            if lines is None:
                if self.alive():
                    self.fail(f"the main menu never showed {self.drive['menu']}")
                return
            if not self.start(lines):
                if self.alive():
                    self.fail(f"the highlight did not reach {self.drive['menu']}")
                return
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
