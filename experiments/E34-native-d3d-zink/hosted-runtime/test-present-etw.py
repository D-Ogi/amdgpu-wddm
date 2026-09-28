"""Negative controls for the ordered allocation/fence ETW witness."""
import copy
import importlib.util
from pathlib import Path
import unittest
s=importlib.util.spec_from_file_location("join",Path(__file__).with_name("analyze-present-etw.py"))
m=importlib.util.module_from_spec(s);s.loader.exec_module(m)

class Controls(unittest.TestCase):
    def fixture(self):
        umd=[];events=[]
        for i in range(3):
            umd.append(dict(src=100+i,dst=0,wait=4+i,signal=1+i))
            common={"ThreadID":"7","ObjectCount":"1"}
            w=dict(common,TimeStamp=str(3*i),**{"ObjectArray[ObjectCount]":"[0x11]","MonitoredFenceValue[ObjectCount]":f"[{4+i}]"})
            p={"ThreadID":"7","TimeStamp":str(3*i+1),"ReturnStatus":"0","hSrcAllocHandle":hex(100+i),"hDstAllocHandle":"0"}
            s=dict(common,TimeStamp=str(3*i+2),ContextCount="1",**{"ObjectArray[ObjectCount]":"[0x22]","MonitoredFenceValue[ObjectCount]":f"[{1+i}]"})
            events.extend([("wait",w),("present",p),("signal",s)])
        return umd,events
    def test_positive(self):
        u,e=self.fixture();self.assertEqual(m.matches(u,e)["matched"],3)
    def test_missing_tail(self):
        u,e=self.fixture()
        with self.assertRaises(ValueError):m.matches(u,e[:-3])
    def test_corruption(self):
        cases=[(1,"hSrcAllocHandle","0xff"),(1,"hDstAllocHandle","0x1"),(1,"ReturnStatus","1"),(0,"MonitoredFenceValue[ObjectCount]","[5]"),(2,"MonitoredFenceValue[ObjectCount]","[2]"),(3,"ObjectArray[ObjectCount]","[0x44]"),(5,"ObjectArray[ObjectCount]","[0x44]"),(1,"ThreadID","8"),(2,"ContextCount","2"),(0,"ObjectCount","2"),(2,"TimeStamp","0")]
        for idx,k,v in cases:
            with self.subTest(field=k,index=idx):
                u,e=self.fixture();e[idx][1][k]=v
                with self.assertRaises(ValueError):m.matches(u,e)
    def test_reorder(self):
        u,e=self.fixture();e[0],e[1]=e[1],e[0]
        with self.assertRaises(ValueError):m.matches(u,e)
    def test_arrays(self):
        self.assertEqual(m.array({"a":"[0x11 : 0x22]"},"a",16),[17,34])
        self.assertEqual(m.fields("name, [a, b], c"),["name","[a, b]","c"])

if __name__=="__main__":unittest.main()
