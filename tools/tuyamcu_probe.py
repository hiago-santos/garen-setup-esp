"""
tuyamcu_probe.py - finge ser o módulo Wi-Fi (Garen Setup) e conversa com a
central do portão pelo protocolo TuyaMCU (quadros 55 AA).

Uso:
  python tuyamcu_probe.py --list                  # lista portas COM
  python tuyamcu_probe.py COM6 sniff              # só escuta (9600 e 115200)
  python tuyamcu_probe.py COM6 probe              # descobre baud, produto e DPs
  python tuyamcu_probe.py COM6 shell --baud 9600  # sessão interativa

Tudo que passa na serial é gravado em tuyamcu_log.txt.

Comandos de descoberta (00, 01, 02, 03, 08) são somente leitura.
Comandos "dp ..." no shell ENVIAM ORDENS À CENTRAL e podem mover o portão.
"""
import argparse
import datetime as dt
import json
import sys
import threading
import time

import serial
import serial.tools.list_ports

LOG = open("tuyamcu_log.txt", "a", encoding="utf-8")

CMD_NAMES = {
    0x00: "heartbeat", 0x01: "product_info", 0x02: "working_mode",
    0x03: "wifi_status", 0x04: "reset_wifi", 0x05: "reset_wifi_mode",
    0x06: "dp_command", 0x07: "dp_report", 0x08: "query_status",
    0x0A: "ota_start", 0x1C: "get_local_time", 0x24: "wifi_signal",
    0x2B: "get_network_status", 0x34: "extended_services", 0x37: "new_features",
}
DP_TYPES = {0: "raw", 1: "bool", 2: "value", 3: "string", 4: "enum", 5: "bitmap"}
DP_TYPE_IDS = {v: k for k, v in DP_TYPES.items()}

dps_seen = {}  # dpid -> (tipo, valor)
product_info = None
last_rx = 0.0


def log(msg):
    line = f"{dt.datetime.now():%H:%M:%S.%f}"[:-3] + " " + msg
    print(line, flush=True)
    LOG.write(line + "\n")
    LOG.flush()


def hexs(b):
    return " ".join(f"{x:02X}" for x in b)


def build(cmd, data=b"", ver=0x00):
    frame = bytes([0x55, 0xAA, ver, cmd, len(data) >> 8, len(data) & 0xFF]) + data
    return frame + bytes([sum(frame) & 0xFF])


def decode_dps(data):
    out, i = [], 0
    while i + 4 <= len(data):
        dpid, typ, ln = data[i], data[i + 1], (data[i + 2] << 8) | data[i + 3]
        raw = data[i + 4:i + 4 + ln]
        if typ == 1:
            val = bool(raw[0]) if raw else None
        elif typ == 2:
            val = int.from_bytes(raw, "big", signed=True)
        elif typ == 3:
            val = raw.decode("utf-8", "replace")
        elif typ in (4, 5):
            val = int.from_bytes(raw, "big")
        else:
            val = hexs(raw)
        out.append((dpid, DP_TYPES.get(typ, f"tipo{typ}"), val))
        i += 4 + ln
    return out


class Parser:
    """Acha quadros 55 AA (e avisa se aparecer o cabeçalho invertido AA 55)."""

    def __init__(self, on_frame):
        self.buf = bytearray()
        self.on_frame = on_frame
        self.warned_aa55 = False

    def feed(self, chunk):
        self.buf += chunk
        while True:
            idx = self.buf.find(b"\x55\xAA")
            if idx < 0:
                if not self.warned_aa55 and b"\xAA\x55" in self.buf:
                    log("!! cabeçalho AA 55 detectado - variante não padrão")
                    self.warned_aa55 = True
                if len(self.buf) > 1:
                    junk = self.buf[:-1]
                    if junk:
                        log(f"   bytes fora de quadro: {hexs(junk)}")
                    del self.buf[:-1]
                return
            if idx > 0:
                log(f"   bytes fora de quadro: {hexs(self.buf[:idx])}")
                del self.buf[:idx]
            if len(self.buf) < 7:
                return
            ln = (self.buf[4] << 8) | self.buf[5]
            if ln > 1024:  # tamanho absurdo, falso cabeçalho
                del self.buf[:2]
                continue
            total = 7 + ln
            if len(self.buf) < total:
                return
            frame = bytes(self.buf[:total])
            del self.buf[:total]
            ok = (sum(frame[:-1]) & 0xFF) == frame[-1]
            self.on_frame(frame, ok)


