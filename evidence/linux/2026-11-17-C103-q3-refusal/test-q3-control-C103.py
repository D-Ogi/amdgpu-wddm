"""Injected mailbox tests only. Never opens hardware or invokes the host launcher."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

def load(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + '.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod

C = load('q3-control-core-C103')
# Mock address is deliberately unrelated to actual platform registers.
TARGET = 0x1000

class Model:
    def __init__(self, fault=None):
        self.fault = fault
        self.regs = {q: dict(response=1, command=0, parameter=0) for q in (0,3)}
        self.writes = []
        self.commands = []
        self.q3_reads = 0
    def read(self, queue, name):
        value = self.regs[queue][name]
        if queue == 3 and name == 'parameter' and value == TARGET and self.fault == 'readback':
            return value ^ 4
        if queue == 3 and name == 'command' and self.fault == 'unstable':
            self.regs[queue]['command'] += 1
        return value
    def write(self, queue, name, value):
        self.writes.append((queue,name,value))
        self.regs[queue][name] = value
        if queue == 3 and name == 'response' and self.fault == 'clear':
            self.regs[queue]['response'] = 1
        if queue == 3 and name == 'parameter' and self.fault == 'early':
            self.regs[queue]['response'] = 1
        if name != 'command': return
        self.commands.append((queue,value,self.regs[queue]['parameter']))
        if queue == 0:
            self.regs[0]['parameter'] = C.VERSION if value == 2 else self.regs[0]['parameter'] + 1
            self.regs[0]['response'] = 1
            return
        self.q3_reads += 1
        if self.fault == 'timeout': return
        if self.fault in ('FD','FC','FE','unknown'):
            self.regs[3]['response'] = dict(FD=0xFD,FC=0xFC,FE=0xFE,unknown=0xFFFFFFFF)[self.fault]
            return
        observed = self.regs[0]['parameter']
        if self.fault == 'stale' and self.q3_reads == 2: observed = C.CHALLENGES[0]+1
        if self.fault == 'echo': observed = TARGET
        if self.fault == 'zero': observed = 0
        if self.fault == 'ones': observed = 0xFFFFFFFF
        self.regs[3].update(response=1, parameter=observed)
        if self.fault == 'alias': self.regs[0]['command'] = 0x2A

class Tests(unittest.TestCase):
    def run_model(self, model, emit=lambda e: None, guard=lambda: None, **kwargs):
        return C.run(model,TARGET,emit,guard,sleep=lambda _: None,**kwargs)
    def test_pass_exact_sequence(self):
        m=Model(); result=self.run_model(m)
        self.assertTrue(result['passed'])
        self.assertFalse(result['other_address_mapping_proven'])
        self.assertEqual(m.commands,[(0,2,0),(0,1,C.CHALLENGES[0]),(3,0x2A,TARGET),
                                     (0,1,C.CHALLENGES[1]),(3,0x2A,TARGET)])
        self.assertEqual(len(m.writes),15)
    def test_refusals_no_second_challenge(self):
        for fault in ('FD','FC','FE','unknown','timeout'):
            with self.subTest(fault=fault):
                m=Model(fault)
                with self.assertRaises(RuntimeError): self.run_model(m)
                self.assertEqual(len(m.commands),3)
                self.assertEqual(m.q3_reads,1)
    def test_wrong_values(self):
        for fault in ('echo','zero','ones','stale'):
            with self.subTest(fault=fault):
                m=Model(fault)
                with self.assertRaisesRegex(RuntimeError,'signature mismatch'): self.run_model(m)
                self.assertEqual(m.q3_reads,2 if fault == 'stale' else 1)
    def test_q0_alias_detected(self):
        m=Model('alias')
        with self.assertRaisesRegex(RuntimeError,'q0 after'): self.run_model(m)
        self.assertEqual(m.q3_reads,1)
    def test_fresh_response_preparation(self):
        for fault in ('clear','early','readback'):
            with self.subTest(fault=fault):
                m=Model(fault)
                with self.assertRaises(RuntimeError): self.run_model(m)
                self.assertEqual(m.q3_reads,0)
    def test_initial_unknown_or_unstable(self):
        for response,fault in ((0,None),(0xFFFFFFFF,None),(1,'unstable')):
            with self.subTest(response=response,fault=fault):
                m=Model(fault);m.regs[3]['response']=response
                with self.assertRaises(RuntimeError): self.run_model(m)
                self.assertFalse(any(q==3 for q,n,v in m.writes))
    def test_stop_after_committed_read(self):
        m=Model()
        def guard():
            if m.q3_reads: raise RuntimeError('STOP')
        with self.assertRaisesRegex(RuntimeError,'STOP'): self.run_model(m,guard=guard)
        self.assertEqual(m.q3_reads,1)
        self.assertEqual(len(m.commands),3)
    def test_log_failure_before_q3_write(self):
        m=Model()
        def emit(e):
            if e.get('kind')=='request' and e['queue']==3: raise OSError('logging failed')
        with self.assertRaises(OSError): self.run_model(m,emit=emit)
        self.assertFalse(any(q==3 for q,n,v in m.writes))
    def test_deadline(self):
        m=Model();clock=iter((0.0,2.0))
        with self.assertRaisesRegex(RuntimeError,'deadline'): self.run_model(m,now=lambda:next(clock))
        self.assertEqual(m.writes,[])
    def test_bad_target(self):
        for target in (0,1,True,0x100000000):
            with self.assertRaises(RuntimeError): C.run(Model(),target,lambda e:None,lambda:None)
    def test_worker_bad_pin_precedes_runtime(self):
        worker=load('q3-control-C103')
        argv=['worker','--execute','--kit','unused','--preflight','unused','--output','unused','--status','unused']
        with patch('sys.argv',argv),patch.object(Path,'read_bytes',return_value=b'bad pin'):
            with self.assertRaisesRegex(RuntimeError,'support pin changed'): worker.main()

if __name__=='__main__': unittest.main(verbosity=2)
