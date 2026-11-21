"""J19（USB Serial/JTAG）のログを、PC の時刻を付けてファイルに取り続ける。

  python serial_log.py COM3 <秒> <出力ファイル> [--reset]

--reset: 開いたあと RTS を 0.1 秒立ててボードをリセットする（効かないときは esptool の chip-id --after hard-reset を先に）。
MAC アドレスと IPv4 アドレス（255.x.x.x のマスクを除く）は、書き出す前に xx に置き換える。
"""
import re
import sys
import time

import serial

MAC = re.compile(r"\b[0-9a-fA-F]{2}(:[0-9a-fA-F]{2}){5}\b")
IP = re.compile(r"\b(?!255\.)\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b")


def mask(s):
    s = MAC.sub("xx:xx:xx:xx:xx:xx", s)
    return IP.sub("xxx.xxx.xxx.xxx", s)


def main():
    port, secs, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
    ser = serial.Serial(port, 115200, timeout=0.2)
    if "--reset" in sys.argv:
        ser.dtr = False
        ser.rts = True
        time.sleep(0.1)
        ser.rts = False
    end = time.time() + secs
    buf = b""
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        while time.time() < end:
            buf += ser.read(4096)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                t = time.strftime("%H:%M:%S") + f".{int(time.time() * 10) % 10}"
                f.write(f"[{t}] {mask(line.decode(errors='replace').rstrip())}\n")
                f.flush()
    ser.close()


if __name__ == "__main__":
    main()
