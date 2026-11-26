"""Offline injected-I/O tests: no devices, modules, or hardware access."""
import errno
import importlib.util
from pathlib import Path
import struct
import unittest
from unittest.mock import patch

def load(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + '.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod

B = load('postcopy-buffers-C100')
C = load('postcopy-core-C100')

class Model:
    def __init__(self, fault=None):
        self.fault = fault
        self.pages = [bytearray(4096), bytearray(4096)]
        self.exposed = []
        self.commands = []
        self.source = 0
        self.regs = {'response': 1, 'command': 0}
        self.regs.update({'arg' + str(i): 0 for i in range(6)})
    def ioctl(self, cmd, slot, size):
        header = struct.pack('<IIII', 1, slot, 0, 0)
        if cmd == B.FILL:
            self.pages[slot][:] = struct.pack('<I', B.CANARIES[slot]) * 1024
            return header
        if cmd == B.EXPOSE:
            self.exposed.append(slot)
            if self.fault == 'copyout':
                raise OSError(errno.EFAULT, 'copyout lost after retention')
            if self.fault == 'slotmixup':
                header = struct.pack('<IIII', 1, 1-slot, 0, 0)
            return header + struct.pack('<QII', 0x100000 + slot*4096, 4096, B.CANARIES[slot])
        if cmd == B.SNAPSHOT:
            return header + bytes(self.pages[slot])
        raise AssertionError('unexpected ioctl')
    def read(self, name):
        return self.regs[name]
    def write(self, name, value):
        self.regs[name] = value
        if name != 'command':
            return
        words = [self.regs['arg'+str(i)] for i in range(6)]
        self.commands.append(words)
        sub = words[0]
        result = 3
        if sub == 0x1F:
            self.source = words[2]
            result = 2
            if self.fault == 'load-success':
                result = 3
        elif sub == 0x14:
            slot = (words[2]-0x100000)//4096
            if self.fault == 'timeout':
                return
            if self.fault == 'refusal':
                result = 2
            elif self.fault != 'untouched':
                word = dict(C.CONTROLS)[self.source]
                if self.fault == 'stale-slot1' and slot == 1:
                    word = C.CONTROLS[0][1]
                struct.pack_into('<I', self.pages[slot], 0, word)
                if self.fault == 'tail':
                    self.pages[slot][4095] ^= 1
        self.regs['arg0'] = result
        self.regs['response'] = 1

class Tests(unittest.TestCase):
    def run_case(self, fault=None):
        model = Model(fault)
        events = []
        buf = B.Buffers(model.ioctl, events.append, lambda: None)
        def transact(*args):
            return C.command(*args, sleep=lambda _: None)
        return model, buf, lambda: C.observe(model, buf, events.append, lambda: None, transact=transact)
    def test_success(self):
        model, buf, run = self.run_case()
        result = run()
        self.assertTrue(result['data_observation_pass'])
        self.assertFalse(result['stable_load_ready_state_proven'])
        self.assertEqual(model.exposed, [0, 1])
        self.assertEqual(buf.verified, [True, True])
        self.assertEqual([x[0] for x in model.commands], [4,31,20,31,20])
    def test_data_faults(self):
        for fault in ('stale-slot1','untouched','tail'):
            with self.subTest(fault=fault):
                model, buf, run = self.run_case(fault)
                with self.assertRaises(RuntimeError): run()
                self.assertTrue(buf.failed)
                self.assertEqual(model.exposed, [0,1] if fault == 'stale-slot1' else [0])
    def test_protocol_failure_stops_second_slot(self):
        for fault in ('refusal','timeout','load-success'):
            with self.subTest(fault=fault):
                model, buf, run = self.run_case(fault)
                with self.assertRaises(RuntimeError): run()
                self.assertNotIn(1, model.exposed)
                self.assertEqual(buf.pending[0], fault != 'load-success')
    def test_copyout_loss_retains(self):
        model, buf, run = self.run_case('copyout')
        with self.assertRaises(OSError): run()
        self.assertEqual(buf.exposure_attempted, [True,False])
        self.assertTrue(buf.state()['kernel_retains_until_reset'])
        self.assertEqual([x[0] for x in model.commands], [4,31])
    def test_slot_mixup(self):
        model, buf, run = self.run_case('slotmixup')
        with self.assertRaisesRegex(RuntimeError, 'slot/header'): run()
        self.assertEqual(model.exposed, [0])
        self.assertFalse(any(buf.pending))
    def test_both_filled_before_expose_and_no_reuse(self):
        model, buf, run = self.run_case()
        with self.assertRaises(RuntimeError): buf.expose(0)
        run()
        with self.assertRaises(RuntimeError): buf.prepare()
        with self.assertRaises(RuntimeError): buf.expose(0)
    def test_logging_failure_latches_exposure(self):
        model, buf, run = self.run_case()
        buf.prepare()
        def broken(event): raise OSError('log failure')
        buf.emit = broken
        with self.assertRaises(OSError): buf.expose(0)
        self.assertTrue(buf.exposure_attempted[0])
        self.assertTrue(buf.failed)
        self.assertEqual(model.exposed, [])
    def test_stop_after_exposure_blocks_submit(self):
        model, buf, run = self.run_case()
        buf.prepare()
        def stop():
            if model.exposed: raise RuntimeError('STOP')
        buf.guard = stop
        with self.assertRaisesRegex(RuntimeError, 'STOP'): buf.expose(0)
        self.assertTrue(buf.failed)
        self.assertFalse(any(buf.pending))
    def test_unknown_command_has_zero_writes(self):
        model = Model()
        with self.assertRaises(RuntimeError):
            C.command(model, [5,0,0,0,0,0], (1,3), lambda e: None, lambda: None)
        self.assertEqual(model.commands, [])
    def test_deadline_zero_writes(self):
        model = Model()
        times = iter((0.0, 2.0))
        with self.assertRaisesRegex(RuntimeError, 'deadline'):
            C.command(model, [4,0,0,0,0,0], (1,3), lambda e: None,
                      lambda: None, now=lambda: next(times))
        self.assertEqual(model.commands, [])
    def test_worker_bad_pin_fails_before_runtime(self):
        worker = load('postcopy-C100')
        argv = ['worker', '--execute', '--kit', 'unused', '--preflight', 'unused',
                '--output', 'unused', '--status', 'unused']
        with patch('sys.argv', argv), patch.object(Path, 'read_bytes', return_value=b'bad pin'):
            with self.assertRaisesRegex(RuntimeError, 'support pin changed'):
                worker.main()

if __name__ == '__main__':
    unittest.main(verbosity=2)
