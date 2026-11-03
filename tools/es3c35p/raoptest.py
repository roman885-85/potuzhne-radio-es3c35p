# -*- coding: utf-8 -*-
"""Перевірка AirPlay з Mac: шле радіо звук так, як iPhone, — з мітками RTP, пакетами синхронізації
(0x54, раз на секунду, затримка 2 с), відповідями на запити часу. Звук — тиша з коротким писком
1 кГц кожні 2 с; для кожного писку друкує, коли за годинником Mac його мають почути.

  raoptest.py [адреса радіо] [секунд] [--quiet]

Без шифрування й без ALAC — сирі 16-бітні відліки (L16), радіо їх приймає так само. Під час
перевірки дивитись «airplay» у консолі радіо: синхронізація є, похибка в кілька мілісекунд.
--quiet — без писку (лише тиша): коли поруч хтось є.
"""
import socket, struct, sys, time, threading, random, math

HOST = '192.168.1.240'
DUR = 20.0
QUIET = False
args = [a for a in sys.argv[1:]]
if '--quiet' in args: QUIET = True; args.remove('--quiet')
if args: HOST = args[0]
if len(args) > 1: DUR = float(args[1])

FS, FPP = 44100, 352
LAT = 88200                      # як iTunes: 77175 + 11025 — 2 с
NTP_EPOCH = 2208988800

def ntp(t=None):
    t = time.time() if t is None else t
    t += NTP_EPOCH
    s = int(t)
    return s, int((t - s) * (1 << 32))

def local_ip_to(host):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect((host, 5000)); ip = s.getsockname()[0]; s.close(); return ip

class Rtsp:
    def __init__(self, host):
        self.s = socket.create_connection((host, 5000), timeout=6)
        self.cseq = 0; self.buf = b''
    def req(self, method, url, headers=None, body=b''):
        self.cseq += 1
        h = f"{method} {url} RTSP/1.0\r\nCSeq: {self.cseq}\r\nUser-Agent: AirPlay/raoptest\r\nClient-Instance: 56B29BB6CB904862\r\nDACP-ID: 56B29BB6CB904862\r\nActive-Remote: 1986535575\r\n"
        for k, v in (headers or {}).items(): h += f"{k}: {v}\r\n"
        if body: h += f"Content-Length: {len(body)}\r\n"
        self.s.sendall(h.encode() + b"\r\n" + body)
        while b"\r\n\r\n" not in self.buf: self.buf += self.s.recv(4096)
        head, _, rest = self.buf.partition(b"\r\n\r\n")
        clen = 0
        for ln in head.decode(errors='replace').split("\r\n"):
            if ln.lower().startswith('content-length:'): clen = int(ln.split(':')[1])
        while len(rest) < clen: rest += self.s.recv(4096)
        self.buf = rest[clen:]
        txt = head.decode(errors='replace')
        if ' 200 ' not in txt.split("\r\n")[0]: raise SystemExit(f"{method}: {txt.splitlines()[0]}")
        return txt

