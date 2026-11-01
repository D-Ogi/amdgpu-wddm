#!/usr/bin/env python3
"""Display lane L8 (GOP half): read named DCN registers through BAR5 before amdgpu is loaded, reads only.

The firmware (GOP) state is what Windows inherits; E21 read 75 of these registers this way (gen_dmu_min.py set =
gop-a.regs). Every other name here is new to a raw read, so the tool reports over stdout as it goes, the way
tools/diagusb/payload/bc250/sweep_stream.py does: '? NAME 0xOFF' is printed and flushed, then a 4 ms pause, then
the read, then 'NAME 0xOFF 0xVALUE'. If the machine hangs, the last '?' line names the register. Run it through
stream.py on the development PC so that line survives the hang.

Usage: bar5dump.py [--stop-before NAME] [--out FILE] <table> [table...]
  --stop-before NAME   end the run before this register (rerun after a hang, skipping the culprit and the rest)
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dcnregs  # noqa: E402

PAUSE_S = 0.004


def main(argv):
    stop, outp, tables = None, None, []
    i = 0
    while i < len(argv):
        if argv[i] == "--stop-before":
            stop, i = argv[i + 1], i + 2
        elif argv[i] == "--out":
            outp, i = argv[i + 1], i + 2
        else:
            tables.append(argv[i])
            i += 1
    if not tables:
        sys.exit(__doc__)
    out = open(outp, "w") if outp else None

    def emit(s):
        sys.stdout.write(s + "\n")
        sys.stdout.flush()
        if out:
            out.write(s + "\n")
            out.flush()

    try:
        bar = dcnregs.Bar5()
    except OSError as e:
        emit("REFUSED %s" % e)
        return 3
    emit("# bar5dump %s bar5_size=0x%X tables=%s" % (dcnregs.stamp(), bar.size,
                                                     ",".join(os.path.basename(t) for t in tables)))
    n = 0
    for t in tables:
        emit("# table %s" % os.path.basename(t))
        for name, o in dcnregs.load_table(t):
            if stop and name == stop:
                emit("# stopped before %s (--stop-before)" % name)
                emit("# end reads=%d %s" % (n, dcnregs.stamp()))
                return 0
            emit("? %s 0x%05X" % (name, o))
            time.sleep(PAUSE_S)
            v = bar.r(name)
            emit("%-56s 0x%05X 0x%08X" % (name, o, v))
            n += 1
    emit("# end reads=%d %s" % (n, dcnregs.stamp()))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
