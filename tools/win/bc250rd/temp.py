#!/usr/bin/env python3
"""SoC temperature (Tctl) of the BC-250 under Windows, read from the development PC.

    python temp.py [count [interval_s]]

Needs `bc250rd.sys` loaded on the target; its service starts automatically. The address is chosen by
`tools/win/target.py` (wired first, Wi-Fi as fallback). Replaces the earlier `temp.sh`, which carried its own
copy of the ssh line and of the target's address.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import target  # noqa: E402


def main(argv):
    count, interval = (argv + ["1", "1"])[:2]
    t = target.Target()
    remote = t.cfg["work_dir"].rstrip("\\") + "\\bc250rd\\bc250rd_cli.exe"
    done = t.ssh(f'cmd /c ""{remote}" temp {int(count)} {int(interval)}"',
                 timeout=60 + int(count) * int(interval))
    sys.stdout.write(done.stdout)
    sys.stderr.write(done.stderr)
    return done.returncode


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except target.TargetError as e:
        sys.exit(str(e))
