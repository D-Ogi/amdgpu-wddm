"""Extraction must fail closed; another function's SO case is never the target."""
import unittest
from pathlib import Path
from so_admission_source_test import extract


class ExtractionControls(unittest.TestCase):
    def setUp(self):
        self.source = (Path(__file__).resolve().parents[2] /
            'driver/umd/d3d12/engine-ddi/generic-programs.cpp').read_text(encoding='utf-8')

    def test_real_source(self):
        final, partial = extract(self.source)
        self.assertIn('AbiVersion', final)
        self.assertEqual(set(partial), {'preraster', 'pixel'})

    def test_wrong_function_name(self):
        with self.assertRaises(ValueError):
            extract(self.source.replace('HRESULT compile_linked_exports(GenericStateObject& object,',
                'HRESULT moved_function(GenericStateObject& object,'))

    def test_missing_so_case(self):
        with self.assertRaises(ValueError):
            extract(self.source.replace('case D3D12DDI_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:', 'case 999:'))

    def test_duplicate_so_case(self):
        marker = 'case D3D12DDI_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:'
        with self.assertRaises(ValueError):
            extract(self.source.replace(marker, marker + '\n' + marker))

    def test_unrelated_later_case_cannot_replace_target(self):
        before = extract(self.source)
        extra = '\nvoid unrelated(){switch(7){case D3D12DDI_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: break;}}'
        self.assertEqual(extract(self.source + extra), before)


if __name__ == '__main__':
    unittest.main()
