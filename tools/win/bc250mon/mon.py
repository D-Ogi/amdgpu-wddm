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
    mon.py windows                      visible top-level windows: handle, process, geometry, title
    mon.py screenshot [--scale 0.5] [--format png|jpg] [--quality 80] [--overlay 0|1]
                      [--window TITLE | --handle 0x...] [--out FILE]
    mon.py scanout [--scale 0.5] [--format png|bmp] [--out FILE]

Screenshots land in P:/BC-250/scratch/screens/<timestamp>.<ext> unless --out says otherwise. Every capture
is written to the monitor's log, so the owner sees on the overlay that a picture was taken.

`scanout` is `screenshot`'s counterpart under the full WDDM table, where GDI's CopyFromScreen reads the CDD's
surfaces and shows black (facts M84): it runs `bc250kmd_cli fbdump` on the target (BC250_ESCAPE_RUN_FBDUMP,
driver/kmd/dcn.c - what the display controller is actually scanning out, not what dxgkrnl thinks it drew),
pulls the BMP back and downsizes it the same way (half scale by default), without needing Pillow or any other
image library on either end.

Which machine and which address: `tools/win/target.py` decides, from the configuration outside this repo.
"""

import base64
import datetime
import json
import os
import struct
import sys
import zlib
from urllib.parse import quote

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import target  # noqa: E402  how to reach the target lives in one place, tools/win/target.py

ROOT = os.environ.get("BC250_ROOT", "P:/BC-250")
API = "http://127.0.0.1:2250"
# tools/win/bc250kmd_cli/README.md: "on the target it lives in C:\BC250\kmd\".
CLI = "C:\\BC250\\kmd\\bc250kmd_cli.exe"
TARGET = None


def _target():
    global TARGET
    if TARGET is None:
        TARGET = target.Target()
    return TARGET


def ssh(ps):
    return _target().run(ps)


def call(method, path, body=None):
    b64 = base64.b64encode(json.dumps(body or {}).encode()).decode()
    out = ssh(f"$b=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('{b64}'));"
              f"$p=@{{Uri='{API}{path}';Method='{method}';UseBasicParsing=$true}};"
              f"if('{method}' -ne 'GET'){{$p.Body=$b;$p.ContentType='application/json'}};"
              "try{(Invoke-WebRequest @p).Content}catch{'{\"error\":\"'+$_.Exception.Message+'\"}'}").strip()
    start = min((i for i in (out.find("{"), out.find("[")) if i >= 0), default=-1)
    try:
        return json.loads(out[start:]) if start >= 0 else {"raw": out}
    except ValueError:
        return {"raw": out}


def image(path):
    """An image from the API. Only text survives the SSH plus PowerShell hop, so the remote side prints the
    pixel size on the first line and the bytes base64-encoded on the second."""
    out = ssh(f"$p=@{{Uri='{API}{path}';UseBasicParsing=$true}};"
              "try{$r=Invoke-WebRequest @p;$r.Headers['X-Capture-Size'];"
              "[Convert]::ToBase64String($r.RawContentStream.ToArray())}"
              "catch{$e=$_;$m=$e.Exception.Message;"
              "try{$m=(New-Object IO.StreamReader($e.Exception.Response.GetResponseStream())).ReadToEnd()}catch{};'ERR '+$m}")
    lines = [l.strip() for l in out.splitlines() if l.strip()]
    if not lines or lines[0].startswith("ERR") or len(lines) < 2:
        sys.exit(out.strip() or "no answer from the monitor")
    return lines[0], base64.b64decode("".join(lines[1:]))


def _read_bmp_top_down(data):
    """Pixel bytes (top-down, BGRA per pixel - the DCN surface's own order), width, height of an uncompressed
    32bpp BI_RGB BMP, the kind bc250kmd_cli fbdump writes (bottom-up, as the format requires)."""
    if data[:2] != b"BM":
        raise ValueError("not a BMP")
    off_bits = struct.unpack_from("<I", data, 10)[0]
    width, height, planes, bits, compression = struct.unpack_from("<iiHHI", data, 18)
    if compression != 0 or bits != 32:
        raise ValueError(f"unsupported BMP: compression {compression}, {bits} bits/pixel")
    top_down, height = height < 0, abs(height)
    pitch = width * 4
    if top_down:
        return data[off_bits:off_bits + pitch * height], width, height
    out = bytearray(pitch * height)
    for y in range(height):
        src = off_bits + (height - 1 - y) * pitch
        out[y * pitch:(y + 1) * pitch] = data[src:src + pitch]
    return bytes(out), width, height


def _encode_bmp(pixels, width, height):
    """The reverse of _read_bmp_top_down: top-down BGRA bytes back into an uncompressed 32bpp BMP file."""
    pitch = width * 4
    header_size = 14 + 40
    file_header = b"BM" + struct.pack("<IHHI", header_size + pitch * height, 0, 0, header_size)
    info_header = struct.pack("<IiiHHIIiiII", 40, width, height, 1, 32, 0, pitch * height, 0, 0, 0, 0)
    body = bytearray(pitch * height)
    for y in range(height):
        body[(height - 1 - y) * pitch:(height - y) * pitch] = pixels[y * pitch:(y + 1) * pitch]
    return file_header + info_header + bytes(body)


def _scale_bgra(pixels, width, height, scale):
    """Nearest-neighbour resample: bc250kmd_cli fbdump has no image library on the target either (it writes
    BMP because that needs none), and pulling a full 1920x1200 raw frame back for every scanout screenshot is
    not the "reader pays per image token" deal mon.py's other screenshots keep (see its docstring). Good enough
    for a debug picture; the GDI path's bicubic PNG/JPEG stays the polished one."""
    if scale >= 1.0:
        return pixels, width, height
    new_w, new_h = max(1, round(width * scale)), max(1, round(height * scale))
    pitch, new_pitch = width * 4, new_w * 4
    out = bytearray(new_pitch * new_h)
    for y in range(new_h):
        src_row = (min(height - 1, (y * height) // new_h)) * pitch
        dst_row = y * new_pitch
        for x in range(new_w):
            sx = min(width - 1, (x * width) // new_w) * 4
            out[dst_row + x * 4:dst_row + x * 4 + 4] = pixels[src_row + sx:src_row + sx + 4]
    return bytes(out), new_w, new_h


def _encode_png(bgra, width, height):
    """Minimal 8-bit RGBA PNG (colour type 6, no interlace), pure stdlib (zlib + struct): see _scale_bgra on
    why this exists instead of pulling in Pillow for one command."""
    def chunk(tag, payload):
        c = tag + payload
        return struct.pack(">I", len(payload)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    pitch = width * 4
    raw = bytearray()
    for y in range(height):
        row = bgra[y * pitch:(y + 1) * pitch]
        rgba = bytearray(pitch)
        rgba[0::4], rgba[1::4], rgba[2::4], rgba[3::4] = row[2::4], row[1::4], row[0::4], row[3::4]
        raw.append(0)          # filter type 0 (none): a debug picture, not worth the CPU a real filter costs
        raw += rgba
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    idat = zlib.compress(bytes(raw), 6)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")


def flags(args, names):
    """--name value pairs. Everything this script passes on is a value, so nothing needs to be a bare switch."""
    out = {}
    for i in range(0, len(args), 2):
        name = args[i][2:]
        if not args[i].startswith("--") or name not in names or i + 1 >= len(args):
            sys.exit(f"bad option {args[i]}, expected one of " + ", ".join("--" + n for n in names))
        out[name] = args[i + 1]
    return out


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
    elif cmd == "windows":
        found = call("GET", "/windows")
        if not isinstance(found, list):
            sys.exit(str(found))
        for w in found:
            geometry = f"{w['width']}x{w['height']}+{w['x']}+{w['y']}"
            print(f"{w['handle']}  {w['process']:<20} {geometry:<20} {w['title']}{'  (minimized)' if w['minimized'] else ''}")
    elif cmd == "screenshot":
        o = flags(args, ("scale", "format", "quality", "overlay", "window", "handle", "out"))
        fmt = o.get("format", "png")
        query = f"scale={o.get('scale', '0.5')}&format={fmt}&quality={o.get('quality', '80')}&overlay={o.get('overlay', '1')}"
        if "handle" in o:
            path = f"/screenshot/window?handle={o['handle']}&{query}"
        elif "window" in o:
            path = f"/screenshot/window?title={quote(o['window'])}&{query}"
        else:
            path = f"/screenshot?{query}"
        size, data = image(path)
        out = o.get("out") or os.path.join(ROOT, "scratch", "screens",
                                           datetime.datetime.now().strftime("%Y%m%d-%H%M%S") + "." + fmt)
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
        with open(out, "wb") as f:
            f.write(data)
        print(f"{os.path.abspath(out)}   {size} px   {len(data)} bytes")
    elif cmd == "scanout":
        o = flags(args, ("scale", "format", "out"))
        scale = float(o.get("scale", "0.5"))
        fmt = o.get("format", "png")
        if fmt not in ("png", "bmp"):
            sys.exit(f"scanout format is png or bmp, not {fmt}")
        t = _target()
        remote_bmp = t.cfg["tmp_dir"].rstrip("\\") + "\\fbdump.bmp"
        # cmd's double-double-quote around the exe path, same as temp.py's line: the transport mon.py already
        # uses for a command on the target, not the overlay's own HTTP API - bc250kmd_cli is a separate tool.
        done = t.ssh(f'cmd /c ""{CLI}" fbdump "{remote_bmp}""')
        text = (done.stdout or "") + (done.stderr or "")
        if done.returncode != 0:
            sys.exit(text.strip() or f"bc250kmd_cli fbdump failed, exit {done.returncode}")
        sys.stdout.write(done.stdout)
        local_bmp = os.path.join(ROOT, "scratch", "screens",
                                 datetime.datetime.now().strftime("%Y%m%d-%H%M%S") + "-scanout-raw.bmp")
        t.pull(remote_bmp, local_bmp)
        with open(local_bmp, "rb") as f:
            raw_bmp = f.read()
        os.remove(local_bmp)     # only the downscaled picture below is worth keeping
        pixels, width, height = _read_bmp_top_down(raw_bmp)
        pixels, width, height = _scale_bgra(pixels, width, height, scale)
        data = _encode_png(pixels, width, height) if fmt == "png" else _encode_bmp(pixels, width, height)
        out = o.get("out") or os.path.join(ROOT, "scratch", "screens",
                                           datetime.datetime.now().strftime("%Y%m%d-%H%M%S") + "-scanout." + fmt)
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
        with open(out, "wb") as f:
            f.write(data)
        print(f"{os.path.abspath(out)}   {width}x{height} px   {len(data)} bytes")
    elif cmd == "stop?":
        stop = call("GET", "/flags").get("stop", False)
        print("STOP requested" if stop else "no stop request")
        sys.exit(1 if stop else 0)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except target.TargetError as e:
        sys.exit(str(e))
