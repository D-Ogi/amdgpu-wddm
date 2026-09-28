import importlib.util
from pathlib import Path
import unittest

spec=importlib.util.spec_from_file_location("origins",Path(__file__).with_name("analyze-ddi-origins.py"))
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)


def trace():
    return """BC250 audit ddi event=begin id=1 tid=10 time_ns=1 origin=update_subresource ctx=100 resource=200 level=0 usage=a x=0 y=0 z=0 box_width=1920 box_height=1200 box_depth=1
BC250 audit lifetime event=begin map=1 tid=10 time_ns=2 ctx=100 resource=200 resource_id=1 object_id=2 target=2 width=1920 height=1200 depth=1 format=105 bind=8 level=0 usage=a user_ptr=0 runtime=0 x=0 y=0 z=0 box_width=1920 box_height=1200 box_depth=1
BC250 audit lifetime event=result map=1 time_ns=3 success=1 resource=300 resource_id=3 object_id=4 usage=a staging=1 stride=7680 layer_stride=9216000
BC250 audit ddi event=copy_complete id=1 tid=10 time_ns=4
BC250 audit lifetime event=end map=1 time_ns=5
BC250 audit ddi event=end id=1 tid=10 time_ns=6
"""


class Origins(unittest.TestCase):
    def test_staging_replacement(self):
        r=m.analyze(trace())
        self.assertEqual(r["matched_map_count"],1)
        self.assertEqual(r["copy_complete_count"],1)
        self.assertFalse(r["unmatched_map_ids"])

    def test_resource_map_does_not_claim_copy(self):
        s=trace().replace("origin=update_subresource","origin=resource_map")
        s="\n".join(l for l in s.splitlines() if "event=copy_complete" not in l)
        self.assertEqual(m.analyze(s)["copy_complete_count"],0)

    def test_interleaved_threads_same_resource(self):
        first=trace().splitlines()
        second=trace().replace("id=1 tid=10", "id=2 tid=20").replace("map=1", "map=2").replace("tid=10", "tid=20").splitlines()
        mixed="\n".join(line for pair in zip(first,second) for line in pair)
        r=m.analyze(mixed)
        self.assertEqual(r["matched_map_count"],2)
        self.assertEqual(r["copy_complete_count"],2)
        self.assertEqual([s["maps"] for s in r["scopes"]],[[1],[2]])

    def test_refuses_corrupt_attribution(self):
        original=trace()
        variants=[
            "\n".join(l for l in original.splitlines() if "event=copy_complete" not in l),
            original.replace("map=1 tid=10", "map=1 tid=11"),
            original.replace("ctx=100 resource=200 level", "ctx=100 resource=201 level"),
            original.replace("event=copy_complete id=1 tid=10 time_ns=4", "event=copy_complete id=1 tid=10 time_ns=2"),
            original.replace("event=end id=1 tid=10 time_ns=6", "event=end id=1 tid=10 time_ns=3"),
            original.replace("id=1 tid=10", "id=2 tid=10"),
            original.replace("origin=update_subresource", "origin=resource_map"),
            original.replace("BC250 audit ddi event=end id=1 tid=10 time_ns=6", ""),
        ]
        for s in variants:
            with self.subTest(trace=s),self.assertRaises(ValueError):m.analyze(s)


if __name__ == "__main__":unittest.main()