class Link:
    def __init__(self, port, baud):
        self.ser = serial.Serial(port, baud, timeout=0.05)
        self.parser = Parser(self.handle)
        self.running = True
        self.wifi_status = 0x04  # finge "conectado à nuvem"
        threading.Thread(target=self.reader, daemon=True).start()

    def reader(self):
        global last_rx
        while self.running:
            try:
                chunk = self.ser.read(256)
            except serial.SerialException as e:
                log(f"erro serial: {e}")
                self.running = False
                return
            if chunk:
                last_rx = time.time()
                LOG.write(f"   raw rx: {hexs(chunk)}\n")
                self.parser.feed(chunk)

    def send(self, cmd, data=b""):
        frame = build(cmd, data)
        log(f"TX {CMD_NAMES.get(cmd, hex(cmd)):<18} {hexs(frame)}")
        self.ser.write(frame)

    def handle(self, frame, ok):
        global product_info
        ver, cmd = frame[2], frame[3]
        data = frame[6:-1]
        name = CMD_NAMES.get(cmd, f"0x{cmd:02X}")
        log(f"RX {name:<18} {hexs(frame)}{'' if ok else '  !! CHECKSUM INVÁLIDO'}  (ver={ver})")

        if cmd == 0x00 and data:
            log(f"   heartbeat: {'primeiro após boot' if data[0] == 0 else 'normal'}")
        elif cmd == 0x01:
            txt = data.decode("utf-8", "replace")
            product_info = txt
            log(f"   PRODUTO: {txt}")
            try:
                j = json.loads(txt)
                log(f"   -> PID={j.get('p')} versão MCU={j.get('v')} modo={j.get('m')}")
            except ValueError:
                pass
        elif cmd == 0x02:
            log(f"   modo de trabalho: {hexs(data) or '(vazio = módulo controla LED/botão)'}")
        elif cmd == 0x07:
            for dpid, typ, val in decode_dps(data):
                old = dps_seen.get(dpid)
                dps_seen[dpid] = (typ, val)
                change = "" if old is None else (
                    f"   <<< MUDOU de {old[1]!r}" if old[1] != val else "   (igual)")
                log(f"   DP {dpid:>3}  {typ:<7} = {val!r}{change}")
        # pedidos que a central (MCU) faz ao módulo
        elif cmd == 0x2B:
            self.send(0x2B, bytes([self.wifi_status]))
        elif cmd == 0x1C:
            t = dt.datetime.now()
            self.send(0x1C, bytes([1, t.year % 100, t.month, t.day,
                                   t.hour, t.minute, t.second, t.isoweekday()]))
        elif cmd == 0x24:
            self.send(0x24, bytes([0xC4]))  # -60 dBm
        elif cmd == 0x04:
            log("   central pediu reset do Wi-Fi (botão de pareamento?)")
            self.send(0x04)
        elif cmd == 0x05:
            self.send(0x05)

    def close(self):
        self.running = False
        time.sleep(0.1)
        self.ser.close()


def cmd_sniff(port, bauds, secs):
    for b in bauds:
        log(f"=== escutando {port} @ {b} por {secs}s (sem transmitir) ===")
        link = Link(port, b)
        time.sleep(secs)
        link.close()


def cmd_probe(port, bauds):
    for b in bauds:
        log(f"=== testando {port} @ {b} ===")
        link = Link(port, b)
        t0 = time.time()
        link.send(0x00)
        time.sleep(1.0)
        if last_rx < t0:
            link.send(0x00)
            time.sleep(1.0)
        if last_rx < t0:
            log(f"   nada respondeu a {b} baud")
            link.close()
            continue
        log(f"*** central respondeu a {b} baud ***")
        for c, d in ((0x01, b""), (0x02, b""), (0x03, bytes([0x04])), (0x08, b"")):
            link.send(c, d)
            time.sleep(1.0)
        time.sleep(2.0)
        link.close()
        summary(b)
        return b
    log("Nenhuma resposta. Verifique: TX/RX invertidos? GND comum? nível 3,3/5V? "
        "Rode o modo 'sniff' e veja se aparece qualquer byte.")
    return None


def summary(baud):
    log("=========== RESUMO ===========")
    log(f"baud: {baud}")
    log(f"produto: {product_info}")
    for dpid in sorted(dps_seen):
        typ, val = dps_seen[dpid]
        log(f"  DP {dpid:>3}  {typ:<7} = {val!r}")


