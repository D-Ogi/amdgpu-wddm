"""Refuse submission symbols which do not match the driver's RSDS identity."""
import argparse
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'packagecheck'))
from packagecheck import PeImage


def check(image, symbols, reader):
    pe = PeImage(image)
    if not pe.valid or not pe.codeview:
        raise ValueError('driver has no readable CodeView identity')
    result = subprocess.run([str(reader), 'dump', '-summary', str(symbols)],
                            capture_output=True, text=True, timeout=60, check=True)
    guid = re.findall(r'^\s*GUID: \{([0-9A-Fa-f-]+)\}\s*$', result.stdout, re.M)
    age = re.findall(r'^\s*Age: (\d+)\s*$', result.stdout, re.M)
    if len(guid) != 1 or len(age) != 1 or (guid[0].upper(), int(age[0])) != pe.codeview[:2]:
        raise ValueError('PDB GUID/age does not match the driver')
    return pe.codeview[:2]


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--pdb', type=Path, required=True)
    parser.add_argument('--reader', type=Path, required=True)
    args = parser.parse_args()
    try:
        guid, age = check(args.image, args.pdb, args.reader)
        print(f'PASS driver/PDB identity {guid} age {age}')
    except (ValueError, OSError, subprocess.SubprocessError) as exc:
        parser.exit(1, f'PDB check failed: {exc}\n')