def main():
    me = local_ip_to(HOST)
    ctrl = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); ctrl.bind(('', 0))
    tim = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); tim.bind(('', 0))
    aud = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    stop = threading.Event()

    def timing():                     # відповіді на запити часу радіо (0x52 → 0xD3)
        tim.settimeout(0.3)
        while not stop.is_set():
            try: d, a = tim.recvfrom(64)
            except socket.timeout: continue
            if len(d) >= 32 and (d[1] & 0x7F) == 0x52:
                s, f = ntp()
                r = bytearray(32); r[0] = 0x80; r[1] = 0xD3; r[2:4] = d[2:4]
                r[8:16] = d[24:32]; r[16:24] = struct.pack('>II', s, f); r[24:32] = struct.pack('>II', s, f)
                tim.sendto(bytes(r), a)
    threading.Thread(target=timing, daemon=True).start()

    r = Rtsp(HOST)
    sid = random.randint(1, 2**31)
    url = f"rtsp://{me}/{sid}"
    r.req('OPTIONS', '*')
    sdp = (f"v=0\r\no=iTunes {sid} 0 IN IP4 {me}\r\ns=iTunes\r\nc=IN IP4 {HOST}\r\nt=0 0\r\n"
           f"m=audio 0 RTP/AVP 96\r\na=rtpmap:96 L16/44100/2\r\n").encode()
    r.req('ANNOUNCE', url, {'Content-Type': 'application/sdp', 'X-Apple-Client-Name': 'Mac (перевірка синхронізації)'}, sdp)
    tr = r.req('SETUP', url, {'Transport': f"RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;control_port={ctrl.getsockname()[1]};timing_port={tim.getsockname()[1]}"})
    ports = {}
    for part in tr.split('Transport:')[1].split('\r\n')[0].split(';'):
        if '=' in part: k, v = part.strip().split('=', 1); ports[k] = v
    aport, cport = int(ports.get('server_port', 6000)), int(ports.get('control_port', 6001))
    seq0, ts0 = random.randint(0, 65535), random.randint(0, 2**32 - 1)
    r.req('RECORD', url, {'Range': 'npt=0-', 'RTP-Info': f"seq={seq0};rtptime={ts0}"})

    beep = []
    if not QUIET:
        n = int(FS * 0.02)
        beep = [int(8000 * math.sin(2 * math.pi * 1000 * i / FS) * math.sin(math.pi * i / n)) for i in range(n)]
    t0 = time.time() + 0.05           # мить, коли шлемо пакет з міткою ts0
    k = 0; last_sync = -1; ssrc = random.randint(0, 2**32 - 1)
    beeps = []
    print(f"радіо {HOST}: аудіо {aport}, керування {cport}; затримка {LAT} кадрів ({LAT / FS:.2f} с)")
    while True:
        due = t0 + k * FPP / FS
        now = time.time()
        if now - t0 > DUR: break
        if due > now: time.sleep(min(0.004, due - now)); continue
        ts = (ts0 + k * FPP) & 0xFFFFFFFF
        # пакет синхронізації: «зараз звучить ts − затримка», надсилаємо ts — раз на секунду
        sec = int((k * FPP) / FS)
        if sec != last_sync:
            last_sync = sec
            s, f = ntp(due)
            pk = struct.pack('>BBHIIII', 0x90 if k == 0 else 0x80, 0xD4, 7, (ts - LAT) & 0xFFFFFFFF, s, f, ts)
            ctrl.sendto(pk, (HOST, cport))
        # звук: тиша, писк на початку кожної другої секунди
        pcm = bytearray(FPP * 4)
        if beep:
            start = (k * FPP) % (2 * FS)
            for i in range(FPP):
                j = (start + i) % (2 * FS)
                if j < len(beep): struct.pack_into('>hh', pcm, i * 4, beep[j], beep[j])
            # писк починається в цьому пакеті, якщо межа двосекундного циклу — всередині нього
            off = (2 * FS - start) % (2 * FS)
            if off < FPP:
                heard = due + (off + LAT) / FS
                beeps.append(heard)
                print(f"  писк: мітка {(ts + off) & 0xFFFFFFFF}, має звучати о {time.strftime('%H:%M:%S', time.localtime(heard))}.{int(heard * 1000) % 1000:03d}")
        hdr = struct.pack('>BBHII', 0x80, 0xE0 if k == 0 else 0x60, (seq0 + k) & 0xFFFF, ts, ssrc)
        aud.sendto(hdr + bytes(pcm), (HOST, aport))
        k += 1
    try: r.req('TEARDOWN', url)
    except SystemExit: pass
    stop.set()
    print(f"надіслано {k} пакетів ({k * FPP / FS:.1f} с), писків {len(beeps)}")

if __name__ == '__main__':
    main()
