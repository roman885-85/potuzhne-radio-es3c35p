#!/usr/bin/env python3
"""Знімок екрана ES3C35P у PNG: команда «scr» радіо (копія кадру з «тіні»).

  scr.py файл.png [--net [АДРЕСА]] [--keep-shadow]

По USB порт відкривається без DTR (див. con.py); без USB-порту чи з --net —
по Wi-Fi, через консоль налагодження радіо (адреса — як у con.py). Розмір кадру
береться з рядка SCR_BEGIN W H, пікселі — RGB565 по рядку в base64.
"""
import sys, os, time, base64
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import con
from PIL import Image

def main():
    args = sys.argv[1:]
    net = False; host = None; rest = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == '--net':
            net = True
            if i + 1 < len(args) and not args[i + 1].startswith('-') and not args[i + 1].endswith('.png'):
                host = args[i + 1]; i += 1
        elif a.startswith('--net='): net = True; host = a[6:]
        else: rest.append(a)
        i += 1
    out = next((a for a in rest if not a.startswith('--')), 'scr.png')
    link = con.open_link(net, host)
    link.read_for(0.3)
    link.send(b"scr\n")
    buf = link.read_for(60, until='SCR_END')
    if not os.environ.get('KEEP') and '--keep-shadow' not in rest:
        link.send(b"shadow 0\n"); link.read_for(0.3)
    link.close()
    txt = buf.decode('ascii', 'replace')
    i = txt.find('SCR_BEGIN')
    if i < 0: sys.exit('немає SCR_BEGIN — ' + txt[-300:])
    head, _, body = txt[i:].partition('\n')
    _, w, h = head.split()[:3]; w, h = int(w), int(h)
    rows = [l.strip() for l in body.split('\n') if l.strip() and not l.startswith('SCR_END') and not l.startswith('#')]
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
