#!/usr/bin/python3
# -*- coding: utf-8 -*-
"""Підставний сервер NTP на Mac — перевірка extras/yoNtpCheck (час від SNTP не береться без другої думки).

Відповідає на запити NTP (UDP 123) часом Mac зі зсувом, як сервер пулу з хибним годинником: 14.11.2026
62.149.0.30 давав час на -44 622 с (−12 год 23 хв 42 с). На радіо: консоль «ntpfake <IP Mac>» — SNTP лише
до цього сервера; «ntpfake» без адреси — назад до налаштувань.

  fakentp.py [--offset С] [--li 0..3] [--stratum N] [--seconds С]
  --offset   зсув часу у відповідях, секунди (типово -44622)
  --li       біт LI (3 — «не синхронізований»), типово 0
  --stratum  страта, типово 2
  --seconds  скільки працювати (типово 600), далі вихід

macOS з Mojave дозволяє слухати порт 123 без root. Брати /usr/bin/python3: python із Homebrew macOS може не
пускати в локальну мережу.
"""
import socket, struct, sys, time

NTP_1900 = 2208988800

def ts(t):
    sec = int(t) + NTP_1900
    frac = int((t - int(t)) * (1 << 32)) & 0xffffffff
    return struct.pack("!II", sec & 0xffffffff, frac)

def main():
    a = sys.argv[1:]
    def opt(name, default):
        return type(default)(a[a.index(name) + 1]) if name in a else default
    offset = opt("--offset", -44622.0)
    li = opt("--li", 0)
    stratum = opt("--stratum", 2)
    seconds = opt("--seconds", 600.0)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", 123))
    s.settimeout(1.0)
    print("fakentp: зсув %+.0f с, LI=%d, страта %d, %d с" % (offset, li, stratum, seconds), flush=True)
    end = time.time() + seconds
    while time.time() < end:
        try:
            req, addr = s.recvfrom(512)
        except socket.timeout:
            continue
        if len(req) < 48 or (req[0] & 7) != 3:
            continue
        now = time.time() + offset
        vn = (req[0] >> 3) & 7 or 4
        head = struct.pack("!BBbb", (li << 6) | (vn << 3) | 4, stratum, req[2] or 6, -20)
        body = struct.pack("!II", 0x100, 0x200) + b"FAKE" + ts(now - 30) + req[40:48] + ts(now) + ts(now)
        s.sendto(head + body, addr)
        lt = time.localtime(now)
        print("%s  запит від %s:%d -> %s" % (time.strftime("%H:%M:%S"), addr[0], addr[1],
              time.strftime("%H:%M:%S %d.%m", lt)), flush=True)

if __name__ == "__main__":
    main()
