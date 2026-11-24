"""The DLL names that a PE image imports, from its import directory and its delay-import directory.

    import pe_image
    data = pe_image.read(path)
    names = pe_image.import_dlls(data)    # {'import': [...], 'delay': [...]}, in table order

No dependency outside the standard library. The reader fails closed: an image that it cannot judge raises PeError
(no MZ or PE signature, an optional header that is neither PE32 nor PE32+, a directory or a name outside the image,
a table without its terminating entry). An image without an optional header, or with fewer data directories than a
table needs, imports nothing from that table.

Layout (Microsoft PE format, "The .idata Section" and "Delay-Load Import Tables"): the import directory is data
directory 1, an array of 20-byte descriptors (name RVA at offset 12). The delay-import directory is data directory
13, an array of 32-byte descriptors (attributes at offset 0, name at offset 4). Each array ends with a descriptor
whose table and name fields are zero. A delay descriptor whose attribute bit 0 (dlattrRva of delayimp.h) is clear
can hold a virtual address in place of an RVA (the form before Visual C++ 7.0): a name at or above the image base
is taken as such an address.
"""
import pathlib

IMPORT_DIRECTORY = 1
DELAY_IMPORT_DIRECTORY = 13


class PeError(Exception):
    """The image cannot be judged."""


def read(path):
    """The bytes of a file that starts as a PE image (MZ, then the PE signature at e_lfanew)."""
    p = pathlib.Path(path)
    try:
        data = p.read_bytes()
    except OSError as e:
        raise PeError('%s: unreadable: %s' % (p, e))
    check(data, str(p))
    return data


def check(data, label='image'):
    if len(data) < 0x40 or data[:2] != b'MZ':
        raise PeError('%s: not a PE image (no MZ header)' % label)
    pe = int.from_bytes(data[0x3c:0x40], 'little')
    if pe + 4 > len(data) or data[pe:pe + 4] != b'PE\0\0':
        raise PeError('%s: not a PE image (no PE signature)' % label)


class _Image:
    def __init__(self, data):
        check(data)
        self.data = data
        coff = self.u(0x3c, 4) + 4
        self.machine = self.u(coff, 2)
        count, optsize = self.u(coff + 2, 2), self.u(coff + 16, 2)
        opt = coff + 20
        self.directories = {}
        self.base = 0
        self.wide = False
        if optsize:
            magic = self.u(opt, 2)
            if magic not in (0x10b, 0x20b):
                raise PeError('optional header magic 0x%x is neither PE32 nor PE32+' % magic)
            self.wide = magic == 0x20b
            self.base = self.u(opt + 24, 8) if self.wide else self.u(opt + 28, 4)
            dirs = opt + (112 if self.wide else 96)
            available = self.u(opt + (108 if self.wide else 92), 4)
            for index in (IMPORT_DIRECTORY, DELAY_IMPORT_DIRECTORY):
                if index < available and dirs + 8 * index + 8 <= opt + optsize:
                    self.directories[index] = self.u(dirs + 8 * index, 4)
        self.sections = [(self.u(s + 12, 4), max(self.u(s + 8, 4), self.u(s + 16, 4)), self.u(s + 20, 4))
                         for s in range(opt + optsize, opt + optsize + 40 * count, 40)]

    def u(self, at, size):
        if at < 0 or at + size > len(self.data):
            raise PeError('a read outside the image (offset 0x%x)' % at)
        return int.from_bytes(self.data[at:at + size], 'little')

    def offset(self, rva):
        for va, size, raw in self.sections:
            if va <= rva < va + size:
                return raw + rva - va
        raise PeError('RVA 0x%x is in no section' % rva)

    def text(self, rva):
        at = self.offset(rva)
        end = self.data.find(b'\0', at)
        if at >= len(self.data) or end < 0:
            raise PeError('a name outside the image (RVA 0x%x)' % rva)
        return self.data[at:end].decode('latin-1')

    # Per directory: the descriptor size, the offset of the name field and the fields that are all zero in the
    # terminating descriptor (import: lookup table, name, address table; delay: name, module handle, address
    # table, name table).
    LAYOUT = {IMPORT_DIRECTORY: (20, 12, (0, 12, 16)), DELAY_IMPORT_DIRECTORY: (32, 4, (4, 8, 12, 16))}

    def descriptors(self, index):
        """[(DLL name RVA, file offset of the descriptor)] of one directory, in table order."""
        rva = self.directories.get(index, 0)
        if not rva:
            return []
        size, name_at, key = self.LAYOUT[index]
        found, at = [], self.offset(rva)
        for _ in range(len(self.data) // size + 1):
            if not any(self.u(at + k, 4) for k in key):
                return found
            name = self.u(at + name_at, 4)
            if index == DELAY_IMPORT_DIRECTORY and not self.u(at, 4) & 1 and self.base and name >= self.base:
                name -= self.base
            if name <= 0:
                raise PeError('a descriptor of directory %d without a name (offset 0x%x)' % (index, at))
            found.append((name, at))
            at += size
        raise PeError('directory %d has no terminating entry' % index)

    def names(self, index):
        return [self.text(name) for name, _ in self.descriptors(index)]


def import_dlls(data):
    """{'import': [DLL names of the import directory], 'delay': [DLL names of the delay-import directory]}."""
    image = _Image(data)
    return {'import': image.names(IMPORT_DIRECTORY), 'delay': image.names(DELAY_IMPORT_DIRECTORY)}


def architecture(data):
    """'x64', 'x86', 'arm64' or the machine value in hex."""
    machine = _Image(data).machine
    return {0x8664: 'x64', 0x14c: 'x86', 0xaa64: 'arm64'}.get(machine, '0x%x' % machine)


def name_offsets(data):
    """[(DLL name, file offset of its text)] of the import directory: the negative control of a gate rewrites one."""
    image = _Image(data)
    return [(image.text(name), image.offset(name)) for name, _ in image.descriptors(IMPORT_DIRECTORY)]
