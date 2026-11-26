"""The space behind the image file of the outer firmware volume of a BC-250 image (read only).

    python free_space.py <image>

The outer volume is at 0xAE0000 of the image and is 0x320000 bytes. The script walks its files from the header
length, stops at the free space (a size of 0xFFFFFF or an erased header), and prints whether the bytes behind the
last file all read FF up to the end of the volume.
"""
import sys

data = open(sys.argv[1], 'rb').read()
base, length = 0xAE0000, 0x320000
d = data[base:base + length]
if d[0x28:0x2C] != b'_FVH' or int.from_bytes(d[0x20:0x28], 'little') != length:
    raise SystemExit('the outer volume is not where the script expects it')
at = int.from_bytes(d[0x30:0x32], 'little')
end = None
while at + 24 <= length:
    size = int.from_bytes(d[at + 20:at + 23], 'little')
    if size == 0xFFFFFF or d[at:at + 24] == b'\xff' * 24:
        break
    end = at + size
    print('file at volume 0x%06X (image 0x%06X): type 0x%02X, 0x%X bytes, ends at image 0x%X' % (
        at, base + at, d[at + 18], size, base + end))
    if size < 24:
        break
    at = (at + size + 7) & ~7
tail = d[end:]
first = next((i for i, b in enumerate(tail) if b != 0xFF), None)
print('behind the last file: %d bytes to the end of the volume (image 0x%X); all FF: %s' % (
    len(tail), base + length, first is None))
if first is not None:
    print('the first byte that is not FF: image 0x%X, value 0x%02X' % (base + end + first, tail[first]))
sector = (base + end + 0xFFF) & ~0xFFF
print('the first whole free sector of 4096 bytes: image 0x%X; %d whole free sectors' % (sector, (base + length - sector) // 0x1000))
