"""Offline tests: real Python IO, real POSIX shell with fault-injected commands.

GRUB snippets are mechanically adapted to sh and commands are mocked. Those cases
test branching, NOT GRUB parser compatibility, persistence or EFI boot acceptance.
No production CLI, remote process, firmware or disk/mount command is executed.
"""
import importlib.util
from contextlib import contextmanager
import os
from pathlib import Path
import re
import subprocess
import tempfile
import shutil
import unittest
import uuid
from unittest import mock

HERE = Path(__file__).resolve().parent
SH = HERE.parents[2] / "toolchain/llvm-mingw-20261020-ucrt-x86_64/busybox/bin/sh.exe"
spec = importlib.util.spec_from_file_location("x3_files", HERE / "x3-files.py")
x3 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(x3)


@contextmanager
def workdir():
    # Python 3.14 TemporaryDirectory uses mode 0700, whose Windows ACL excludes
    # the restricted tool token. A normal inherited workspace ACL is sufficient.
    path = HERE / ("test-x3-" + uuid.uuid4().hex)
    path.mkdir(mode=0o777)
    try:
        yield str(path)
    finally:
        if path.resolve().parent != HERE.resolve():
            raise ValueError("test cleanup escaped X3 directory")
        shutil.rmtree(path)


def env(lines=b""):
    content = x3.HEADER + lines
    return content + b"#" * (1024 - len(content))


def original_config(default=4):
    names = ["full", "readonly", "off", "reference", "BC-250 reference: network only, no GPU probe", "host test"]
    return (f"set timeout=5\nset default={default}\n" + "".join(
        f'menuentry "{name}" {{\nlinux /boot/vmlinuz-lts\ninitrd /boot/initramfs-lts\n}}\n' for name in names)).encode()


class Files(unittest.TestCase):
    def test_env_preserves_bytes_and_case(self):
        other = b"saved_entry=another entry # with spaces\nBC250_try=keep\nempty=\n"
        self.assertEqual(x3.clear_env(env(other + b"bc250_try=2\n")), env(other))
        self.assertEqual(x3.clear_env(env()), env())

    def test_env_corruption_refused(self):
        for data in (b"", env()[:-1], env().replace(b"Block", b"block"), env(b"bc250_try=0\n"),
                     env(b"bc250_try=1\nbc250_try=2\n"), env(b"x=1\nx=2\n"), env(b"bad\n"),
                     env(b"x=\x00\n"), env(b"x=\xff\n"), env(b"x=1\r\n"), env(b"x=1")):
            with self.subTest(data=data[:55]), self.assertRaises(ValueError):
                x3.clear_env(data)

    def test_config_upgrade_idempotent_preserves_default(self):
        head, entry = (HERE / "x3-head.cfg").read_bytes(), (HERE / "x3-entry.cfg").read_bytes()
        for default in (0, 4, 6):
            old = original_config(default)
            new = x3.corrected_config(old, head, entry)
            self.assertIn(f"set default={default}\n".encode(), new)
            self.assertEqual(x3.corrected_config(new, head, entry), new)
            self.assertIn(old[old.index(b'menuentry'):], new)
            self.assertIn(b"set default=6\n", x3.switch_config(new))

    def test_config_bad_layout_refused(self):
        head, entry = (HERE / "x3-head.cfg").read_bytes(), (HERE / "x3-entry.cfg").read_bytes()
        for data in (original_config() + b"set default=4\n", original_config().replace(b"network only", b"other")):
            with self.assertRaises(ValueError):
                x3.corrected_config(data, head, entry)
        with self.assertRaises(ValueError):
            x3.switch_config(original_config())

    def test_real_files_backups_and_switch(self):
        with workdir() as tmp:
            stick = Path(tmp)
            g = stick / "boot/grub"
            g.mkdir(parents=True)
            old = original_config()
            previous_env = env(b"saved_entry=unchanged\nbc250_try=3\n")
            (g / "grub.cfg").write_bytes(old)
            (g / "grubenv").write_bytes(previous_env)
            x3.prepare_upgrade(stick, HERE)
            self.assertEqual((g / "grubenv").read_bytes(), previous_env)
            os.replace(g / "grub.cfg.x3-new", g / "grub.cfg")
            x3.verify_upgrade(stick, HERE)
            x3.prepare_switch(stick)
            os.replace(g / "grubenv.x3-new", g / "grubenv")
            os.replace(g / "grub.cfg.x3-new", g / "grub.cfg")
            x3.verify_switch(stick)
            self.assertEqual((g / "grubenv").read_bytes(), env(b"saved_entry=unchanged\n"))
            self.assertTrue(any(p.read_bytes() == previous_env for p in g.glob("grubenv.x3-backup-*")))
            self.assertTrue(any(p.read_bytes() == old for p in g.glob("grub.cfg.x3-backup-*")))

    def test_io_failure_never_changes_live_files(self):
        with workdir() as tmp:
            stick = Path(tmp)
            g = stick / "boot/grub"
            g.mkdir(parents=True)
            cfg = x3.corrected_config(original_config(), (HERE / "x3-head.cfg").read_bytes(), (HERE / "x3-entry.cfg").read_bytes())
            (g / "grub.cfg").write_bytes(cfg)
            (g / "grubenv").write_bytes(env(b"bc250_try=2\n"))
            with mock.patch.object(os, "fsync", side_effect=OSError("injected flush failure")), self.assertRaises(OSError):
                x3.prepare_switch(stick)
            self.assertEqual((g / "grub.cfg").read_bytes(), cfg)
            self.assertEqual((g / "grubenv").read_bytes(), env(b"bc250_try=2\n"))


