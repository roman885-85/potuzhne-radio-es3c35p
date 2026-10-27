#!/usr/bin/env python3
"""Консоль ES3C35P: по USB Serial/JTAG (без перемикання DTR) або по Wi-Fi.

USB. На ESP32-S3 з рідним USB піднятий DTR засувує GPIO0 у нуль, і наступне
скидання йде в завантажувач. Тому порт відкриваємо з DTR=0 і RTS=0 і їх не
чіпаємо; перезапуск (--reset) — лише RTS 0→1→0 при DTR=0.

Wi-Fi. Консоль налагодження самого радіо (extras/yoNetCon, TCP 2323): ті самі
команди й той самий журнал. Адреса — --net АДРЕСА, або змінна RADIO_HOST, або
файл tools/es3c35p/.radio (перший рядок, у git не йде; напр. potuzhne-c53b60.local).
Немає USB-порту, а адреса є — консоль сама йде по Wi-Fi. Перезапуск по Wi-Fi —
команда reboot; далі чекаємо, доки радіо знову в мережі, і показуємо журнал
старту з його пам'яті (команда каналу «hist»).

  con.py [--net [АДРЕСА]] [--reset] [--wait С] [--listen С] "команда" ["команда" ...]
  --listen С — просто слухати стільки секунд (нічого не посилаючи)
  --host     — надрукувати адресу радіо для Wi-Fi і вийти (для flash.sh --wifi)
"""
import os, sys, time, termios, fcntl, struct, select, glob, socket

TIOCMBIS, TIOCMBIC = 0x8004746d, 0x8004746b
DTR, RTS = 0x002, 0x004
NET_PORT = 2323
HERE = os.path.dirname(os.path.abspath(__file__))

def port():
    """USB-порт плати або None."""
    p = os.environ.get("PORT")
    if p: return p
    c = sorted(glob.glob("/dev/cu.usbmodem*"))
    return c[0] if c else None

def net_host(arg=None):
    """Адреса радіо для Wi-Fi: аргумент, RADIO_HOST або tools/es3c35p/.radio."""
    if arg: return arg
    h = os.environ.get("RADIO_HOST")
    if h: return h.strip()
    f = os.path.join(HERE, ".radio")
    if os.path.exists(f):
        t = open(f, encoding="utf-8").read().split()
        if t: return t[0]
    return None

class Link:
    """Спільне для USB і Wi-Fi: читання з терміном, надсилання, закриття."""
    fd = -1
    def read_for(self, secs, until=None):
        out = b''; end = time.time() + secs
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if r:
                d = self._recv()
                if d is None: break                    # з'єднання закрилось
                out += d
                if until and until.encode() in out: break
        return out

class SerialLink(Link):
    def __init__(self, path):
        fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        a = termios.tcgetattr(fd); a[0] = a[1] = a[3] = 0
        a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL; a[4] = a[5] = termios.B115200
        a[6][termios.VMIN] = 0; a[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, a)
        fcntl.ioctl(fd, TIOCMBIC, struct.pack('I', DTR | RTS))
        self.fd = fd
    def _recv(self):
        try: return os.read(self.fd, 65536)
        except OSError: return b''
    def send(self, b): os.write(self.fd, b)
    def reset(self):
        fcntl.ioctl(self.fd, TIOCMBIS, struct.pack('I', RTS)); time.sleep(0.1)
        fcntl.ioctl(self.fd, TIOCMBIC, struct.pack('I', RTS))
        return self
    def close(self): os.close(self.fd)

class NetLink(Link):
    def __init__(self, host, wait=0):
        """wait — скільки секунд пробувати під'єднатись (радіо щойно перезапустилось)."""
        self.host = host
        end = time.time() + wait; err = None
        while True:
            try:
                self.s = socket.create_connection((host, NET_PORT), timeout=5)
                break
            except OSError as e:
                err = e
                # macOS пускає в локальну мережу лише дозволені програми: Python із python.org
                # (і node) отримують «No route to host», системний /usr/bin/python3 — ні.
                # Тоді просто перезапускаємось ним (той самий скрипт, ті самі аргументи).
                if (e.errno == 65 and sys.platform == 'darwin' and os.path.exists('/usr/bin/python3')
                        and os.path.realpath(sys.executable) != os.path.realpath('/usr/bin/python3')
                        and not os.environ.get('CON_REEXEC')):
                    os.environ['CON_REEXEC'] = '1'
                    os.execv('/usr/bin/python3', ['/usr/bin/python3'] + sys.argv)
                if time.time() >= end:
                    sys.exit(f"радіо {host}:{NET_PORT} не відповідає по Wi-Fi ({err}); "
                             "чи ввімкнено «Розробник › Консоль по Wi-Fi»?")
                time.sleep(1)
        self.s.setblocking(False)
        self.fd = self.s.fileno()
    def _recv(self):
        try:
            d = self.s.recv(65536)
            return d if d else None
        except BlockingIOError: return b''
        except OSError: return None
    def send(self, b):
        self.s.setblocking(True); self.s.sendall(b); self.s.setblocking(False)
    def reset(self):
        """Перезапуск по Wi-Fi: reboot, тоді нове з'єднання й журнал старту з пам'яті радіо."""
        try: self.send(b"reboot\n")
        except OSError: pass
        self.s.close()
        time.sleep(4)
        n = NetLink(self.host, wait=90)
        n.send(b"hist\n")
        return n
    def close(self):
        try: self.send(b"bye\n")
        except OSError: pass
        self.s.close()

def open_link(net=False, host=None):
    """USB, якщо плата під'єднана й Wi-Fi не просили; інакше Wi-Fi."""
    if not net:
        p = port()
        if p: return SerialLink(p)
    h = net_host(host)
    if not h:
        sys.exit("немає ні USB-порту, ні адреси радіо: --net АДРЕСА, RADIO_HOST або файл tools/es3c35p/.radio")
    return NetLink(h)

# — для старих скриптів —
def open_port(path): return SerialLink(path).fd
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
    args = sys.argv[1:]; wait = 3.0; reset = False; cmds = []; listen = 0.0; net = False; host = None
    i = 0
    while i < len(args):
        a = args[i]
        if a == '--reset': reset = True
        elif a == '--wait': wait = float(args[i + 1]); i += 1
        elif a == '--listen': listen = float(args[i + 1]); i += 1
        elif a == '--host':
            h = net_host(); print(h or ''); sys.exit(0 if h else 1)
        elif a == '--net':
            net = True
            if i + 1 < len(args) and not args[i + 1].startswith('-') and ('.' in args[i + 1] or ':' in args[i + 1]):
                host = args[i + 1]; i += 1
        elif a.startswith('--net='): net = True; host = a[6:]
        else: cmds.append(a)
        i += 1
    link = open_link(net, host)
    out = b''
    if reset:
        link = link.reset()
        # --listen після --reset — саме стільки (раніше бралось --wait, 3 с, і журнал старту обривався)
        out += link.read_for(listen if listen else (wait if not cmds else 6), until="##PROBE# готово" if cmds else None)
    else:
        out += link.read_for(listen if listen else 0.3)
    for c in cmds:
        link.send((c + "\n").encode())
        out += link.read_for(wait)
    link.close()
    sys.stdout.write(out.decode('utf-8', 'replace'))

if __name__ == '__main__':
    main()