def cmd_watch(port, baud, secs, poll):
    """Age como módulo conectado e, a cada `poll` segundos, força a central a
    despejar todos os DPs. A central Garen ignora o 0x08 (query_status), mas
    reenvia o status completo quando o status de rede passa de 03 para 04.
    Só imprime o que mudou. Nenhum comando de DP é enviado."""
    global log
    full_log = log
    link = None

    def quiet(msg):  # só mudanças e anomalias vão para a tela; o resto só para o arquivo
        if "MUDOU" in msg or "fora de quadro" in msg or "CHECKSUM" in msg or "erro" in msg \
                or msg.startswith("   DP") and "(igual)" not in msg:
            full_log(msg)
        else:
            LOG.write(f"{dt.datetime.now():%H:%M:%S.%f}"[:-3] + " " + msg + "\n")

    full_log(f"=== watch @ {baud}, status a cada {poll}s, "
             f"{'até Ctrl+C' if secs <= 0 else f'por {secs}s'} - só leitura ===")
    log = quiet
    t_end = time.time() + secs if secs > 0 else float("inf")
    t_hb = 0.0
    try:
        while time.time() < t_end:
            if link is None or not link.running:  # (re)conecta se a porta sumiu
                if link is not None:
                    full_log("!! conexão perdida - aguardando a porta voltar")
                    try:
                        link.ser.close()
                    except Exception:
                        pass
                    link = None
                try:
                    link = Link(port, baud)
                    full_log(f"conectado em {port}")
                    t_hb = 0.0
                except serial.SerialException:
                    time.sleep(1)
                    continue
            try:
                if time.time() - t_hb >= 15:
                    link.send(0x00)
                    t_hb = time.time()
                link.send(0x03, bytes([0x03]))
                time.sleep(0.15)
                link.send(0x03, bytes([0x04]))
            except serial.SerialException:
                link.running = False
                continue
            time.sleep(poll)
    except KeyboardInterrupt:
        pass
    log = full_log
    if link is not None:
        link.close()
    summary(baud)


SHELL_HELP = """comandos:
  hb | info | mode | status        heartbeat / info produto / modo / pedir todos os DPs
  wifi <0-5>                       informar status de rede (4 = nuvem)
  raw <cmd_hex> [dados hex...]     envia quadro arbitrário, ex: raw 08
  dp <id> <bool|value|enum|string|raw> <valor>   !! ENVIA ORDEM À CENTRAL
  dps                              lista DPs vistos
  q                                sair
(heartbeat automático a cada 15 s)"""


def cmd_shell(port, baud):
    link = Link(port, baud)
    stop = threading.Event()

    def heartbeat():
        while not stop.wait(15):
            link.send(0x00)

    threading.Thread(target=heartbeat, daemon=True).start()
    link.send(0x00)
    print(SHELL_HELP)
    try:
        while True:
            parts = input("> ").strip().split()
            if not parts:
                continue
            c = parts[0].lower()
            try:
                if c == "q":
                    break
                elif c == "hb":
                    link.send(0x00)
                elif c == "info":
                    link.send(0x01)
                elif c == "mode":
                    link.send(0x02)
                elif c == "status":
                    link.send(0x08)
                elif c == "wifi":
                    link.wifi_status = int(parts[1])
                    link.send(0x03, bytes([link.wifi_status]))
                elif c == "raw":
                    link.send(int(parts[1], 16), bytes(int(x, 16) for x in parts[2:]))
                elif c == "dps":
                    for dpid in sorted(dps_seen):
                        print(f"  DP {dpid:>3}  {dps_seen[dpid][0]:<7} = {dps_seen[dpid][1]!r}")
                elif c == "dp":
                    dpid, typ, val = int(parts[1]), parts[2], " ".join(parts[3:])
                    if typ == "bool":
                        v = bytes([1 if val.lower() in ("1", "true", "on") else 0])
                    elif typ == "value":
                        v = int(val).to_bytes(4, "big", signed=True)
                    elif typ == "enum":
                        v = bytes([int(val)])
                    elif typ == "string":
                        v = val.encode()
                    else:
                        v = bytes.fromhex(val)
                    payload = bytes([dpid, DP_TYPE_IDS[typ], len(v) >> 8, len(v) & 0xFF]) + v
                    link.send(0x06, payload)
                else:
                    print(SHELL_HELP)
            except (IndexError, ValueError, KeyError) as e:
                print(f"erro: {e}\n{SHELL_HELP}")
    except (KeyboardInterrupt, EOFError):
        pass
    stop.set()
    link.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?")
    ap.add_argument("mode", nargs="?", default="probe", choices=["sniff", "probe", "watch", "shell"])
    ap.add_argument("--baud", help="baud ou lista, ex: 9600 ou 2400,4800,9600,19200,38400,57600,115200 "
                                   "ou 'all' (padrão: 9600,115200)")
    ap.add_argument("--secs", type=int, default=20, help="tempo de escuta no sniff")
    ap.add_argument("--poll", type=float, default=2.0, help="intervalo do watch em s")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()

    if a.list or not a.port:
        for p in serial.tools.list_ports.comports():
            print(f"{p.device:8} {p.description}")
        return
    if a.baud == "all":
        bauds = [9600, 115200, 2400, 4800, 19200, 38400, 57600]
    elif a.baud:
        bauds = [int(x) for x in a.baud.split(",")]
    else:
        bauds = [9600, 115200]
    log(f"##### sessão {dt.datetime.now():%Y-%m-%d %H:%M} porta={a.port} modo={a.mode}")
    if a.mode == "sniff":
        cmd_sniff(a.port, bauds, a.secs)
    elif a.mode == "probe":
        cmd_probe(a.port, bauds)
    elif a.mode == "watch":
        cmd_watch(a.port, bauds[0], a.secs, a.poll)
    else:
        cmd_shell(a.port, bauds[0])


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
