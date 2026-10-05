"""Run: python -m unittest discover -s tools/diagusb   (after build_usb.py or gen_probes.py)"""

import io
import json
import os
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
PAYLOAD = HERE / "payload" / "bc250"
sys.path.insert(0, str(PAYLOAD))
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "regcalc"))

import decode_qr  # noqa: E402
import diag  # noqa: E402
import qrcodec  # noqa: E402
import gen_probes  # noqa: E402
import gen_sweep  # noqa: E402
from gen_probes import build, denied, deny_range  # noqa: E402
from regcalc import RegMap  # noqa: E402


class StubSegno:
    """Stand-in for the QR encoder, which is vendored onto the stick and not installed here.

    Only the size and shape of the matrix matter for the layout tests; 97 and 33 modules are the
    version 20 and version 4 codes the payload actually produces.
    """

    @staticmethod
    def make(text, **kwargs):
        n = 97 if len(text) > 500 else 33
        rows = [[(x * 7 + y * 3 + len(text)) % 3 == 0 for x in range(n)] for y in range(n)]
        return type("StubQr", (), {"matrix": rows})()


try:
    import segno  # noqa: F401,E402
except ImportError:
    sys.modules["segno"] = StubSegno
import qrshow  # noqa: E402


class FakeBar:
    """Register file where GRBM_GFX_INDEX selects which bank of banked registers is visible."""

    def __init__(self, probes, size=0x80000):
        self.size = size
        self.probes = probes
        self.idx_off = diag.reg_off(probes, "GRBM_GFX_INDEX")
        self.mem = {self.idx_off: diag.GFX_INDEX_BROADCAST}
        self.banks = {}
        self.writes = []

    def r32(self, off):
        if off + 4 > self.size:
            return None
        sh = (self.mem[self.idx_off] >> 8) & 0xFF
        return self.banks.get((off, sh), self.mem.get(off, 0))

    def w32(self, off, value):
        self.writes.append((off, value))
        self.mem[off] = value


class Probes(unittest.TestCase):
    def test_probe_offsets_come_from_headers(self):
        probes = build()
        gc = RegMap()
        for r in probes["regs"]:
            if r["ip"] == "GC":
                self.assertEqual(r["off"], gc.byte_offset("mm" + r["n"]))
        self.assertEqual(diag.reg_off(probes, "MP1_SMN_C2PMSG_90"), (0x16000 + 0x29A) * 4)
        self.assertTrue(all(r["off"] < 0x80000 for r in probes["regs"]), "all probes inside the 512 KiB BAR5")

    def test_checked_in_probes_json_is_current(self):
        self.assertEqual(json.loads((PAYLOAD / "probes.json").read_text())["id"], build()["id"])


class UvdDenyWindow(unittest.TestCase):
    """The UVD/VCN window is denied by rule. A read there is reported to wedge this SoC (facts M787)."""

    def test_the_band_comes_from_the_ip_table_and_covers_both_uvd_segments(self):
        low, high = deny_range()
        # cyan_skillfish_ip_offset.h: UVD0 segments at dword 0x7800 and 0x7E00, next IP base at 0x9000.
        self.assertEqual((low, high), (0x7800 * 4, 0x9000 * 4))
        self.assertTrue(denied(0x7800 * 4) and denied(0x7E00 * 4) and denied(high - 4))
        self.assertFalse(denied(low - 4) or denied(high))

    def test_no_probe_and_no_swept_register_is_inside_the_band(self):
        self.assertEqual([r for r in build()["regs"] if denied(r["off"])], [])
        self.assertEqual([r for r in gen_sweep.build()["regs"] if denied(r[2])], [])

    def test_a_spec_entry_inside_the_band_stops_the_generator(self):
        class FakeMap:
            regs = {"mmUVD_FAKE": (0, 0)}
            segs = {0: 0x7800}

            def byte_offset(self, name):
                return (self.segs[0] + self.regs[name][0]) * 4

        with mock.patch.object(gen_probes, "SPEC", [("UVD0", "nonexistent.h", [("mmUVD_FAKE", False)])]), \
                mock.patch.object(gen_probes, "RegMap", lambda **kwargs: FakeMap()):
            with self.assertRaises(SystemExit) as caught:
                build()
        self.assertIn("UVD/VCN", str(caught.exception))


