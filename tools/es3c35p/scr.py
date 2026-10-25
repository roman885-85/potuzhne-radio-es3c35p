#!/usr/bin/env python3
"""Знімок екрана ES3C35P у PNG: команда «scr» радіо (копія кадру з «тіні»).

  scr.py файл.png [--keep-shadow]

Порт відкривається без DTR (див. con.py). Розмір кадру береться з рядка
SCR_BEGIN W H, пікселі — RGB565 по рядку в base64.
"""
import sys, os, time, base64, select
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import con
from PIL import Image

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else 'scr.png'
    fd = con.open_port(con.port())
    con.read_for(fd, 0.3)
    os.write(fd, b"scr\n")
    buf = b''; end = time.time() + 60
    while time.time() < end and b'SCR_END' not in buf:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try: buf += os.read(fd, 65536)
            except OSError: pass
    if not os.environ.get('KEEP') and '--keep-shadow' not in sys.argv:
        os.write(fd, b"shadow 0\n"); con.read_for(fd, 0.3)
    os.close(fd)
    txt = buf.decode('ascii', 'replace')
    i = txt.find('SCR_BEGIN')
    if i < 0: sys.exit('немає SCR_BEGIN — ' + txt[-300:])
    head, _, rest = txt[i:].partition('\n')
    _, w, h = head.split()[:3]; w, h = int(w), int(h)
    rows = [l.strip() for l in rest.split('\n') if l.strip() and not l.startswith('SCR_END') and not l.startswith('#')]
    img = Image.new('RGB', (w, h))
    px = img.load()
    for y, l in enumerate(rows[:h]):
        try: d = base64.b64decode(l)
        except Exception: continue
        for x in range(min(w, len(d) // 2)):
            v = d[2 * x] | (d[2 * x + 1] << 8)
            px[x, y] = (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)
    img.save(out)
    print(f'{out}: {w}x{h}, рядків {len(rows)}')

if __name__ == '__main__':
    main()
