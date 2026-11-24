#!/usr/bin/env python3
"""E2E test: NET A0h-A9h Unicard uc3 v0.37 přes skutečné sokety (MCP pipe).

Dva emulátory (Unicard v0.37, každý s vlastní SD) hrají přes malou relay
v tomto souboru (jen standardní knihovna, protokol MZPico NET z BomberNet
docs/net-protocol.md): jednou JSON řádky přes TCP ([UNICARD] net_relay),
jednou WebSocket (NETRELAY = ws://127.0.0.1:port v /unicard/network.cfg na
SD, jako karta). Porty se řídí přes MCP io_write/io_read jako u programu na
MZ: CREATE a čekání na stav, JOIN s kódem místnosti, READY a start,
vstupy snímku z obou strojů, zpráva, odchod a DROPPED u druhého.

Jen MZ-800 (uc3 v0.37). Binárka v kořeni repa nebo v ``MZ_EMU_DIR``.
Výstup anglicky. Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (žádná binárka).
"""

import base64
import hashlib
import json
import os
import queue
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
import test_gdg_cksw as h  # noqa: E402 - _find_exe, TestFailure, check

CMD, DATA = 0x50, 0x51
GAME, BUILD = 0x1234, 0x0102


# ---------------------------------------------------------------- relay

class Relay:
    """Relay NET: místnosti, members, start, přeposílání vstupů a zpráv, dropped."""

    WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

    def __init__(self):
        self.lock = threading.Lock()
        self.rooms = {}             # code -> {"game", "build", "slots", "bytes", "settings", "members": [conn]}
        self.next_code = 0
        self.lines = []             # řádky od klientů (kontrola)
        self.pongs = []             # odpovědi WebSocketu na ping
        self.tcp = self._listen(self._serve_tcp)
        self.ws = self._listen(self._serve_ws)

    def _listen(self, handler):
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        s.listen(8)
        threading.Thread(target=self._accept, args=(s, handler), daemon=True).start()
        return s.getsockname()[1]

    def _accept(self, s, handler):
        while True:
            c, _ = s.accept()
            threading.Thread(target=handler, args=(c,), daemon=True).start()

    # --- doprava

    def _serve_tcp(self, c):
        conn = {"send": lambda line: c.sendall((line + "\n").encode()), "slot": None, "ready": 0}
        buf = b""
        while True:
            d = c.recv(4096)
            if not d:
                break
            buf += d
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self._line(conn, line.decode())
        self._gone(conn)

    def _serve_ws(self, c):
        req = b""
        while b"\r\n\r\n" not in req:
            d = c.recv(4096)
            if not d:
                return
            req += d
        head, rest = req.split(b"\r\n\r\n", 1)
        hdr = {}
        for ln in head.decode().split("\r\n")[1:]:
            k, _, v = ln.partition(":")
            hdr[k.strip().lower()] = v.strip()
        accept = base64.b64encode(hashlib.sha1((hdr["sec-websocket-key"] + self.WS_GUID).encode()).digest()).decode()
        c.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                   f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode())

        def send(line, op=1):
            data = line.encode() if isinstance(line, str) else line
            c.sendall(bytes([0x80 | op, len(data)]) + data if len(data) < 126
                      else bytes([0x80 | op, 126]) + struct.pack(">H", len(data)) + data)
        conn = {"send": send, "slot": None, "ready": 0, "path": head.decode().split(" ")[1]}
        send(b"x", 9)                               # ping: karta musí odpovědět pongem
        buf = rest
        while True:
            while len(buf) >= 2:
                n = buf[1] & 0x7F
                h0 = 2
                if n == 126:
                    if len(buf) < 4:
                        break
                    n, h0 = struct.unpack(">H", buf[2:4])[0], 4
                if len(buf) < h0 + 4 + n:
                    break
                op, mask = buf[0] & 0x0F, buf[h0:h0 + 4]
                data = bytes(b ^ mask[i & 3] for i, b in enumerate(buf[h0 + 4:h0 + 4 + n]))
                buf = buf[h0 + 4 + n:]
                if op == 1:
                    self._line(conn, data.decode())
                elif op == 10:
                    self.pongs.append(data)
                elif op == 8:
                    self._gone(conn)
                    return
            d = c.recv(4096)
            if not d:
                break
            buf += d
        self._gone(conn)

    # --- protokol

    def _members(self, room):
        ready = sum(1 << m["slot"] for m in room["members"] if m["ready"])
        for m in room["members"]:
            m["send"](json.dumps({"op": "members", "count": len(room["members"]), "ready": ready}))
        if len(room["members"]) >= 2 and all(m["ready"] for m in room["members"]):
            mask = sum(1 << m["slot"] for m in room["members"])
            for m in room["members"]:
                m["send"](json.dumps({"op": "start", "seed": 4660, "frame": 0, "mask": mask}))

    def _line(self, conn, line):
        msg = json.loads(line)
        with self.lock:
            self.lines.append(msg)
            op = msg["op"]
            if op == "create":
                code = "QR" + "ABCDEFGHJKLMNPQRSTUVWXYZ"[self.next_code] * 2
                self.next_code += 1
                room = dict(game=msg["game"], build=msg["build"], slots=msg["slots"], bytes=msg["bytes"],
                            settings=msg["settings"], members=[conn])
                self.rooms[code] = room
                conn.update(slot=0, room=room)
                conn["send"](json.dumps({"op": "room", "code": code, "slot": 0, "slots": room["slots"],
                                         "bytes": room["bytes"], "settings": room["settings"]}))
            elif op == "join":
                room = self.rooms.get(msg["code"])
                if not room or room["game"] != msg["game"] or len(room["members"]) >= room["slots"]:
                    conn["send"](json.dumps({"op": "error", "code": 7, "text": "room"}))
                    return
                if room["build"] != msg["build"]:
                    conn["send"](json.dumps({"op": "error", "code": 6, "text": "build"}))
                    return
                conn.update(slot=len(room["members"]), room=room)
                room["members"].append(conn)
                conn["send"](json.dumps({"op": "room", "code": msg["code"], "slot": conn["slot"],
                                         "slots": room["slots"], "bytes": room["bytes"],
                                         "settings": room["settings"]}))
                self._members(room)
            elif op == "ready":
                conn["ready"] = msg["ready"]
                self._members(conn["room"])
            elif op == "input":
                for m in conn["room"]["members"]:
                    if m is not conn:
                        m["send"](json.dumps({"op": "input", "frame": msg["frame"], "slot": conn["slot"],
                                              "data": msg["data"]}))
            elif op == "msg":
                for m in conn["room"]["members"]:
                    if m is not conn and msg["to"] in (-1, m["slot"]):
                        m["send"](json.dumps({"op": "msg", "from": conn["slot"], "data": msg["data"]}))
            elif op == "leave":
                self._gone(conn, locked=True)

    def _gone(self, conn, locked=False):
        if not locked:
            self.lock.acquire()
        try:
            room = conn.pop("room", None)
            if room and conn in room["members"]:
                room["members"].remove(conn)
                for m in room["members"]:
                    try:
                        m["send"](json.dumps({"op": "dropped", "slot": conn["slot"]}))
                    except OSError:
                        pass
        finally:
            if not locked:
                self.lock.release()


