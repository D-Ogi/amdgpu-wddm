"""Use M687's known copies with synthetic phase labels; no new lab evidence."""
import copy
import importlib.util
from pathlib import Path
import unittest

HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location("dwm",HERE/"analyze-dwm-checkpoints.py")
dwm=importlib.util.module_from_spec(spec)
spec.loader.exec_module(dwm)
FIXTURE=HERE.parents[2]/"evidence/windows/2026-09-27-E34-audit-client001/stderr.log"

class DwmCheckpointTests(unittest.TestCase):
    def setUp(self):
        self.text=FIXTURE.read_text()
        labels=["render-start","progress-a","dynamic-known-capture-start","dynamic-known-capture-end","progress-b","render-end","final-capture-start","final-capture-end"]
        self.receipts=[]
        for line in self.text.splitlines():
            if "BC250 audit lifetime event=checkpoint " not in line:
                continue
            row=dict(item.split("=",1) for item in line.split(dwm.lifetimes.PREFIX,1)[1].split())
            marker=int(row["marker"])
            if marker:
                self.receipts.append(dict(marker=marker,label=labels[marker-1],pid=3024,process_start_utc="synthetic-control",line=line))

    def test_known_copies_remain_visible_and_reads_separate(self):
        report=dwm.analyze(self.text,self.receipts)
        self.assertEqual(report["render_image_write_ids"],list(range(46,80,3)))
        mapped={row["map"]:row for row in report["maps"]}
        self.assertEqual(mapped[43]["phase"],"dynamic-known-capture")
        self.assertEqual(mapped[82]["phase"],"final-capture")
        self.assertEqual(len(report["live_buffer_ids"]),8)

    def test_bad_boundaries_rejected(self):
        variants=[]
        bad=copy.deepcopy(self.receipts);bad[1]["pid"]=999;variants.append(bad)
        bad=copy.deepcopy(self.receipts);bad[1]["line"]+=" mismatch";variants.append(bad)
        bad=copy.deepcopy(self.receipts);del bad[2];variants.append(bad)
        bad=copy.deepcopy(self.receipts);bad[3]["label"]="unpaired-capture-end";variants.append(bad)
        bad=copy.deepcopy(self.receipts);bad[-1]["label"]="missing-final";variants.append(bad)
        for index,bad in enumerate(variants):
            with self.subTest(index=index),self.assertRaises(ValueError):
                dwm.analyze(self.text,bad)

if __name__ == "__main__":
    unittest.main()
