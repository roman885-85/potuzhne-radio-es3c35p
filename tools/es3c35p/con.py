#!/usr/bin/env python3
"""Консоль ES3C35P по USB Serial/JTAG — без перемикання DTR.

На ESP32-S3 з рідним USB піднятий DTR засувує GPIO0 у нуль, і наступне
скидання йде в завантажувач. Тому порт відкриваємо з DTR=0 і RTS=0 і їх не
чіпаємо; перезапуск (--reset) — лише RTS 0→1→0 при DTR=0.

  con.py [--reset] [--wait С] [--listen С] "команда" ["команда" ...]
  --listen С — просто слухати порт стільки секунд (нічого не посилаючи)
"""
import os, sys, time, termios, fcntl, struct, select, glob

TIOCMBIS, TIOCMBIC = 0x8004746d, 0x8004746b
DTR, RTS = 0x002, 0x004

def port():
    p = os.environ.get("PORT")
    if p: return p
    c = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not c: sys.exit("порт /dev/cu.usbmodem* не знайдено")
    return c[0]

def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd); a[0] = a[1] = a[3] = 0
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL; a[4] = a[5] = termios.B115200
    a[6][termios.VMIN] = 0; a[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, a)
    fcntl.ioctl(fd, TIOCMBIC, struct.pack('I', DTR | RTS))
    return fd

def read_for(fd, secs, until=None):
    out = b''; end = time.time() + secs
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try: out += os.read(fd, 65536)
            except OSError: pass
            if until and until.encode() in out: break
    return out

def main():
    args = sys.argv[1:]; wait = 3.0; reset = False; cmds = []; listen = 0.0
    i = 0
    while i < len(args):
        if args[i] == '--reset': reset = True
        elif args[i] == '--wait': wait = float(args[i + 1]); i += 1
        elif args[i] == '--listen': listen = float(args[i + 1]); i += 1
        else: cmds.append(args[i])
        i += 1
    fd = open_port(port())
    out = b''
    if reset:
        fcntl.ioctl(fd, TIOCMBIS, struct.pack('I', RTS)); time.sleep(0.1)
        fcntl.ioctl(fd, TIOCMBIC, struct.pack('I', RTS))
        # --listen після --reset — саме стільки (раніше бралось --wait, 3 с, і журнал старту обривався)
        out += read_for(fd, listen if listen else (wait if not cmds else 6), until="##PROBE# готово" if cmds else None)
    else:
        out += read_for(fd, listen if listen else 0.3)
    for c in cmds:
        os.write(fd, (c + "\n").encode())
        out += read_for(fd, wait)
    os.close(fd)
    sys.stdout.write(out.decode('utf-8', 'replace'))

if __name__ == '__main__':
    main()