# ---------------------------------------------------------------- emulátor

class Machine:
    """Emulátor v pipe módu s Unicard v0.37 a vlastní SD."""

    @emu_test_proc.kill_on_init_failure
    def __init__(self, exe, tmp, name, net_relay="", network_cfg=None):
        sd = tmp / ("sd_" + name)
        (sd / "unicard").mkdir(parents=True)
        if network_cfg:
            (sd / "unicard" / "network.cfg").write_text(network_cfg, encoding="ascii")
        ini = tmp / (name + ".ini")
        ini.write_text("[UNICARD]\nconnected = 1\nfw_version = 1\nuc3_version = v0.37\n"
                       f"sd_root = {sd}\nruntime_check_on_connect = 0\nnet_relay = {net_relay}\n", encoding="utf-8")
        wd = tmp / ("work_" + name)
        wd.mkdir()
        args = [str(exe), "--mcp-pipe", "--headless", "--no-save-ini", "--no-first-run-windows",
                f"--cfg-dir={wd}", f"--work-dir={wd}", f"--config={ini}"]
        env = dict(os.environ)
        env.pop("MZ800EMU_UNICARD_NET_RELAY", None)
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, bufsize=1, encoding="utf-8",
                                     cwd=str(h._REPO_ROOT), env=env)
        self.q = queue.Queue()
        threading.Thread(target=self._pump, daemon=True).start()
        self.rid = 0
        if not self._read(20.0, lambda m: m.get("type") == "hello"):
            raise h.TestFailure("no hello from emulator")
        self.call("pause")

    _pump = h.PipeEmu._pump
    _read = h.PipeEmu._read
    call = h.PipeEmu.call

    def close(self):
        self.dead = False
        h.PipeEmu.close(self)

    def out(self, port, value):
        self.call("io_write", {"port": port, "value": value})

    def inp(self, port):
        return self.call("io_read", {"port": port})["value"]

    def status(self):
        self.out(CMD, 0x03)
        return self.inp(CMD)

    def error_code(self):
        self.out(CMD, 0x03)
        return [self.inp(CMD) for _ in range(3)][2]

    def cmd(self, c, params=b"", n_out=0, wait=10.0):
        """Příkaz s parametry; čeká na stav bez IN_PROGRESS; vrací výstup, nebo kód chyby (int)."""
        self.out(CMD, c)
        for b in params:
            self.out(DATA, b)
        deadline = time.time() + wait
        st = self.status()
        while st & 0x40 and time.time() < deadline:
            time.sleep(0.02)
            st = self.status()
        if st & 0x40:
            raise h.TestFailure(f"command {c:02X}h still IN_PROGRESS after {wait:.0f} s")
        if st & 0x80:
            return self.error_code()
        return bytes(self.inp(DATA) for _ in range(n_out))