class PhaseA(unittest.TestCase):
    def setUp(self):
        self.probes = build()
        self.bar = FakeBar(self.probes)
        cc = diag.reg_off(self.probes, "CC_GC_SHADER_ARRAY_CONFIG")
        spi = diag.reg_off(self.probes, "SPI_PG_ENABLE_STATIC_WGP_MASK")
        for sh in (0, 1):
            self.bar.banks[(cc, sh)] = diag.STOCK_CC_GC_SHADER_ARRAY_CONFIG
            self.bar.banks[(spi, sh)] = diag.STOCK_SPI_PG_WGP_MASK
        self.bar.mem[diag.reg_off(self.probes, "GRBM_STATUS")] = 0x00003028

    def test_banked_sampling_restores_broadcast(self):
        res = diag.phase_a(self.bar, self.probes, allow_writes=True)
        self.assertEqual(self.bar.mem[self.bar.idx_off], diag.GFX_INDEX_BROADCAST)
        self.assertEqual(set(res["bank"]), {"0.0", "0.1"})
        self.assertTrue(res["scratch"]["ok"])
        scratch = diag.reg_off(self.probes, "SCRATCH_REG0")
        self.assertEqual(self.bar.mem[scratch], 0, "scratch register restored")

    def test_only_two_registers_are_ever_written(self):
        diag.phase_a(self.bar, self.probes, allow_writes=True)
        allowed = {self.bar.idx_off, diag.reg_off(self.probes, "SCRATCH_REG0")}
        self.assertEqual({off for off, _ in self.bar.writes}, allowed)

    def test_readonly_mode_writes_nothing(self):
        res = diag.phase_a(self.bar, self.probes, allow_writes=False)
        self.assertEqual(self.bar.writes, [])
        self.assertNotIn("bank", res)

    def test_verdict_and_qr_roundtrip(self):
        a = diag.phase_a(self.bar, self.probes, allow_writes=True)
        verdict = diag.verdict(self.probes, a, None)
        self.assertIn("A_SCRATCH_RW", verdict)
        self.assertTrue(any(v.startswith("A_BANK0.1") and v.endswith("STOCK") for v in verdict))
        report = {"probes_id": self.probes["id"], "mode": "noload", "kernel": "test", "A": a, "verdict": verdict}
        summary = diag.summarize(report)
        chunks = qrcodec.encode(summary, chunk_chars=300)
        self.assertGreater(len(chunks), 1)
        scrambled = "noise\n" + "\n".join(reversed(chunks)) + "\n" + chunks[0].lower()
        self.assertEqual(qrcodec.decode(scrambled), json.loads(json.dumps(summary)))
        with self.assertRaisesRegex(ValueError, "missing chunk"):
            qrcodec.decode("\n".join(chunks[1:]))


class Codec(unittest.TestCase):
    def test_base41_roundtrip(self):
        for data in (b"", b"\x00", b"\xff", b"\xff\xff", bytes(range(256)), bytes(range(255))):
            self.assertEqual(qrcodec.b41decode(qrcodec.b41encode(data)), data)

    def test_alphabet_is_qr_alphanumeric_without_fragile_characters(self):
        qr_alnum = set("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:")
        self.assertTrue(set(qrcodec.ALPHABET) <= qr_alnum)
        self.assertFalse(set(qrcodec.ALPHABET) & set(" %*:"))


def sample_summary(probes):
    """A summary of a phase A run, as the stick would produce it."""
    bar = FakeBar(probes)
    bar.mem[diag.reg_off(probes, "GRBM_STATUS")] = 0x00003028
    a = diag.phase_a(bar, probes, allow_writes=True)
    report = {"probes_id": probes["id"], "mode": "noload", "kernel": "test",
              "dmi": {"board_vendor": "ASRock"}, "A": a, "verdict": []}
    report["verdict"] = diag.verdict(probes, a, None)
    return diag.summarize(report)


