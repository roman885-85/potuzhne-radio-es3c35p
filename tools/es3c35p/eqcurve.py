# -*- coding: utf-8 -*-
"""АЧХ і спотворення ланцюга обробки радіо (extras/yoDsp.cpp) — ті самі формули, на Mac.

  eqcurve.py [--state state.json] [--vol N] [--png файл.png]

Без --state бере налаштування власника на 03.11.2026. Друкує АЧХ по смугах, а для
віртуального басу — гармоніки й інтермодуляцію на тонах басу (чому «рипить»).
"""
import sys, json, math
import numpy as np

BAND_HZ = [31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000]
FS = 44100.0

def peak(fs, f, Q, db):
    A = 10 ** (db / 40); w = 2 * math.pi * f / fs; c = math.cos(w); al = math.sin(w) / (2 * Q)
    a0 = 1 + al / A
    return [(1 + al * A) / a0, -2 * c / a0, (1 - al * A) / a0, -2 * c / a0, (1 - al / A) / a0]

def lowshelf(fs, f, db, S=1.0):
    A = 10 ** (db / 40); w = 2 * math.pi * f / fs; c = math.cos(w); s = math.sin(w)
    al = s / 2 * math.sqrt((A + 1 / A) * (1 / S - 1) + 2); sA = 2 * math.sqrt(A) * al
    a0 = (A + 1) + (A - 1) * c + sA
    return [A * ((A + 1) - (A - 1) * c + sA) / a0, 2 * A * ((A - 1) - (A + 1) * c) / a0,
            A * ((A + 1) - (A - 1) * c - sA) / a0, -2 * ((A - 1) + (A + 1) * c) / a0, ((A + 1) + (A - 1) * c - sA) / a0]

def highshelf(fs, f, db, S=1.0):
    A = 10 ** (db / 40); w = 2 * math.pi * f / fs; c = math.cos(w); s = math.sin(w)
    al = s / 2 * math.sqrt((A + 1 / A) * (1 / S - 1) + 2); sA = 2 * math.sqrt(A) * al
    a0 = (A + 1) - (A - 1) * c + sA
    return [A * ((A + 1) + (A - 1) * c + sA) / a0, -2 * A * ((A - 1) + (A + 1) * c) / a0,
            A * ((A + 1) + (A - 1) * c - sA) / a0, 2 * ((A - 1) - (A + 1) * c) / a0, ((A + 1) - (A - 1) * c - sA) / a0]

def hpf(fs, f, Q):
    w = 2 * math.pi * f / fs; c = math.cos(w); al = math.sin(w) / (2 * Q); a0 = 1 + al
    return [(1 + c) / 2 / a0, -(1 + c) / a0, (1 + c) / 2 / a0, -2 * c / a0, (1 - al) / a0]

def lpf(fs, f, Q):
    w = 2 * math.pi * f / fs; c = math.cos(w); al = math.sin(w) / (2 * Q); a0 = 1 + al
    return [(1 - c) / 2 / a0, (1 - c) / a0, (1 - c) / 2 / a0, -2 * c / a0, (1 - al) / a0]

def mag_db(q, f, fs=FS):
    b0, b1, b2, a1, a2 = q
    z = np.exp(-1j * 2 * np.pi * np.asarray(f) / fs)
    return 20 * np.log10(np.abs((b0 + b1 * z + b2 * z * z) / (1 + a1 * z + a2 * z * z)))

def run(q, x):
    from scipy.signal import lfilter
    b0, b1, b2, a1, a2 = q
    return lfilter([b0, b1, b2], [1, a1, a2], x)

def build_old(s, vol):
    """Ланки як у _build() 02.11.2026 (до переробки низу)."""
    q = []; speaker = s['dac'] in (0, 3)
    if speaker and s['guard']:
        f = 160.0 if s['guard'] >= 2 else 100.0
        q += [hpf(FS, f, 0.5412), hpf(FS, f, 1.3066)] if s['guard'] >= 2 else [hpf(FS, f, 0.7071)]
    for b in range(10):
        db = (s['eq'][b] if s['on'] else 0) + (s['room'][b] if s['roomOn'] and s['roomDac'] == s['dac'] else 0)
        db = max(-18, min(18, db))
        if abs(db) >= 0.25: q.append(peak(FS, BAND_HZ[b], 1.2, db))
    if s['loud']:
        att = max(0.0, -20 * math.log10(vol / 254)) if vol > 0 else 48.0
        strong = s['loud'] >= 2
        lo = min(att * (0.40 if strong else 0.25), 12.0 if strong else 8.0)
        hi = min(att * (0.15 if strong else 0.10), 5.0 if strong else 3.0)
        q.append(lowshelf(FS, 180.0 if speaker else 100.0, lo)); q.append(highshelf(FS, 8000.0, hi))
    return q