class Shell(unittest.TestCase):
    def run_script(self, script, fail="", extra=""):
        with workdir() as tmp:
            log = Path(tmp) / "calls.txt"
            fixture = Path(tmp) / "fixture.sh"
            body = '''set -eu
record() { echo "$1" >> "$LOG"; [ "$FAIL" != "$1" ]; }
mount() { record "mount:$2"; }
python3() { record "python:$2"; }
mv() { record "mv:$1"; }
sync() { record sync; }
reboot() { record reboot; }
grub-script-check() { record syntax; }
'''
            fixture.write_text(body + extra + '\n. "' + script.as_posix() + '"\n', newline="\n")
            variables = dict(os.environ, FAIL=fail, LOG=log.as_posix())
            result = subprocess.run([str(SH), str(fixture)], env=variables, capture_output=True, text=True, timeout=10)
            calls = log.read_text().splitlines() if log.exists() else []
            return result, calls

    def test_real_sh_syntax(self):
        for name in ("to-windows-x3.sh", "x3-install.sh"):
            self.assertEqual(subprocess.run([str(SH), "-n", str(HERE / name)], capture_output=True).returncode, 0)

    def test_switch_faults_never_reboot(self):
        cases = ("python:validate", "mount:remount,rw", "python:prepare", "syntax",
                 "mv:/media/usb/boot/grub/grubenv.x3-new", "mv:/media/usb/boot/grub/grub.cfg.x3-new",
                 "python:verify", "sync", "mount:remount,ro")
        for fail in cases:
            with self.subTest(fail=fail):
                result, calls = self.run_script(HERE / "to-windows-x3.sh", fail)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertNotIn("reboot", calls)

    def test_switch_success_reboots_only_last(self):
        result, calls = self.run_script(HERE / "to-windows-x3.sh")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(calls[-3:], ["sync", "mount:remount,ro", "reboot"])

    def test_installer_stops_on_parser_failure(self):
        result, calls = self.run_script(HERE / "x3-install.sh", "syntax")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(any(x.startswith("mv:") for x in calls))
        self.assertNotIn("reboot", calls)


class GrubBranches(unittest.TestCase):
    def translated(self, text):
        # Explicitly a control-flow harness, NOT a GRUB interpreter/parser.
        text = re.sub(r"(?m)^(\s*)set (\w+)=(.*)$", r"\1\2=\3", text)
        text = re.sub(r"\(\$\{(\w+)\}\)(/[^\s;]+)", r'"${\1}\2"', text)
        return text

    def run_grub(self, body):
        with tempfile.NamedTemporaryFile(mode="w", suffix=".sh", prefix="test-x3-", dir=HERE, delete=False) as f:
            f.write(body)
        try:
            return subprocess.run([str(SH), f.name], capture_output=True, text=True, timeout=10)
        finally:
            Path(f.name).unlink()

    def test_counter_all_branches(self):
        text = self.translated((HERE / "x3-head.cfg").read_text())
        for default, value, read_ok, save_ok, want, saved in (
            (6, "", 0, 0, 6, "1"), (6, "1", 0, 0, 6, "2"), (6, "2", 0, 0, 6, "3"),
            (6, "3", 0, 0, 4, "no"), (6, "junk", 0, 0, 4, "no"),
            (6, "", 1, 0, 4, "no"), (6, "2", 0, 1, 4, "3"), (4, "", 0, 0, 4, "no")):
            with self.subTest(default=default, value=value, read=read_ok, save=save_ok):
                body = f'''default={default}; stick=usb; root=usb; saved=no; bc250_try=stale
load_env() {{ bc250_try='{value}'; return {read_ok}; }}
save_env() {{ saved=$bc250_try; return {save_ok}; }}
{text}
echo "RESULT $default $saved $root"
'''
                run = self.run_grub(body)
                self.assertEqual(run.returncode, 0, run.stderr)
                self.assertIn(f"RESULT {want} {saved} usb", run.stdout)

    def test_chain_failure_preserves_usb_root(self):
        text = self.translated((HERE / "x3-entry.cfg").read_text())
        text = re.sub(r'^menuentry .*\{$', 'entry() {', text, flags=re.M)
        for search_rc in (0, 1):
            body = f'''root=usb; stick=usb
search() {{ win_esp=nvme; return {search_rc}; }}
chainloader() {{ echo "CHAIN $1 ROOT $root"; return 1; }}
{text}
entry
echo "FALLBACK ROOT $root"
'''
            run = self.run_grub(body)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn("FALLBACK ROOT usb", run.stdout)
            if search_rc == 0:
                # root = the ESP while chainloader builds the device path (the form proven on the lab at 01:08:04Z).
                self.assertIn("CHAIN /EFI/Microsoft/Boot/bootmgfw.efi ROOT nvme", run.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
