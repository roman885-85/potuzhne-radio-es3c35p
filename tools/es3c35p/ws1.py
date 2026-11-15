# -*- coding: utf-8 -*-
"""Одна команда в веб-сокет радіо (/ws) — як зі сторінки — і відповіді за кілька секунд.

  ws1.py [команда=значення] [секунд] [адреса]      напр. ws1.py getindex=1 3, ws1.py gettimezone=1

Затримка перед командою — змінна DELAY (с, типово 0,5). Запускати системним /usr/bin/python3.
"""
import os, sys, socket, base64, struct, time
CMD = (sys.argv[1] if len(sys.argv) > 1 else 'getindex=1').encode()
WAIT = float(sys.argv[2]) if len(sys.argv) > 2 else 4
H = sys.argv[3] if len(sys.argv) > 3 else '192.168.1.240'
s = socket.create_connection((H, 80), timeout=5)
key = base64.b64encode(os.urandom(16)).decode()
s.sendall(('GET /ws HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\n'
           'Sec-WebSocket-Version: 13\r\n\r\n' % (H, key)).encode())
buf = b''
while b'\r\n\r\n' not in buf: buf += s.recv(4096)
head, _, buf = buf.partition(b'\r\n\r\n')
print('рукостискання:', head.split(b'\r\n')[0].decode())
time.sleep(float(os.environ.get('DELAY', '0.5')))
m = os.urandom(4)
s.sendall(bytes([0x81, 0x80 | len(CMD)]) + m + bytes(b ^ m[i % 4] for i, b in enumerate(CMD)))
t0 = time.time(); msgs = []
s.settimeout(0.3)
while time.time() - t0 < WAIT:
    try: d = s.recv(4096)
    except socket.timeout: continue
    if not d: print('радіо закрило'); break
    buf += d
    while len(buf) >= 2:
        op, n, q = buf[0] & 0x0F, buf[1] & 0x7F, 2
        if n == 126: n = struct.unpack('>H', buf[2:4])[0]; q = 4
        if len(buf) < q + n: break
        data = buf[q:q + n]; buf = buf[q + n:]
        msgs.append((round(time.time() - t0, 2), data.decode(errors='replace')))
m = os.urandom(4); s.sendall(bytes([0x88, 0x80]) + m); time.sleep(0.2); s.close()
print('отримано %d кадрів за %.0f с:' % (len(msgs), WAIT))
for t, d in msgs: print('  +%.2f с  %s' % (t, d[:200]))