def vb_old(s, x):
    """Гілка віртуального басу 02.11.2026: НЧ 4-го порядку → |x| → ВЧ 1,2f → НЧ 4f → ×K."""
    if not s['vb']: return np.zeros_like(x)
    f = (160.0 if s['guard'] >= 2 else 100.0) if (s['dac'] == 0 and s['guard']) else 120.0
    m = run(lpf(FS, f, 1.3066), run(lpf(FS, f, 0.5412), x))
    m = np.abs(m)
    m = run(lpf(FS, f * 4, 0.7071), run(hpf(FS, f * 1.2, 0.7071), m))
    return m * [0, 0.9, 1.6, 2.6][s['vb']]

def tone_spectrum(sig, freqs):
    n = len(sig); w = np.hanning(n); sp = np.abs(np.fft.rfft(sig * w)) / (n / 4)
    fr = np.fft.rfftfreq(n, 1 / FS)
    return {f: 20 * math.log10(max(1e-9, sp[np.argmin(np.abs(fr - f))])) for f in freqs}

OWNER = {"on": 1, "eq": [6, -1, -3, -1, 3, -3, 0, 2, 2, -1], "room": [0, 6, -7, -8, 0, 5, 1, -6, 4, 6],
         "roomOn": 1, "roomDac": 1, "dac": 1, "loud": 2, "guard": 2, "vb": 3}

if __name__ == '__main__':
    a = sys.argv[1:]; s = dict(OWNER); vol = 25
    if '--state' in a:
        d = json.load(open(a[a.index('--state') + 1])); snd = d['snd']
        s = {"on": snd['on'], "eq": snd['eq'], "room": snd['room'], "roomOn": snd['roomOn'], "roomDac": snd['roomDac'],
             "dac": d['dev']['dac'], "loud": snd['loud'], "guard": snd['guard'], "vb": snd['vb']}
        vol = d.get('vol', vol)
    if '--vol' in a: vol = int(a[a.index('--vol') + 1])
    q = build_old(s, vol)
    fr = [20, 25, 31, 40, 50, 62, 80, 100, 125, 160, 200, 250, 315, 400, 500, 1000, 2000, 4000, 8000, 16000]
    tot = sum(mag_db(x, fr) for x in q)
    print("налаштування:", s, "гучність", vol)
    print("АЧХ (без віртуального басу):")
    for f, v in zip(fr, tot): print("  %6d Гц %+6.1f дБ" % (f, v))
    # віртуальний бас на одному тоні 60 Гц і на двох (55 + 82 Гц)
    t = np.arange(int(FS * 2)) / FS
    for name, x in (("тон 60 Гц, −12 дБ", 0.25 * np.sin(2 * math.pi * 60 * t)),
                    ("тони 55+82 Гц, по −15 дБ", 0.178 * (np.sin(2 * math.pi * 55 * t) + np.sin(2 * math.pi * 82 * t)))):
        vb = vb_old(s, x)
        y = x + vb
        for st in q: y = run(st, y)
        seg = y[int(FS):]
        ff = [55, 60, 82, 27, 110, 120, 137, 164, 192, 219, 240, 246, 274, 300, 360, 410]
        sp = tone_spectrum(seg, ff)
        base = max(sp[f] for f in (55, 60, 82))
        print(f"{name}: після ланцюга, дБ відносно найгучнішого тону:")
        print("   " + "  ".join("%d:%+.0f" % (f, sp[f] - base) for f in ff))

# ---------------- нова обробка низу (03.11.2026) ----------------
NEW = dict(qlow=0.9, room_min_hz=100, room_lim_lo=4.0, room_lim=6.0, sub_hz=25.0, cap=10.0,
           vb_k=[0, 0.5, 0.9, 1.4], vb_post_hz=1.25)

def room_applied(s, P=NEW):
    """Поправка під кімнату, яку справді ставимо: без смуг, де мікрофон плати не міряє (≤ 62 Гц),
    згладжена з сусідами і обмежена (низ — ±4 дБ, решта — ±6 дБ)."""
    r = [float(v) if BAND_HZ[i] >= P['room_min_hz'] else 0.0 for i, v in enumerate(s['room'])]
    out = []
    for i in range(10):
        if BAND_HZ[i] < P['room_min_hz']: out.append(0.0); continue
        a = r[i - 1] if i > 0 and BAND_HZ[i - 1] >= P['room_min_hz'] else r[i]
        b = r[i + 1] if i < 9 else r[i]
        v = 0.25 * a + 0.5 * r[i] + 0.25 * b
        lim = P['room_lim_lo'] if BAND_HZ[i] < 500 else P['room_lim']
        out.append(max(-lim, min(lim, v)))
    return out

