"""Native/C# health payload contract, checked by the normal monitor build."""
from pathlib import Path
import re
import unittest
HERE = Path(__file__).resolve().parent
HEADER = HERE.parents[2] / 'driver/kmd/bc250kmd_escape.h'
class StartHealthAbiTest(unittest.TestCase):
    def test_field_order_and_width(self):
        native = re.search(r'typedef struct _BC250_ESCAPE_START_HEALTH \{(.*?)\} BC250_ESCAPE_START_HEALTH;', HEADER.read_text(), re.S).group(1)
        managed = re.search(r'public struct StartHealthSnapshot\s*\{(.*?)\n    \}', (HERE/'src/Driver.cs').read_text(), re.S).group(1)
        expected=[]
        for kind,names in re.findall(r'(unsigned long long|unsigned long)\s+([^;]+);', native):
            for name in names.split(','):
                name=name.strip()
                if name=='Reserved[2]': expected.extend([('uint','Reserved0'),('uint','Reserved1')])
                else: expected.append(('ulong' if kind=='unsigned long long' else 'uint',name))
        actual=[]
        for kind,names in re.findall(r'public (uint|ulong)\s+([^;]+);', managed):
            actual.extend((kind,n.strip()) for n in names.split(','))
        self.assertEqual(expected,actual)
    def test_protocol_constants(self):
        source=HEADER.read_text()
        for name,value in {'ABI':1,'READ':0,'CONFIRM':1,'FULL':1,'READY':2,'VISIBLE':4,'CONFIRMED':8,'REQUIRED':7,'MIN_MS':60000,'FRESH_MS':15000}.items():
            found=re.search(r'#define BC250_START_HEALTH_'+name+r'\s+(\d+)',source)
            self.assertIsNotNone(found,name)
            self.assertEqual(int(found.group(1)),value,name)