class Framebuffer(unittest.TestCase):
    """The QR code has to end up visible, which is a question of where it is written."""

    GEOMETRY = (1280, 800, 4, 5120)  # width, height, bytes per pixel, stride

    def draw(self, matrix, geometry=None, short=False):
        """draw_fb against an in-memory framebuffer -> (result, buffer, [(offset, length)])."""
        width, height, bytespp, stride = geometry or self.GEOMETRY
        buf, writes = bytearray(stride * height), []

        def pwrite(fd, data, off):
            n = max(1, len(data) // 2) if short and len(writes) % 2 == 0 else len(data)
            writes.append((off, n))
            buf[off:off + n] = data[:n]
            return n

        with mock.patch.object(qrshow, "fb_geometry", lambda *a: (width, height, bytespp, stride)), \
                mock.patch.object(qrshow, "fb_nodes", lambda: [("/sys/class/graphics/fb0", "/dev/fb0")]), \
                mock.patch("os.open", lambda *a: 7), mock.patch("os.close", lambda fd: None), \
                mock.patch("os.pwrite", pwrite, create=True):
            ok = qrshow.draw_fb(matrix)
        return ok, buf, writes

    def test_no_write_ends_on_a_scanline_boundary(self):
        """The bug this guards: the DRM fbdev damage rectangle of such a write comes out empty and
        the code is never flushed to the display, although every write succeeds."""
        ok, _buf, writes = self.draw(qrshow.make_matrix("X" * 1200))
        self.assertTrue(ok)
        stride = self.GEOMETRY[3]
        self.assertTrue(writes)
        for off, length in writes:
            self.assertNotEqual((off + length) % stride, 0, f"write at {off} ends on a line boundary")

    def test_version_20_layout_at_1280x800(self):
        matrix = qrshow.make_matrix("X" * 1200)
        self.assertEqual(len(matrix), 97 + 2 * qrshow.QUIET)
        scale, x0, y0 = qrshow.fb_layout(*self.GEOMETRY, len(matrix))
        self.assertEqual((scale, x0, y0), (7, 529, 32))  # 7 px per module, 735 px square
        self.assertGreater(x0, qrshow.TEXT_COLUMNS * 8, "the header text must not reach the code")

    def test_pixels_match_the_matrix(self):
        matrix = qrshow.make_matrix("X" * 20)
        ok, buf, _writes = self.draw(matrix)
        scale = qrshow.fb_layout(*self.GEOMETRY, len(matrix))[0]
        white = sum(row.count(0) for row in matrix) * scale * scale * self.GEOMETRY[2]
        self.assertTrue(ok)
        self.assertEqual(buf.count(b"\xff"), white)

    def test_short_writes_are_completed(self):
        matrix = qrshow.make_matrix("X" * 20)
        _ok, full, _w = self.draw(matrix)
        _ok, chopped, _w = self.draw(matrix, short=True)
        self.assertEqual(full, chopped)

    def test_unusable_framebuffers_are_refused(self):
        modules = 105
        self.assertIsNone(qrshow.fb_layout(1280, 800, 1, 1280, modules), "8 bpp is not supported")
        self.assertIsNone(qrshow.fb_layout(200, 100, 4, 800, modules), "too small for 2 px modules")
        self.assertIsNone(qrshow.fb_layout(1280, 800, 4, 4096, modules), "stride shorter than a line")

    def test_every_framebuffer_gets_the_code(self):
        """After modprobe amdgpu the console may sit on a different fbN than at boot."""
        matrix = qrshow.make_matrix("X" * 20)
        seen = []
        with mock.patch.object(qrshow, "fb_nodes",
                               lambda: [("/sys/class/graphics/fb0", "/dev/fb0"),
                                        ("/sys/class/graphics/fb1", "/dev/fb1")]), \
                mock.patch.object(qrshow, "draw_one", lambda m, sysfs, dev: seen.append(dev) or True):
            self.assertTrue(qrshow.draw_fb(matrix))
        self.assertEqual(seen, ["/dev/fb0", "/dev/fb1"])

    def test_text_never_reaches_into_the_code_or_scrolls(self):
        lines = qrshow.text_lines(0, 3, [f"B_LONG_VERDICT_LINE_{i} " * 8 for i in range(40)], rows=50)
        self.assertLessEqual(len(lines), 49)
        self.assertLessEqual(max(len(l) for l in lines), qrshow.TEXT_COLUMNS)

    def test_net_line_is_built_from_ip_output(self):
        wlan = ("3: wlan0    inet 192.168.1.57/24 brd 192.168.1.255 scope global wlan0\\"
                "       valid_lft forever preferred_lft forever\n")
        eth = "2: eth0    inet 10.0.2.15/24 brd 10.0.2.255 scope global dynamic eth0\n"
        self.assertEqual(qrshow.net_line(wlan), "NET ssh root@192.168.1.57 (wlan0)")
        self.assertEqual(qrshow.net_line(eth + wlan),
                         "NET ssh root@10.0.2.15 (eth0), root@192.168.1.57 (wlan0)")
        self.assertEqual(qrshow.net_line(""), "NET no address yet")
        # Anything that is not an "inet" record is ignored, IPv6 lines included.
        self.assertEqual(qrshow.net_line("2: eth0    inet6 fe80::1/64 scope link\nrubbish\n"),
                         "NET no address yet")

    def test_net_line_is_shown_only_with_ssh_set_up(self):
        self.assertIsNone(qrshow.current_net_line(keys=str(HERE / "no-such-file")))
        lines = qrshow.text_lines(0, 1, ["VERDICT"], rows=50, net="NET no address yet")
        self.assertLess(lines.index("NET no address yet"), lines.index("VERDICT"))
        self.assertNotIn("NET no address yet", qrshow.text_lines(0, 1, ["VERDICT"], rows=50))


class TestHook(unittest.TestCase):
    """BC250_TEST_DEV / BC250_TEST_BAR: exercising the MMIO path where there is no BC-250."""

    def hook(self, **env):
        with mock.patch.dict(os.environ, env, clear=False):
            for key in (diag.TEST_DEV_ENV, diag.TEST_BAR_ENV):
                if key not in env:
                    os.environ.pop(key, None)
            return diag.test_override()

    def test_needs_both_variables(self):
        self.assertIsNone(self.hook())
        self.assertIsNone(self.hook(BC250_TEST_DEV=str(HERE)))
        self.assertIsNone(self.hook(BC250_TEST_BAR="2"))

    def test_rejects_a_path_that_is_not_a_pci_device(self):
        with self.assertRaisesRegex(SystemExit, "not a PCI device directory"):
            self.hook(BC250_TEST_DEV=str(HERE), BC250_TEST_BAR="2")

    def test_rejects_a_bar_index_that_does_not_exist(self):
        with mock.patch("os.path.isfile", return_value=True), \
                self.assertRaisesRegex(SystemExit, "not a BAR index"):
            self.hook(BC250_TEST_DEV="/sys/bus/pci/devices/0000:00:01.0", BC250_TEST_BAR="9")

    def test_accepts_a_device_directory(self):
        with mock.patch("os.path.isfile", return_value=True):
            self.assertEqual(self.hook(BC250_TEST_DEV="/sys/bus/pci/devices/0000:00:01.0",
                                       BC250_TEST_BAR="2"),
                             {"dev": "/sys/bus/pci/devices/0000:00:01.0", "bar": 2})

    def test_a_readonly_bar_refuses_writes(self):
        bar = diag.Bar.__new__(diag.Bar)
        bar.readonly, bar.size = True, 0x1000
        with self.assertRaisesRegex(ValueError, "read-only"):
            bar.w32(0, 1)

    def test_run_is_labelled_so_it_cannot_pass_for_a_measurement(self):
        probes = build()
        test = {"dev": "/sys/bus/pci/devices/0000:00:01.0", "bar": 2}
        v = diag.verdict(probes, None, None, test)
        self.assertTrue(v[0].startswith("TEST_HOOK "))
        summary = diag.summarize({"probes_id": probes["id"], "mode": "readonly", "kernel": "t",
                                  "verdict": v, "test_hook": test})
        self.assertEqual(summary["test"], test)


class PhaseB(unittest.TestCase):
    def setUp(self):
        self.probes = build()

    def test_a_stuck_modprobe_does_not_block_and_skips_driver_side_reads(self):
        touched = []
        with mock.patch.object(diag, "run_patient", return_value=("timeout", "")), \
                mock.patch.object(diag, "run", return_value=(0, "no amdgpu lines")), \
                mock.patch.object(diag, "find_card", side_effect=AssertionError("would wait")), \
                mock.patch.object(diag, "find_debugfs", side_effect=AssertionError("would block")):
            res = diag.phase_b("/sys/bus/pci/devices/0000:01:00.0", FakeBar(self.probes),
                               self.probes, lambda name, text: touched.append(name))
        self.assertEqual(res["load"], "timeout")
        self.assertNotIn("dbg", res)
        self.assertNotIn("raw", res)
        self.assertIn("dmesg.txt", touched)
        self.assertTrue(any("B_MODPROBE_STUCK" in v for v in diag.verdict(self.probes, None, res)))

    def test_run_patient_gives_up_instead_of_waiting(self):
        import time
        t0 = time.time()
        rc, _out = diag.run_patient([sys.executable, "-c", "import time; time.sleep(30)"], 1)
        self.assertEqual(rc, "timeout")
        self.assertLess(time.time() - t0, 15)

    def test_debugfs_short_read_is_not_a_crash(self):
        with mock.patch("os.pread", return_value=b"", create=True):
            self.assertIsNone(diag.debugfs_read(7, 0x1234))
        with mock.patch("os.pread", return_value=b"\x01\x02\x03\x04", create=True):
            self.assertEqual(diag.debugfs_read(7, 0x1234, (0, 1)), 0x04030201)


class DecodeQr(unittest.TestCase):
    def setUp(self):
        self.probes = build()
        self.summary = sample_summary(self.probes)
        self.chunks = qrcodec.encode(self.summary, chunk_chars=90)
        self.assertGreaterEqual(len(self.chunks), 4, "the missing-chunk tests need a middle")
        self.tmpdir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmpdir, True)

    def scan_text(self):
        """What a phone actually delivers: out of order, one code scanned twice, plus noise."""
        return ("Fwd: codes from the BC-250\n" + self.chunks[-1] + "\nsent from my phone\n"
                + "\n".join(reversed(self.chunks[:-1])) + "\n" + self.chunks[0] + "\n")

    def run_cli(self, text, *args):
        tmp = Path(self.tmpdir) / "scan.txt"
        tmp.write_text(text, encoding="utf-8")
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), mock.patch("sys.stderr", err):
            code = decode_qr.main([str(tmp), *args])
        return code, out.getvalue(), err.getvalue()

    def test_decodes_scrambled_input_and_names_the_registers(self):
        code, out, err = self.run_cli(self.scan_text())
        self.assertEqual(code, 0)
        self.assertIn(f"{len(self.chunks)} of {len(self.chunks)} chunks present", err)
        self.assertIn("GRBM_STATUS [GC]", out)
        self.assertIn("A_SCRATCH_RW", out)
        self.assertIn("OFFSETS THE PREVIOUS DRIVER ATTEMPT USED", out)
        off = diag.reg_off(self.probes, "GRBM_STATUS")
        self.assertRegex(out, rf"GRBM_STATUS \[GC\]\s+{off:05X}\s+00003028")

    def test_missing_chunks_are_named(self):
        code, out, err = self.run_cli(self.chunks[0] + "\n" + self.chunks[-1])
        self.assertEqual(code, 2)
        self.assertIn(f"MISSING chunk(s): {', '.join(str(i) for i in range(2, len(self.chunks)))}", err)
        self.assertEqual(out, "")

    def test_input_without_chunks_says_so(self):
        code, _out, err = self.run_cli("just a mail about something else")
        self.assertEqual(code, 2)
        self.assertIn("No BC250 chunk found", err)

    def test_probes_of_another_revision_are_refused(self):
        other = dict(self.probes, id="DEADBEEF")
        path = Path(self.tmpdir) / "probes.json"
        path.write_text(json.dumps(other), encoding="utf-8")
        with self.assertRaises(SystemExit) as cm:
            self.run_cli(self.scan_text(), "--probes", str(path))
        self.assertIn("DEADBEEF", str(cm.exception))
        self.assertIn(self.probes["id"], str(cm.exception))

    def test_json_output_is_the_summary(self):
        code, out, _err = self.run_cli(self.scan_text(), "--json")
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(out), json.loads(json.dumps(self.summary)))


class Dmesg(unittest.TestCase):
    def test_digest(self):
        text = "\n".join([
            "[    5.1] [drm] amdgpu kernel modesetting enabled.",
            "[    5.9] amdgpu 0000:01:00.0: amdgpu: VRAM: 512M 0x000000F400000000 - 0x000000F41FFFFFFF (512M used)",
            "[    6.2] amdgpu 0000:01:00.0: amdgpu: ring gfx_0.0.0 uses VM inv eng 0 on hub 0",
            "[    6.3] amdgpu 0000:01:00.0: [drm] *ERROR* something failed",
            "[    6.4] [drm] Initialized amdgpu 3.64.0 for 0000:01:00.0 on minor 0",
            "[    7.0] usb 1-1: unrelated",
        ])
        d = diag.digest_dmesg(text)
        self.assertTrue(d["initialized"])
        self.assertEqual(d["rings"], 1)
        self.assertEqual(len(d["errors"]), 1)
        self.assertTrue(d["vram"].startswith("amdgpu 0000:01:00.0: amdgpu: VRAM: 512M"))


if __name__ == "__main__":
    unittest.main()
