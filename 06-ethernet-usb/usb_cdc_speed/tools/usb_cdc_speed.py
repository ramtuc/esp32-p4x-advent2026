"""usb_cdc_speed（ESP32-P4 の USB CDC-ACM）の転送速度を PC 側から測る。

  python usb_cdc_speed.py COMx [--size 64] [--repeat 3]
  python usb_cdc_speed.py --selftest      # ボードなしで、このスクリプトの動きだけ確かめる

- PC -> ボード: "W <n>" を送り、続けて n バイトを送る。ボードが "OK W <n> <us>" を返すまでの時間で割る
- ボード -> PC: "R <n>" を送り、n バイトを受け取り終えるまでの時間で割る。中身（32 ビットの通し番号）も確かめる
  受け取り用のバッファは先に n バイト確保しておく。読みながら継ぎ足すと、Python がバッファを大きく取り直す間
  読むのが止まり、そのあいだに Windows の CDC ドライバの中でパケットがなくなることがあったため
- MB/s は 10^6 バイト／秒。Mbit/s も並べて出す
"""
import argparse
import array
import sys
import threading
import time

def expected(n):
    """ボードが送る中身: 32 ビットの通し番号（リトルエンディアン）。512 バイト単位の抜けも見つけられる"""
    return array.array("I", range((n + 3) // 4)).tobytes()[:n]


def read_line(ser, timeout=5.0):
    end = time.perf_counter() + timeout
    buf = b""
    while time.perf_counter() < end:
        c = ser.read(1)
        if c:
            if c == b"\n":
                return buf.decode(errors="replace").strip()
            buf += c
    raise TimeoutError(f"no line (got {buf!r})")


def info(ser):
    ser.reset_input_buffer()
    ser.write(b"I\n")
    return read_line(ser)


def pc_to_board(ser, n, chunk=1 << 16):
    data = expected(n)
    ser.reset_input_buffer()
    ser.write(f"W {n}\n".encode())
    t0 = time.perf_counter()
    for off in range(0, n, chunk):
        ser.write(data[off:off + chunk])
    line = read_line(ser, timeout=60)
    t1 = time.perf_counter()
    parts = line.split()
    if parts[:2] != ["OK", "W"] or int(parts[2]) != n:
        raise RuntimeError(f"unexpected reply: {line!r}")
    return t1 - t0, int(parts[3]) / 1e6


def analyze(got, n):
    """受けた中身を 4 バイトの通し番号として調べ、抜け・置き換わりを全部挙げる"""
    words = array.array("I", bytes(got[:len(got) // 4 * 4]))
    gaps = []
    expect = 0
    for idx, w in enumerate(words):
        if w != expect:
            gaps.append({"pos": expect * 4, "mod4096": (expect * 4) % 4096, "expected": expect, "got": w,
                         "kind": "skip" if w > expect else "back", "at_word": idx})
            expect = w
        expect += 1
    # それぞれの食い違いのあと、番号が何語つながったか
    for i, g in enumerate(gaps):
        nxt = gaps[i + 1]["at_word"] if i + 1 < len(gaps) else len(words)
        g["run_words"] = nxt - g["at_word"]
        del g["at_word"]
    last = words[-1] if len(words) else None
    return {"gaps": gaps, "last_word": last, "final_word": (n + 3) // 4 - 1,
            "tail_ok": last == (n + 3) // 4 - 1}


def board_to_pc(ser, n, chunk=1 << 16):
    """戻り値: (秒, データが正しいか, 食い違いの記録 or None)"""
    buf = bytearray(n)  # 先に確保する（読んでいる途中で取り直しが起きないように）
    mv = memoryview(buf)
    exp = expected(n)   # 照合用の期待値も、測る前に作っておく
    ser.reset_input_buffer()
    ser.write(f"R {n}\n".encode())
    t0 = time.perf_counter()
    pos = 0
    idle = 0
    while pos < n:
        b = ser.read(min(chunk, n - pos))
        if not b:
            idle += 1
            if idle >= 3:  # 3 秒何も来なければ打ち切る
                break
            continue
        mv[pos:pos + len(b)] = b
        pos += len(b)
    t1 = time.perf_counter()
    if pos == n and buf == exp:
        return t1 - t0, True, None
    rep = analyze(bytes(buf[:pos]), n)
    rep["received"] = pos
    return t1 - t0, False, rep


def mbps(n, sec):
    return f"{n / sec / 1e6:7.2f} MB/s ({n * 8 / sec / 1e6:7.1f} Mbit/s)"


def run(ser, size_mb, repeat):
    print("info:", info(ser))
    n = int(size_mb * 1_000_000)
    for i in range(repeat):
        sec, board_sec = pc_to_board(ser, n)
        print(f"[{i + 1}] PC->board {n} B: {sec:6.3f} s  {mbps(n, sec)}   (board timer: {mbps(n, board_sec)})")
        sec, ok, rep = board_to_pc(ser, n)
        if ok:
            print(f"[{i + 1}] board->PC {n} B: {sec:6.3f} s  {mbps(n, sec)}   data OK")
        else:
            g = rep["gaps"]
            print(f"[{i + 1}] board->PC {n} B: {sec:6.3f} s  MISMATCH received={rep['received']} gaps={len(g)} "
                  f"last_word={rep['last_word']} (final {rep['final_word']}, tail_ok={rep['tail_ok']})")
            for x in g[:20]:
                print(f"      gap at byte {x['pos']} (mod 4096 = {x['mod4096']}): expected word {x['expected']}, "
                      f"got {x['got']} ({x['kind']} {abs(x['got'] - x['expected']) * 4} B), then {x['run_words']} words in order")
            mods = sorted({x["mod4096"] for x in g})
            print(f"      mod 4096 of all gaps: {mods}")


class FakeBoard:
    """--selftest 用。pyserial の Serial と同じ read/write/reset_input_buffer を持ち、
    ボードの main.c と同じ約束で答える（速さは測らない）。"""

    def __init__(self):
        self.out = bytearray()
        self.lock = threading.Lock()
        self.line = b""
        self.sink = 0
        self.sink_t0 = 0

    def reset_input_buffer(self):
        with self.lock:
            self.out.clear()

    def _emit(self, b):
        with self.lock:
            self.out += b

    def write(self, data):
        i = 0
        while i < len(data):
            if self.sink:
                if self.sink_t0 == 0:
                    self.sink_t0 = time.perf_counter()
                take = min(self.sink, len(data) - i)
                self.sink -= take
                i += take
                if self.sink == 0:
                    us = int((time.perf_counter() - self.sink_t0) * 1e6) or 1
                    self._emit(f"OK W {self.sink_n} {us}\n".encode())
            else:
                c = data[i:i + 1]
                i += 1
                if c == b"\n":
                    cmd = self.line.decode()
                    self.line = b""
                    if cmd == "I":
                        self._emit(b"INFO port=FAKE speed=none ep=0 rx=0 tx=0\n")
                    elif cmd.startswith("W"):
                        self.sink = self.sink_n = int(cmd[1:])
                        self.sink_t0 = 0
                    elif cmd.startswith("R"):
                        self._emit(expected(int(cmd[1:])))
                else:
                    self.line += c
        return len(data)

    def read(self, n=1):
        with self.lock:
            b = bytes(self.out[:n])
            del self.out[:n]
        return b


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?")
    ap.add_argument("--size", type=float, default=64, help="1 回に送る量（MB = 10^6 バイト）")
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--rxbuf", type=int, default=0, help="Windows のドライバの受信バッファ（バイト）")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        fb = FakeBoard()
        run(fb, 1.0, 1)
        # 抜けの解析だけを確かめる: 4096*3+512 から 512 バイトを抜いた列
        e = expected(100000)
        bad = e[:4096 * 3 + 512] + e[4096 * 3 + 1024:]
        print("analyze test:", analyze(bad, 100000))
        return 0
    import serial
    with serial.Serial(a.port, 115200, timeout=1) as ser:  # CDC-ACM ではボーレートは速さに関係しない
        if a.rxbuf:
            ser.set_buffer_size(rx_size=a.rxbuf, tx_size=a.rxbuf)
        run(ser, a.size, a.repeat)
    return 0


if __name__ == "__main__":
    sys.exit(main())