def build_new(s, vol, P=NEW):
    q = []; speaker = s['dac'] in (0, 3)
    if speaker and s['guard']:
        f = 160.0 if s['guard'] >= 2 else 100.0
        q += [hpf(FS, f, 0.5412), hpf(FS, f, 1.3066)] if s['guard'] >= 2 else [hpf(FS, f, 0.7071)]
    elif P['sub_hz']:
        q.append(hpf(FS, P['sub_hz'], 0.7071))            # інфразвук: динамік лише «бовтається»
    room = room_applied(s, P) if s['roomOn'] and s['roomDac'] == s['dac'] else [0] * 10
    db = [max(-18, min(18, (s['eq'][b] if s['on'] else 0) + room[b])) for b in range(10)]
    lo = hi = 0.0
    if s['loud']:
        att = max(0.0, -20 * math.log10(vol / 254)) if vol > 0 else 48.0
        strong = s['loud'] >= 2
        lo = min(att * (0.40 if strong else 0.25), 12.0 if strong else 8.0)
        hi = min(att * (0.15 if strong else 0.10), 5.0 if strong else 3.0)
    # стеля сумарного підйому низу: підйоми смуг ≤125 Гц і тонкомпенсації стискаємо разом
    k = 1.0
    for _ in range(6):
        qq = list(q)
        for b in range(10):
            d = db[b] * (k if (db[b] > 0 and BAND_HZ[b] <= 125) else 1.0)
            if abs(d) >= 0.25: qq.append(peak(FS, BAND_HZ[b], P['qlow'] if BAND_HZ[b] <= 125 else 1.2, d))
        if s['loud']:
            qq.append(lowshelf(FS, 180.0 if speaker else 100.0, lo * k, 0.7)); qq.append(highshelf(FS, 8000.0, hi))
        fr = np.geomspace(30, 160, 24)
        m = max(sum(mag_db(x, fr) for x in qq))
        if m <= P['cap'] + 0.2: break
        k *= max(0.3, P['cap'] / m)
    return qq

def vb_new(s, x, P=NEW):
    if not s['vb']: return np.zeros_like(x)
    f = (160.0 if s['guard'] >= 2 else 100.0) if (s['dac'] == 0 and s['guard']) else 120.0
    m = run(lpf(FS, f, 1.3066), run(lpf(FS, f, 0.5412), x))
    m = run(hpf(FS, 40.0, 0.7071), m)                      # вужча смуга: менше різницевих тонів
    m = np.abs(m)
    fh = f * P['vb_post_hz']
    m = run(hpf(FS, fh, 1.3066), run(hpf(FS, fh, 0.5412), m))   # 4-й порядок: гул різниці не проходить
    m = run(lpf(FS, f * 4, 0.7071), m)
    return m * P['vb_k'][s['vb']]

def report(s, vol, build, vbf, after_eq, title):
    q = build(s, vol)
    fr = [20, 25, 31, 40, 50, 62, 80, 100, 125, 160, 200, 250, 315, 400, 500, 1000]
    tot = sum(mag_db(x, fr) for x in q)
    print(title)
    print("   " + "  ".join("%d:%+.1f" % (f, v) for f, v in zip(fr, tot)))
    t = np.arange(int(FS * 2)) / FS
    x = 0.178 * (np.sin(2 * math.pi * 55 * t) + np.sin(2 * math.pi * 82 * t))
    vb = vbf(s, x)
    if after_eq:
        y = x
        for st in q: y = run(st, y)
        y = y + vb
    else:
        y = x + vb
        for st in q: y = run(st, y)
    sp = tone_spectrum(y[int(FS):], [55, 82, 27, 110, 137, 164, 192, 219, 246, 274, 301, 328, 410])
    base = max(sp[55], sp[82])
    print("   55+82 Гц: " + "  ".join("%d:%+.0f" % (f, sp[f] - base) for f in [27, 110, 137, 164, 192, 219, 246, 274, 301, 328, 410]))

if __name__ == '__main__' and '--compare' in sys.argv:
    s = dict(OWNER); vol = 25
    report(s, vol, build_old, vb_old, False, "БУЛО (02.11):")
    report(s, vol, build_new, vb_new, True, "СТАЛО:")
    print("поправка під кімнату: виміряно", s['room'], "→ ставимо", [round(v, 1) for v in room_applied(s)])
