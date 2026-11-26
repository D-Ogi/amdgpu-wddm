"""C096 two one-shot slots. Kernel, not this process, owns exposed memory."""
import ctypes
import hashlib
import os
import struct

CANARIES = (0xA55A3CC3, 0x6DB6E719)
FILL, EXPOSE, SNAPSHOT = 1074812512, 3223344737, 3490731618


class Device:
    def __init__(self):
        self.fd = None
        self.lib = ctypes.CDLL(None, use_errno=True)
        self.lib.ioctl.argtypes = [ctypes.c_int, ctypes.c_ulong, ctypes.c_void_p]
        self.lib.ioctl.restype = ctypes.c_int
        self.fd = os.open('/dev/bc250_dma_c096', os.O_RDWR | os.O_CLOEXEC)

    def __call__(self, command, slot, size):
        raw = struct.pack('<IIII', 1, slot, 0, 0) + bytes(size - 16)
        buf = ctypes.create_string_buffer(raw, size)
        ctypes.set_errno(0)
        if self.lib.ioctl(self.fd, command, ctypes.cast(buf, ctypes.c_void_p)) < 0:
            number = ctypes.get_errno()
            raise OSError(number, os.strerror(number))
        return buf.raw

    def close(self):
        if self.fd is not None:
            fd, self.fd = self.fd, None
            os.close(fd)  # C096 release never frees exposed allocations.


class Buffers:
    def __init__(self, ioctl, emit, guard):
        self.ioctl, self.emit, self.guard = ioctl, emit, guard
        self.filled = [False, False]
        self.exposure_attempted = [False, False]
        self.addresses = [None, None]
        self.pending = [False, False]
        self.verified = [False, False]
        self.failed = False

    def state(self):
        return dict(exposure_attempted=list(self.exposure_attempted),
                    dma_pending=list(self.pending), verified=list(self.verified),
                    failed=self.failed, kernel_retains_until_reset=any(self.exposure_attempted))

    def _io(self, command, slot, size):
        if self.failed:
            raise RuntimeError('buffer adapter stopped')
        try:
            self.guard()
            self.emit(dict(kind='buffer-io', phase='before', command=command, slot=slot))
            self.guard()
            result = self.ioctl(command, slot, size)
            if type(result) is not bytes or len(result) != size:
                raise RuntimeError('short or invalid ioctl output')
            self.emit(dict(kind='buffer-io', phase='after', command=command, slot=slot))
            self.guard()
            return result
        except BaseException:
            self.failed = True
            raise

    def _snapshot(self, slot):
        raw = self._io(SNAPSHOT, slot, 4112)
        if raw[:16] != struct.pack('<IIII', 1, slot, 0, 0):
            raise RuntimeError('snapshot slot/header mismatch')
        return raw[16:]

    def prepare(self):
        if any(self.filled) or any(self.exposure_attempted):
            raise RuntimeError('prepare once only')
        try:
            for slot in (0, 1):
                self._io(FILL, slot, 16)
                self.filled[slot] = True
                if self._snapshot(slot) != struct.pack('<I', CANARIES[slot]) * 1024:
                    raise RuntimeError('initial full-page canary mismatch')
        except BaseException:
            self.failed = True
            raise

    def expose(self, slot):
        if type(slot) is not int or slot not in (0, 1) or self.failed:
            raise RuntimeError('invalid slot or stopped adapter')
        if not all(self.filled) or self.exposure_attempted[slot] or (slot == 1 and not self.verified[0]):
            raise RuntimeError('exposure order/state violation')
        # Before even attempting ioctl/logging: lost copyout may already latch C096.
        self.exposure_attempted[slot] = True
        try:
            raw = self._io(EXPOSE, slot, 32)
            if raw[:16] != struct.pack('<IIII', 1, slot, 0, 0):
                raise RuntimeError('descriptor slot/header mismatch')
            dma, size, canary = struct.unpack_from('<QII', raw, 16)
            if not (0 < dma <= 0xFFFFFFFF - 4095 and dma % 4096 == 0 and
                    size == 4096 and canary == CANARIES[slot]):
                raise RuntimeError('invalid DMA descriptor')
            if dma in self.addresses:
                raise RuntimeError('duplicate DMA slot address')
            self.addresses[slot] = dma
            return dma
        except BaseException:
            self.failed = True
            raise

    def before_submit(self, slot):
        if self.failed or self.addresses[slot] is None or self.pending[slot] or self.verified[slot]:
            raise RuntimeError('DMA submit state violation')
        self.pending[slot] = True

    def verify_after_reply(self, slot, pair, expected):
        if self.failed or not self.pending[slot] or pair != (1, 3):
            raise RuntimeError('no fresh successful DMA reply')
        try:
            data = self._snapshot(slot)
            word = struct.unpack_from('<I', data)[0]
            if word != expected or data[4:] != struct.pack('<I', CANARIES[slot]) * 1023:
                raise RuntimeError('known word or tail mismatch')
            self.verified[slot] = True
            self.pending[slot] = False  # Diagnostic state only; no kernel release/refill.
            return dict(slot=slot, observed=word, unchanged_tail=True,
                        page_sha256=hashlib.sha256(data).hexdigest())
        except BaseException:
            self.failed = True
            raise
