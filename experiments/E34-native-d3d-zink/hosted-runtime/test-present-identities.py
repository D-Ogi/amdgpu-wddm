"""Synthetic positive/negative stream controls for runtime surface identity joins."""
import importlib.util
from pathlib import Path
import unittest

spec=importlib.util.spec_from_file_location("identities",Path(__file__).with_name("analyze-present-identities.py"))
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
IMPORT="BC250 audit runtime event=import seq=1 time_ns=1 resource=1000 resource_id=11 object_id=12 allocation=2000 identity=3000 va=4096 width=1920 height=1200 format=105 bind=40000a"
WAIT="BC250 audit present event=wait device=3000 present=1 context=4000 count=1 hr=00000000"
FENCE="BC250 audit present event=wait_fence device=3000 present=1 index=0 sync=5000 value=3"
SIGNAL="BC250 audit present event=signal device=3000 present=1 context=4000 sync=6000 value=1 hr=00000000"
COMPLETE="BC250 audit present event=complete pid=9 device=3000 present=1 time_ns=2 context=4000 src=1000 src_allocation=2000 dst=0 dst_allocation=0 width=1920 height=1200 format=105 primary=1 flags=0 hr=00000000"
END="BC250 audit lifetime event=checkpoint seq=1 marker=8 runtime_events=1"
ROWS=[IMPORT,WAIT,FENCE,SIGNAL,COMPLETE,END]

class IdentityControls(unittest.TestCase):
    def test_positive_and_runtime_map(self):
        rows=ROWS[:5]+["BC250 audit lifetime event=begin map=21 runtime=1 resource_id=11 usage=2",END]
        result=m.analyze("\n".join(rows),8)
        self.assertEqual(result["presented_resource_ids"],[11])
        self.assertEqual(result["presented_resource_maps"],[dict(map=21,resource_id=11,usage=2)])
    def test_pointer_reuse(self):
        rows=ROWS[:5]+["BC250 audit runtime event=destroy seq=2 resource=1000 resource_id=11 object_id=12"]
        rows += [IMPORT.replace("seq=1","seq=3").replace("resource_id=11","resource_id=13").replace("object_id=12","object_id=14")]
        rows += [r.replace("present=1","present=2") for r in (WAIT,FENCE,SIGNAL.replace("value=1","value=2"),COMPLETE)]
        rows += [END.replace("runtime_events=1","runtime_events=3")]
        result=m.analyze("\n".join(rows),8)
        self.assertEqual(result["presented_resource_ids"],[11,13])
    def test_mutations_fail_closed(self):
        cases={
            "missing_import":ROWS[1:],
            "runtime_gap":[IMPORT.replace("seq=1","seq=2")]+ROWS[1:],
            "wrong_allocation":ROWS[:4]+[COMPLETE.replace("src_allocation=2000","src_allocation=2001"),END],
            "wrong_device":[IMPORT.replace("identity=3000","identity=3001")]+ROWS[1:],
            "wrong_shape":ROWS[:4]+[COMPLETE.replace("width=1920","width=1"),END],
            "lost_fence":ROWS[:2]+ROWS[3:],
            "lost_signal":ROWS[:3]+ROWS[4:],
            "failed_signal":ROWS[:3]+[SIGNAL.replace("hr=00000000","hr=80004005")]+ROWS[4:],
            "signal_gap":ROWS[:3]+[SIGNAL.replace("value=1","value=2")]+ROWS[4:],
            "wrong_context":ROWS[:3]+[SIGNAL.replace("context=4000","context=4001")]+ROWS[4:],
            "counter_gap":ROWS[:5]+[END.replace("runtime_events=1","runtime_events=2")],
            "pending_present":ROWS[:4]+[END],
            "no_boundary":ROWS[:5],
            "present_gap":[r.replace("present=1","present=2") for r in ROWS],
            "no_render_work":[r.replace("value=3","value=0") for r in ROWS],
        }
        for label,rows in cases.items():
            with self.subTest(label=label):
                with self.assertRaises(ValueError):m.analyze("\n".join(rows),8)

if __name__=="__main__":unittest.main()
