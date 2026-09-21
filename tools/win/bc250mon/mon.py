#!/usr/bin/env python3
"""Talk to bc250mon on the BC-250 from the development PC. The API listens on the target's loopback only, so
every call goes through SSH; the JSON body travels base64-encoded to stay clear of shell quoting.

    mon.py state
    mon.py status "reading GC block" [info|good|warn|error]
    mon.py log "text" [level]
    mon.py panel e02 "E02 control read" "registers=5542" "identical=5074:good" "hangs=0:good"
    mon.py unpanel e02
    mon.py action clock.cool            mon.py action clock.set '{"mhz": 1200, "mv": 850}'
    mon.py stop?                        exit code 1 if the owner asked to stop
"""

import base64
import json
import os
import subprocess
import sys

ROOT = os.environ.get("BC250_ROOT", "P:/BC-250")
HOST = os.environ.get("BC250_WIN_HOST", "bc250@192.168.69.41")
API = "http://127.0.0.1:2250"


def call(method, path, body=None):
    b64 = base64.b64encode(json.dumps(body or {}).encode()).decode()
    ps = (f"$b=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('{b64}'));"
          f"$p=@{{Uri='{API}{path}';Method='{method}';UseBasicParsing=$true}};"
          f"if('{method}' -ne 'GET'){{$p.Body=$b;$p.ContentType='application/json'}};"
          "try{(Invoke-WebRequest @p).Content}catch{'{\"error\":\"'+$_.Exception.Message+'\"}'}")
    cmd = ["ssh", "-i", f"{ROOT}/secrets/client/bc250diag_ed25519", "-o", "BatchMode=yes", "-o", "IdentitiesOnly=yes",
           "-o", "ConnectTimeout=10", "-o", f"UserKnownHostsFile={ROOT}/secrets/client/known_hosts_win", HOST,
           "powershell -NoProfile -EncodedCommand " + base64.b64encode(ps.encode("utf-16-le")).decode()]
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=60).stdout.strip()
    start = min((i for i in (out.find("{"), out.find("[")) if i >= 0), default=-1)
    try:
        return json.loads(out[start:]) if start >= 0 else {"raw": out}
    except ValueError:
        return {"raw": out}


def main(argv):
    if not argv:
        sys.exit(__doc__)
    cmd, args = argv[0], argv[1:]
    level = lambda i: args[i] if len(args) > i else "info"
    if cmd == "state":
        s = call("GET", "/state")
        print(f"status: {s.get('status')}   stop: {s.get('stop')}")
        for p in s.get("panels", []):
            print(f"[{p['title']}]")
            for r in p["rows"]:
                print(f"  {r[0]:<14} {r[1]}")
        for l in s.get("log", [])[-10:]:
            print(f"  {l['time'][11:]} {l['level']:<5} {l['source']}: {l['text']}")
    elif cmd == "status":
        print(call("POST", "/status", {"text": args[0], "level": level(1)}))
    elif cmd == "log":
        print(call("POST", "/log", {"text": args[0], "level": level(1), "source": "claude"}))
    elif cmd == "panel":
        rows = []
        for item in args[2:]:
            label, _, rest = item.partition("=")
            value, _, lvl = rest.rpartition(":") if rest.rsplit(":", 1)[-1] in ("info", "good", "warn", "error") else (rest, "", "info")
            rows.append([label, value, lvl])
        print(call("PUT", f"/panel/{args[0]}", {"title": args[1], "rows": rows}))
    elif cmd == "unpanel":
        print(call("DELETE", f"/panel/{args[0]}"))
    elif cmd == "action":
        print(call("POST", f"/action/{args[0]}", json.loads(args[1]) if len(args) > 1 else {}))
    elif cmd == "stop?":
        stop = call("GET", "/flags").get("stop", False)
        print("STOP requested" if stop else "no stop request")
        sys.exit(1 if stop else 0)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
