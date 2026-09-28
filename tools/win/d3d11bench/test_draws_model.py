"""Pins draws_model.py: no edge through a pixel centre at common sizes, and a known image. Run by build.ps1."""

import unittest

import draws_model
import imgdiff


class DrawsModelTest(unittest.TestCase):
    def test_no_edge_touches_a_pixel_centre(self):
        # expected() asserts it for every pixel near every triangle.
        for width, height in ((16, 16), (64, 64), (800, 600), (1280, 720), (1920, 1080), (2560, 1440)):
            draws_model.expected(40, width, height)

    def test_legs_differ_by_an_odd_number(self):
        for width, height in ((16, 16), (64, 64), (1280, 720), (1920, 1080), (8192, 8192)):
            dx, dy = draws_model.legs(width, height)
            self.assertEqual((dy - dx) % 2, 1)

    def test_detects_a_changed_pixel(self):
        model = draws_model.expected(8, 64, 64)
        pixels = bytearray(b"".join(bytes(p) for row in model for p in row))
        image = (64, 64, bytes(pixels))
        self.assertEqual(draws_model.compare(8, image), [])
        pixels[0] ^= 1
        self.assertEqual(len(draws_model.compare(8, (64, 64, bytes(pixels)))), 1)

    def test_64x64_eight_draws_checksum(self):
        # The development PC's native D3D11, DXVK and WARP all produced 1cb634333a935ce5 for
        # --size 64x64 --draws 8 (scene revision 2).
        model = draws_model.expected(8, 64, 64)
        image = (64, 64, b"".join(bytes(p) for row in model for p in row))
        self.assertEqual(imgdiff.checksum(image), "1cb634333a935ce5")


if __name__ == "__main__":
    unittest.main()
