# -*- coding: utf-8 -*-
"""Навантаження веб-сокета радіо — перевірка гонки AsyncWebSocket (дамп 06.11.2026: assert у multi_heap_free ←
textAll ← NetServer::processQueue). Як сторінка радіо: один клієнт тримає /ws увесь час, ще кілька раз у раз
підключаються, просять стан (getindex=1 — радіо відповідає кожному окремо, ~10 повідомлень) і рвуть з'єднання
без кадру закриття (FIN або RST); /api/state — раз на секунду. Звук і налаштування не змінюються.
Перезапуск радіо видно за «up» (час роботи) у /api/state.

  wsstress.py [адреса] [секунд] [клієнтів, що рвуть] [--далі]

Типово навантаження зупиняється на першому перезапуску радіо (його хтось може слухати); --далі — не зупинятись.

Запускати системним /usr/bin/python3 (python.org-Python у локальну мережу macOS не пускають).
"""
import os, sys, time, socket, struct, random, threading, json, base64, urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 else '192.168.1.240'
DUR = float(sys.argv[2]) if len(sys.argv) > 2 else 600
NCHURN = int(sys.argv[3]) if len(sys.argv) > 3 else 3
STOP_ON_REBOOT = '--далі' not in sys.argv   # типово — стоп на першому перезапуску: радіо хтось слухає
stop = threading.Event()
lock = threading.Lock()
st = dict(conn=0, connFail=0, rst=0, fin=0, msgChurn=0, msgHold=0, holdDrops=0, pollOk=0, pollFail=0,
          reboots=0, heapMin=None, heapMax=None, blkMin=None, up=None, build='')

def add(k, v=1):
    with lock: st[k] += v

def log(m):
    print(time.strftime('%H:%M:%S'), m, flush=True)

def ws_open(timeout=5):
    s = socket.create_connection((HOST, 80), timeout=timeout)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall(('GET /ws HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
               'Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n' % (HOST, key)).encode())
    buf = b''
    while b'\r\n\r\n' not in buf:
        d = s.recv(4096)
        if not d: raise ConnectionError('радіо закрило до відповіді')
        buf += d
    head, _, rest = buf.partition(b'\r\n\r\n')
    if b' 101 ' not in head.split(b'\r\n')[0]: raise ConnectionError(head.split(b'\r\n')[0].decode(errors='replace'))
    return s, rest

def ws_frame(op, payload):
    m = os.urandom(4)
    n = len(payload)
    hdr = bytes([0x80 | op]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack('>H', n))
    return hdr + m + bytes(b ^ m[i % 4] for i, b in enumerate(payload))

def ws_read(s, buf, until, counter):
    """Читати кадри до моменту until; повертає (буфер, чи живе з'єднання). На ping — pong."""
    while time.time() < until and not stop.is_set():
        s.settimeout(max(0.05, min(0.5, until - time.time())))
        try: d = s.recv(4096)
        except socket.timeout: continue
        if not d: return buf, False
        buf += d
        while len(buf) >= 2:
            op, n, p = buf[0] & 0x0F, buf[1] & 0x7F, 2
            if n == 126:
                if len(buf) < 4: break
                n = struct.unpack('>H', buf[2:4])[0]; p = 4
            elif n == 127:
                if len(buf) < 10: break
                n = struct.unpack('>Q', buf[2:10])[0]; p = 10
            if len(buf) < p + n: break
            data = buf[p:p + n]; buf = buf[p + n:]
            if op == 0x9: s.sendall(ws_frame(0xA, data))
            elif op == 0x8: return buf, False
            elif op in (0x1, 0x2): add(counter)
    return buf, True

def holder():
    while not stop.is_set():
        try:
            s, buf = ws_open()
            s.sendall(ws_frame(0x1, b'getindex=1'))
            while not stop.is_set():
                buf, ok = ws_read(s, buf, time.time() + 1.0, 'msgHold')
                if not ok: break
            s.close()
            if not stop.is_set(): add('holdDrops'); log('постійний клієнт: обрив, знову за 2,5 с')
        except Exception as e:
            if stop.is_set(): break
            add('holdDrops'); log('постійний клієнт: %s' % e)
        stop.wait(2.5)

def churn():
    while not stop.is_set():
        try:
            s, buf = ws_open()
            add('conn')
            s.sendall(ws_frame(0x1, b'getindex=1'))
            ws_read(s, buf, time.time() + random.uniform(0.0, 1.2), 'msgChurn')
            if random.random() < 0.5:
                s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0)); add('rst')
            else: add('fin')
            s.close()
        except Exception:
            add('connFail'); stop.wait(1.0)
        stop.wait(random.uniform(0.0, 0.3))

def poll():
    while not stop.is_set():
        t = time.time()
        try:
            with urllib.request.urlopen('http://%s/api/state' % HOST, timeout=4) as r: j = json.loads(r.read().decode())
            add('pollOk')
            with lock:
                up = j.get('up', 0)
                if st['up'] is not None and up + 5 < st['up']:
                    st['reboots'] += 1; log('ПЕРЕЗАПУСК: up %s → %s, rst=%s' % (st['up'], up, j.get('rst')))
                    if STOP_ON_REBOOT: log('зупиняю навантаження — радіо вже раз перезапустилось'); stop.set()
                st['up'] = up; st['build'] = j.get('build', '')
                h, b = j.get('heap', 0), j.get('heapBlk', 0)
                st['heapMin'] = h if st['heapMin'] is None else min(st['heapMin'], h)
                st['heapMax'] = h if st['heapMax'] is None else max(st['heapMax'], h)
                st['blkMin'] = b if st['blkMin'] is None else min(st['blkMin'], b)
        except Exception as e:
            add('pollFail')
        stop.wait(max(0.0, 1.0 - (time.time() - t)))

def summary(tag):
    with lock: d = dict(st)
    log('%s: підключень %d (RST %d, FIN %d, не вдалось %d), повідомлень %d + постійному %d, обривів постійного %d, '
        '/api/state %d/%d, перезапусків %d, heap %s…%s, найбільший шматок ≥ %s, up %s, збірка %s' % (
        tag, d['conn'], d['rst'], d['fin'], d['connFail'], d['msgChurn'], d['msgHold'], d['holdDrops'],
        d['pollOk'], d['pollOk'] + d['pollFail'], d['reboots'], d['heapMin'], d['heapMax'], d['blkMin'], d['up'], d['build']))

if __name__ == '__main__':
    log('радіо %s, %d с, клієнтів, що рвуть: %d' % (HOST, DUR, NCHURN))
    th = [threading.Thread(target=f, daemon=True) for f in [holder, poll] + [churn] * NCHURN]
    for t in th: t.start()
    t0 = time.time(); nxt = t0 + 30
    try:
        while time.time() - t0 < DUR and not stop.is_set():
            time.sleep(0.5)
            if time.time() >= nxt: summary('%3d с' % (time.time() - t0)); nxt += 30
    except KeyboardInterrupt: pass
    stop.set()
    for t in th: t.join(timeout=3)
    summary('ПІДСУМОК')
    sys.exit(1 if st['reboots'] else 0)
