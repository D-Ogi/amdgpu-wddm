"""Pins imgdiff.py on synthetic images, in memory. Run by build.ps1 before the compiler."""

import unittest

import imgdiff


def pam(width, height, pixels):
    header = "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n" % (width, height)
    return header.encode("ascii") + bytes(pixels)


class ImgDiffTest(unittest.TestCase):
    def test_identical(self):
        a = imgdiff.parse_pam(pam(2, 1, [1, 2, 3, 255, 4, 5, 6, 255]))
        r = imgdiff.compare(a, a, 0)
        self.assertEqual((r["pixels"], r["differing"], r["beyond_tolerance"], r["max_rgba"]), (2, 0, 0, [0, 0, 0, 0]))

    def test_tolerance_and_first_pixel(self):
        a = imgdiff.parse_pam(pam(2, 2, [10] * 16))
        b = imgdiff.parse_pam(pam(2, 2, [10] * 4 + [11, 10, 10, 10] + [10] * 4 + [10, 10, 13, 10]))
        r = imgdiff.compare(a, b, 1)
        self.assertEqual(r["differing"], 2)
        self.assertEqual(r["beyond_tolerance"], 1)
        self.assertEqual(r["max_rgba"], [1, 0, 3, 0])
        self.assertEqual(r["first_beyond"], (1, 1))
        self.assertEqual(imgdiff.compare(a, b, 3)["beyond_tolerance"], 0)

    def test_rejects_bad_input(self):
        with self.assertRaises(imgdiff.ImageError):
            imgdiff.parse_pam(b"P6\n2 1\n255\n" + bytes(6))
        with self.assertRaises(imgdiff.ImageError):
            imgdiff.parse_pam(pam(2, 1, [0] * 7))
        with self.assertRaises(imgdiff.ImageError):
            imgdiff.compare(imgdiff.parse_pam(pam(1, 1, [0] * 4)), imgdiff.parse_pam(pam(2, 1, [0] * 8)), 0)

    def test_checksum_matches_d3d11bench_byte_order(self):
        # One pixel R=1 G=2 B=3 A=4 is stored as B, G, R, A = 3, 2, 1, 4 in d3d11bench's staging texture.
        h = 14695981039346656037
        for byte in (3, 2, 1, 4):
            h = ((h ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        self.assertEqual(imgdiff.checksum(imgdiff.parse_pam(pam(1, 1, [1, 2, 3, 4]))), "%016x" % h)

    def test_diff_image_is_a_pam(self):
        a = imgdiff.parse_pam(pam(1, 1, [0, 0, 0, 255]))
        b = imgdiff.parse_pam(pam(1, 1, [1, 0, 20, 0]))
        w, h, p = imgdiff.parse_pam(imgdiff.diff_image(a, b))
        self.assertEqual((w, h, list(p)), (1, 1, [16, 0, 255, 255]))


if __name__ == "__main__":
    unittest.main()
