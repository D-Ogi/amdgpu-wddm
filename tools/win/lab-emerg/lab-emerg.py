#!/usr/bin/env python3
"""Emergency channel to unit A when SSH fails (listener.ps1 on the lab, TCP 8722, HMAC-signed requests).

    python lab-emerg.py status
    python lab-emerg.py restart-sshd
    python lab-emerg.py kill-game
    python lab-emerg.py desktop-cpu                  GPU DWM kill switch at the next boot: DwmForceCpu 1 + cpu request;
                                                     DWM is not restarted (BD-060); then reboot reboot-now, and
                                                     route.py release verify once SSH answers
    python lab-emerg.py kill-dwm accept-bd060        LAST RESORT for a hung compositor: stops DWM; WinUI pointer
                                                     input stays broken until Windows restarts (BD-060)
    python lab-emerg.py ps FILE.ps1 [SECONDS]        run a local script on the lab (job, 120 s default, 600 max)
    python lab-emerg.py ps -c "PowerShell text" [SECONDS]
    python lab-emerg.py get REMOTE LOCAL             fetch a file (16 MB max), SHA-256 checked
    python lab-emerg.py put LOCAL REMOTE             write a file; an existing one keeps its ACL and gets a .bak
    python lab-emerg.py copy|move REMOTE_SRC REMOTE_DST
    python lab-emerg.py delete REMOTE                a file or an empty directory
    python lab-emerg.py mkdir REMOTE_DIR             new directories get Administrators as owner
    python lab-emerg.py list REMOTE_DIR
    python lab-emerg.py acl REMOTE
    python lab-emerg.py tail REMOTE [N]
    python lab-emerg.py usb-boot status|windows|linux:N   steer the diagnostic stick (no restart)
    python lab-emerg.py reboot reboot-now

Key: <BC250_ROOT>\\secrets\\lab-emergency\\key.bin (never printed). Addresses: target.py's configuration, tried in
order. Nothing stays running on this PC.
"""
import base64
import hashlib
import hmac
import json
import os
import secrets
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(os.path.abspath(__file__)).parent
# BC250_ROOT is the workspace root; by default the parent directory of this repository
# (this file is tools/win/lab-emerg/lab-emerg.py, so the repository root is three levels up from here).
ROOT = Path(os.environ.get('BC250_ROOT', str(HERE.parents[2].parent)))
# target.py beside this kit when it sits in the repository, the workspace repository otherwise.
sys.path.insert(0, str(HERE.parent if (HERE.parent / 'target.py').is_file() else ROOT / 'bc250-win/tools/win'))
import target  # noqa: E402

# The key is a credential: it is read, never printed, and never copied into this repository.
KEY = os.environ.get('BC250_LAB_EMERG_KEY', str(ROOT / 'secrets' / 'lab-emergency' / 'key.bin'))
PORT = 8722


def call(action, arg='', body=b'', timeout=150):
    key = open(KEY, 'rb').read()
    arg64 = base64.b64encode(arg.encode('utf-8')).decode('ascii')
    body_hash = hashlib.sha256(body).hexdigest()
    addresses = target.Target().cfg['addresses']
    last = None
    for address in addresses:
        ts = str(int(time.time()))
        nonce = secrets.token_hex(16)
        sig = hmac.new(key, f'{ts}\n{nonce}\n{action}\n{arg64}\n{body_hash}'.encode(), hashlib.sha256).hexdigest()
        req = urllib.request.Request(f'http://{address}:{PORT}/lab/', data=body, method='POST',
                                     headers={'X-Ts': ts, 'X-Nonce': nonce, 'X-Sig': sig, 'X-Action': action,
                                              'X-Arg': arg64, 'Content-Type': 'application/octet-stream'})
        try:
            with urllib.request.urlopen(req, timeout=timeout if address == addresses[0] else 20) as r:
                data = r.read()
                if r.headers.get('Content-Type', '').startswith('application/octet-stream'):
                    return r.status, data, r.headers.get('X-Sha256', '')
                return r.status, json.loads(data.decode('utf-8')), None
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read().decode('utf-8') or '{}'), None
        except OSError as e:
            last = f'{type(e).__name__}: {e}'
            continue
    return None, {'ok': False, 'error': f'no address answered on {PORT}: {last}'}, None


def main(argv):
    if not argv:
        sys.exit(__doc__)
    action, rest = argv[0], argv[1:]
    body, arg = b'', ''
    if action == 'ps':
        if rest and rest[0] == '-c':
            body = rest[1].encode('utf-8'); rest = rest[2:]
        else:
            body = open(rest[0], 'rb').read(); rest = rest[1:]
        arg = rest[0] if rest else ''
    elif action == 'put':
        body = open(rest[0], 'rb').read(); arg = rest[1]
    elif action in ('copy', 'move'):
        arg = f'{rest[0]}|{rest[1]}'
    elif action == 'tail':
        arg = rest[0] + (f'|{rest[1]}' if len(rest) > 1 else '')
    elif action == 'get':
        arg = rest[0]
    elif rest:
        arg = rest[0]
    code, out, digest = call(action, arg, body)
    if action == 'get' and isinstance(out, (bytes, bytearray)):
        if hashlib.sha256(out).hexdigest() != digest.lower():
            print('get: SHA-256 mismatch'); return 1
        with open(rest[1], 'wb') as f:
            f.write(out)
        print(f'get: {len(out)} bytes -> {rest[1]} sha256 {digest.lower()}')
        return 0
    print(json.dumps(out, indent=1))
    return 0 if code == 200 and out.get('ok') else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
