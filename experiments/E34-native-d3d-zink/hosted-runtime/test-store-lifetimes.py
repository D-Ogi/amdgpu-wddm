import importlib.util
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('lifetimes', Path(__file__).with_name('analyze-map-lifetimes.py'))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def trace():
    rows = [
        'lifetime event=begin map=1 ctx=1 resource=1 resource_id=1 object_id=1 target=0 width=128 height=1 depth=1 format=0 bind=8 level=0 usage=2 user_ptr=0 runtime=0 x=0 y=0 z=0 box_width=128 box_height=1 box_depth=1',
        'lifetime event=result map=1 success=1 resource=1 resource_id=1 object_id=1 usage=2 staging=0 stride=0 layer_stride=0',
        'lifetime event=checkpoint marker=1 requests=1 successful=1 failed=0 ended=0 pending=0 live=1',
        'store event=begin store=2 map=1 writer=descriptor kind=get_descriptor offset=96 bytes=32 capacity=128 mapped_offset=0 valid=1',
        'lifetime event=checkpoint marker=2 requests=1 successful=1 failed=0 ended=0 pending=0 live=1',
        'store event=end store=2',
        'lifetime event=checkpoint marker=3 requests=1 successful=1 failed=0 ended=0 pending=0 live=1',
        'lifetime event=end map=1',
    ]
    for index, begun, ended in [(2, 0, 0), (4, 1, 0), (6, 1, 1)]:
        rows[index] += f' stores_begun={begun} stores_ended={ended}'
    return [f'BC250 audit {row} seq={i} time_ns={i}' for i, row in enumerate(rows, 1)]


class Stores(unittest.TestCase):
    def test_crossing(self):
        result = m.analyze(trace())
        self.assertEqual(result['completed_store_span_bytes'], 32)
        self.assertEqual(result['pending_store_ids'], [])
        self.assertEqual(result['stores'][0]['crossing_markers'], [2])
        self.assertEqual(result['stores'][0]['mapped_resource_id'], 1)
        self.assertFalse(result['stores'][0]['staging'])

    def test_pending_at_marker(self):
        result = m.analyze(trace(), allow_live=True, start_marker=1, end_marker=2)
        self.assertEqual(result['completed_store_span_bytes'], 0)
        self.assertEqual(result['pending_store_ids'], [2])
        self.assertIsNone(result['stores'][0]['end_seq'])
        self.assertEqual(result['interval_store_span_bytes'], 0)
        self.assertEqual(result['interval_boundary_store_ids'], [2])

    def test_interval_excludes_startup(self):
        result = m.analyze(trace(), allow_live=True, start_marker=2, end_marker=3)
        self.assertEqual(result['completed_store_span_bytes'], 32)
        self.assertEqual(result['interval_store_span_bytes'], 0)
        self.assertEqual(result['interval_boundary_store_ids'], [2])
        whole = m.analyze(trace(), allow_live=True, start_marker=1, end_marker=3)
        self.assertEqual(whole['interval_store_span_bytes'], 32)
        self.assertEqual(whole['interval_boundary_store_ids'], [])

    def test_bad_evidence(self):
        mutations = [
            (3, 'map=1', 'map=99'),
            (3, 'capacity=128', 'capacity=129'),
            (3, 'valid=1', 'valid=0'),
            (3, 'bytes=32', 'bytes=33'),
            (3, 'offset=96', 'offset=18446744073709551615'),
            (3, 'kind=get_descriptor', 'kind=unknown'),
            (5, 'store=2', 'store=3'),
            (0, 'usage=2', 'usage=1'),
            (0, 'target=0', 'target=2'),
            (3, 'seq=4', 'seq=5'),
        ]
        for index, old, new in mutations:
            with self.subTest(old=old, new=new):
                rows = trace()
                self.assertIn(old, rows[index])
                rows[index] = rows[index].replace(old, new)
                with self.assertRaises(ValueError):
                    m.analyze(rows)

    def test_unmap_before_completion(self):
        rows = trace()
        rows[4] = 'BC250 audit lifetime event=end map=1 seq=5 time_ns=5'
        with self.assertRaisesRegex(ValueError, 'unmap while store pending'):
            m.analyze(rows)

    def test_missing_whole_store_detected(self):
        rows = [r for r in trace() if 'audit store' not in r]
        with self.assertRaisesRegex(ValueError, 'event sequence'):
            m.analyze(rows)

    def test_missing_store_pair_resequenced(self):
        rows = [r for r in trace() if 'audit store' not in r]
        import re
        rows = [re.sub(r'seq=\d+', f'seq={i}', r) for i, r in enumerate(rows, 1)]
        with self.assertRaisesRegex(ValueError, 'checkpoint stores_begun mismatch'):
            m.analyze(rows)

    def test_wrong_completion_count(self):
        rows = trace()
        rows[4] = rows[4].replace('stores_ended=0', 'stores_ended=1')
        with self.assertRaisesRegex(ValueError, 'checkpoint stores_ended mismatch'):
            m.analyze(rows)

    def test_mixed_checkpoint_schemas(self):
        rows = trace()
        rows[4] = rows[4].replace(' stores_begun=1 stores_ended=0', '')
        with self.assertRaisesRegex(ValueError, 'mixed store checkpoint schemas'):
            m.analyze(rows)

    def test_store_after_unmap(self):
        rows = trace()
        rows[2] = 'BC250 audit lifetime event=end map=1 seq=3 time_ns=3'
        with self.assertRaisesRegex(ValueError, 'outside successful map lifetime'):
            m.analyze(rows)

    def test_runtime_event_counter(self):
        rows = [r + ' runtime_events=1' if 'event=checkpoint' in r else r for r in trace()]
        rows.insert(0, 'BC250 audit runtime event=import seq=1')
        self.assertEqual(m.analyze(rows)['successful'], 1)
        with self.assertRaisesRegex(ValueError, 'runtime checkpoint count mismatch'):
            m.analyze(rows[1:])

    def test_runtime_sequence_gap(self):
        rows = ['BC250 audit runtime event=import seq=2'] + trace()
        with self.assertRaisesRegex(ValueError, 'runtime sequence gap'):
            m.analyze(rows)

    def test_runtime_missing_checkpoint_counter(self):
        rows = ['BC250 audit runtime event=import seq=1'] + trace()
        with self.assertRaisesRegex(ValueError, 'runtime events without checkpoint counter'):
            m.analyze(rows)

    def test_legacy(self):
        rows = [r for r in trace() if 'event=checkpoint' not in r and 'audit store' not in r]
        rows = [' '.join(v for v in r.split() if not v.startswith('seq=')) for r in rows]
        result = m.analyze(rows)
        self.assertEqual(result['successful'], 1)
        self.assertEqual(result['stores'], [])


if __name__ == '__main__':
    unittest.main()
