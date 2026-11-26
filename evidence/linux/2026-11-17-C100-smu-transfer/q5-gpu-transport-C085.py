"""GPU BAR5 indirect SMN transport, from amdgpu_device.c (MIT)."""
import ctypes
import mmap
import os


class GpuIndirectTransport:
    def __init__(self, dev, size, addresses, offsets, native, emit):
        self.addresses = frozenset(addresses)
        self.offsets = offsets
        self.emit = emit
        self.mapping = None
        if set(offsets) != {'mmPCIE_INDEX2','mmPCIE_DATA2'}:
            raise RuntimeError('wrong indirect register set')
        if any(type(o) is not int or o%4 or not 0 <= o <= size-4 for o in offsets.values()):
            raise RuntimeError('indirect register outside BAR5')
        self.dev, self.size = dev, size
        self.lib = ctypes.CDLL(str(native))
        self.lib.observer_read32.argtypes = [ctypes.c_void_p]
        self.lib.observer_read32.restype = ctypes.c_uint32
        self.lib.observer_write32.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        self.lib.observer_write32.restype = None
        cell = ctypes.c_uint32(0x12345678)
        self.lib.observer_write32(ctypes.addressof(cell), 0x89ABCDEF)
        if cell.value != 0x89ABCDEF or self.lib.observer_read32(ctypes.addressof(cell)) != cell.value:
            raise RuntimeError('native accessor RAM check failed')

    def open(self):
        fd = os.open(self.dev/'resource5', os.O_RDWR | os.O_SYNC | os.O_CLOEXEC)
        try:
            self.mapping = mmap.mmap(fd, self.size, flags=mmap.MAP_SHARED,
                                    prot=mmap.PROT_READ | mmap.PROT_WRITE)
        finally:
            os.close(fd)
        self.base = ctypes.addressof(ctypes.c_char.from_buffer(self.mapping))

    def close(self):
        if self.mapping is not None:
            self.mapping.close()
            self.mapping = None

    def _read(self, name):
        self.emit(dict(kind='gpu-bar-io', phase='before', operation='read', name=name))
        value = int(self.lib.observer_read32(self.base+self.offsets[name]))
        self.emit(dict(kind='gpu-bar-io', phase='after', operation='read', name=name, value=value))
        return value

    def _write(self, name, value):
        self.emit(dict(kind='gpu-bar-io', phase='before', operation='write', name=name, value=value))
        self.lib.observer_write32(self.base+self.offsets[name], value)
        self.emit(dict(kind='gpu-bar-io', phase='after', operation='write', name=name, value=value))

    def _select(self, address):
        if address not in self.addresses:
            raise RuntimeError('SMN address outside pinned q0/q5 fields')
        self._write('mmPCIE_INDEX2', address)
        if self._read('mmPCIE_INDEX2') != address:
            raise RuntimeError('indirect selector did not read back; no DATA2 access')

    def read_smu_reg(self, address):
        self._select(address)
        return self._read('mmPCIE_DATA2')

    def write_smu_reg(self, address, value):
        self._select(address)
        self._write('mmPCIE_DATA2', value)
        self._read('mmPCIE_DATA2')  # AMD's posted-data readback.
