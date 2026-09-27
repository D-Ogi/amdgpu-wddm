"""Bind a runtime PASS receipt to the exact binaries selected for deployment."""
import argparse
import hashlib
import json
from pathlib import Path


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest().upper()


def verify(receipt, artifacts):
    if receipt.get('status') != 'PASS' or receipt.get('exit_code') != 0:
        raise ValueError('Runtime test did not pass')
    measured = receipt.get('loaded_artifacts', {})
    for name, path in artifacts.items():
        if name not in measured or measured[name].upper() != sha(path):
            raise ValueError('Untested or changed artifact: '+name)
    if not artifacts:
        raise ValueError('No deployment artifacts')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--receipt',required=True,type=Path)
    p.add_argument('--artifact',required=True,action='append',help='module-name=local-path')
    p.add_argument('--out',required=True,type=Path)
    args = p.parse_args()
    artifacts = dict(x.split('=',1) for x in args.artifact)
    if len(artifacts) != len(args.artifact):
        raise ValueError('Duplicate artifact name')
    receipt=json.loads(args.receipt.read_text(encoding='utf-8-sig'))
    verify(receipt,artifacts)
    args.out.write_text(json.dumps({'status':'PASS','receipt_sha256':sha(args.receipt),
        'artifacts':{name:{'path':str(Path(path).resolve()),'sha256':sha(path)} for name,path in artifacts.items()}},indent=2)+'\n')
    print('PASS: runtime evidence matches',len(artifacts),'deployment binaries')