def net_status(m):
    return m.cmd(0xA0, n_out=8)


def until(what, fn, wait=5.0):
    deadline = time.time() + wait
    while time.time() < deadline:
        v = fn()
        if v:
            return v
        time.sleep(0.05)
    raise h.TestFailure("timeout: " + what)


def game(exe, tmp, relay, kind):
    print(f"  --- {kind}")
    if kind == "tcp":
        a = Machine(exe, tmp, "a_tcp", net_relay=f"127.0.0.1:{relay.tcp}")
        b = Machine(exe, tmp, "b_tcp", net_relay=f"127.0.0.1:{relay.tcp}")
    else:
        cfg = f"NETRELAY = ws://127.0.0.1:{relay.ws}\r\n"
        a = Machine(exe, tmp, "a_ws", network_cfg=cfg)
        b = Machine(exe, tmp, "b_ws", network_cfg=cfg)
    try:
        info = a.cmd(0x95, n_out=16)
        h.check(info[3] & 0x08 == 0x08, "INFO: NET")
        h.check(net_status(a)[0] == 1, "NETSTATUS: READY")

        create = struct.pack("<HHBBB", GAME, BUILD, 2, 1, 2) + b"\xaa\xbb" + bytes(14)
        out = a.cmd(0xA1, create, 6)
        h.check(isinstance(out, bytes) and out[4] == 0x0D and out[5] == 0, f"CREATE: room {out!r}")
        code = out[:4]
        print(f"  room {code.decode()}")

        bad = b.cmd(0xA2, struct.pack("<HH", GAME, BUILD + 1) + code + b"\r", 20)
        h.check(bad == 6, f"JOIN with another build: error 6 (got {bad!r})")
        out = b.cmd(0xA2, struct.pack("<HH", GAME, BUILD) + code + b"\r", 20)
        h.check(isinstance(out, bytes) and out[:6] == b"\x01\x02\x01\x02\xaa\xbb", f"JOIN: slot 1 {out!r}")

        until("A sees 2 members", lambda: net_status(a)[2] == 2)
        b.cmd(0xA4, b"\x01", 4)
        a.cmd(0xA4, b"\x01", 4)
        seed = until("start", lambda: (lambda o: o if o != b"\xff\xff\xff\xff" else None)(a.cmd(0xA4, b"\x01", 4)))
        h.check(seed == b"\x34\x12\x00\x00", f"READY: seed 1234h, frame 0 ({seed!r})")
        until("B RUNNING", lambda: net_status(b)[0] == 3)

        a.cmd(0xA5, b"\x00\x00\x41\x00\x00\x00")
        b.cmd(0xA5, b"\x00\x00\x42\x00\x00\x00")
        for m, n in ((a, "A"), (b, "B")):
            got = until(f"{n}: frame 0", lambda: (lambda o: o if o[:2] == b"\x00\x00" else None)(m.cmd(0xA6, b"\x00\x00", 4)))
            h.check(got == b"\x00\x00\x41\x42", f"{n}: frame 0 inputs {got!r}")

        a.cmd(0xA8, b"\xff\x02hi" + bytes(30))
        got = until("B: message", lambda: (lambda o: o if o[0] != 0xFF else None)(b.cmd(0xA9, n_out=34)))
        h.check(got[:4] == b"\x00\x02hi", f"B: message from slot 0 {got[:4]!r}")

        h.check(a.cmd(0xA3) == b"", "A: LEAVE")
        until("B: DROPPED", lambda: net_status(b)[0] == 5)
        h.check(net_status(a)[0] == 1, "A: READY after LEAVE")
        if kind == "ws":
            h.check(relay.pongs.count(b"x") >= 2, f"WebSocket: pong to the relay's ping ({relay.pongs!r})")
    finally:
        a.close()
        b.close()


def main():
    emu_test_proc.install(deadline_s=180)
    exe = h._find_exe("mz800emu")
    if exe is None:
        print("SKIP: mz800emu binary not found")
        return 77
    tmp = Path(tempfile.mkdtemp(prefix="unicard_net_"))
    relay = Relay()
    print(f"\n=== {exe.name}: relay tcp {relay.tcp}, ws {relay.ws} ===")
    ok = True
    for kind in ("tcp", "ws"):
        try:
            game(exe, tmp, relay, kind)
        except h.TestFailure as e:
            print(f"  FAILED: {e}")
            ok = False
    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
