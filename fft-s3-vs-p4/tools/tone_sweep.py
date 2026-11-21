"""
PC のスピーカーから、周波数の分かっている sin 波を順に鳴らし、
ボード（mic_fft）がシリアルに出す「いちばん強い周波数」と比べる。

- 使うのは Python の標準ライブラリ（wave・winsound）と pyserial だけ（Windows 用）
- 音量は小さく始める: まず 1kHz を小さい振幅から鳴らし、ボードが 1kHz を拾えた振幅で、残りの周波数を鳴らす
- --dry-run: 音を出さず、ボードもつながない。作った WAV の周波数を、この中の DFT で確かめるだけ

例:
  python tone_sweep.py --dry-run
  python tone_sweep.py --port COM5 --csv sweep_s3.csv

SPDX-License-Identifier: Apache-2.0
"""
import argparse
import math
import os
import re
import statistics
import struct
import sys
import tempfile
import time
import wave

RATE = 48000
DEFAULT_FREQS = [250, 500, 1000, 1234, 2000, 4000, 8000]
RAMP_AMPS = [0.01, 0.02, 0.05, 0.1, 0.2]   # 1kHz で拾えるまで、この順に大きくする（1.0 がフルスケール）
LINE = re.compile(r"peak=([0-9.]+)Hz bin=(\d+) level=(-?[0-9.]+)dBFS proc=(\d+)us fft=(\d+)us block=(\d+)us ovf=(\d+)")


def make_wav(path, freq, amp, dur, fade=0.05):
    """freq Hz・振幅 amp の sin 波を dur 秒。頭と終わりを fade 秒かけて上げ下げする（プツッという音を避ける）"""
    n = int(RATE * dur)
    nf = int(RATE * fade)
    frames = bytearray()
    for i in range(n):
        g = min(1.0, i / nf, (n - 1 - i) / nf) if nf else 1.0
        v = int(32767 * amp * g * math.sin(2 * math.pi * freq * i / RATE))
        frames += struct.pack("<h", v)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(frames))


def wav_peak(path, fmin=50, fmax=12000, step=1.0):
    """WAV の真ん中 0.5 秒で、fmin〜fmax を step Hz ごとに DFT して、いちばん強い周波数を返す（確かめ用）"""
    with wave.open(path, "rb") as w:
        n = w.getnframes()
        data = struct.unpack("<%dh" % n, w.readframes(n))
    m = RATE // 2
    seg = data[(n - m) // 2:(n - m) // 2 + m]
    best_f, best_p = 0.0, -1.0
    f = float(fmin)
    while f <= fmax:
        w0 = 2 * math.pi * f / RATE
        # Goertzel 法（1 つの周波数だけの DFT）
        c = 2 * math.cos(w0)
        s1 = s2 = 0.0
        for x in seg:
            s0 = x + c * s1 - s2
            s2, s1 = s1, s0
        p = s1 * s1 + s2 * s2 - c * s1 * s2
        if p > best_p:
            best_f, best_p = f, p
        f += step
    return best_f


def coarse_then_fine(path, freq):
    """全体を粗く探してから、見つかった所の近くを細かく探す"""
    f0 = wav_peak(path, 50, 12000, 25.0)
    return wav_peak(path, max(50, f0 - 30), f0 + 30, 0.5)


def play_and_listen(ser, path, dur, settle=0.6):
    """WAV を非同期で鳴らし、鳴っている間のボードの行を集める（鳴り始めの settle 秒は捨てる）"""
    import winsound
    ser.reset_input_buffer()
    winsound.PlaySound(path, winsound.SND_FILENAME | winsound.SND_ASYNC)
    t0 = time.time()
    rows = []
    buf = b""
    while time.time() - t0 < dur:
        buf += ser.read(4096)
        *lines, buf = buf.split(b"\n")
        for ln in lines:
            m = LINE.search(ln.decode("utf-8", "replace"))
            if m and time.time() - t0 > settle:
                rows.append(tuple(float(g) for g in m.groups()))
    winsound.PlaySound(None, 0)
    return rows


def summarize(rows):
    if not rows:
        return None
    peaks = [r[0] for r in rows]
    return {
        "blocks": len(rows),
        "peak_median": statistics.median(peaks),
        "level_median": statistics.median(r[2] for r in rows),
        "proc_max_us": max(r[3] for r in rows),
        "fft_max_us": max(r[4] for r in rows),
        "block_us": rows[0][5],
        "ovf_last": rows[-1][6],
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="ボードの COM ポート（無ければ --dry-run）")
    ap.add_argument("--freqs", type=float, nargs="*", default=DEFAULT_FREQS)
    ap.add_argument("--dur", type=float, default=3.0, help="1 つの音の長さ（秒）")
    ap.add_argument("--amp", type=float, help="振幅を固定する（0〜1）。無ければ 1kHz で小さい方から探す")
    ap.add_argument("--csv", help="結果を書く CSV")
    ap.add_argument("--dry-run", action="store_true", help="音を出さず、WAV の周波数だけ確かめる")
    a = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix="tone_sweep_")
    if a.dry_run or not a.port:
        ok = True
        for f in a.freqs:
            p = os.path.join(tmp, "t%g.wav" % f)
            make_wav(p, f, 0.05, 1.0)
            got = coarse_then_fine(p, f)
            res = abs(got - f) <= 1.0
            ok &= res
            print("dry-run %7.1f Hz -> WAV peak %7.1f Hz %s" % (f, got, "OK" if res else "NG"))
        print("dry-run", "OK" if ok else "NG", "(WAV: %s)" % tmp)
        return 0 if ok else 1

    import serial
    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = a.port, 115200, 0.05
    ser.dtr = False   # 開くときにボードをリセットしない（ESPr の自動リセット回路）
    ser.rts = False
    ser.open()
    time.sleep(1.0)

    # 無音のとき（部屋の音）
    q = os.path.join(tmp, "silence.wav")
    make_wav(q, 1000, 0.0, a.dur)
    s = summarize(play_and_listen(ser, q, a.dur))
    print("silence:", s)
    results = [("silence", 0.0, s)]

    amp = a.amp
    if amp is None:
        for cand in RAMP_AMPS:
            p = os.path.join(tmp, "ramp_%g.wav" % cand)
            make_wav(p, 1000, cand, a.dur)
            s = summarize(play_and_listen(ser, p, a.dur))
            print("ramp amp=%.2f 1000Hz:" % cand, s)
            results.append(("ramp", cand, s))
            if s and abs(s["peak_median"] - 1000) < 30:
                amp = cand
                break
        if amp is None:
            print("1kHz を拾えなかった（マイクの向き・配線・PC の音量を確かめる）")
            return 2

    for f in a.freqs:
        p = os.path.join(tmp, "t%g.wav" % f)
        make_wav(p, f, amp, a.dur)
        s = summarize(play_and_listen(ser, p, a.dur))
        err = (s["peak_median"] - f) if s else float("nan")
        print("tone %7.1f Hz amp=%.2f -> %s  err=%+.1f Hz" % (f, amp, s, err))
        results.append(("tone", f, s))
    ser.close()

    if a.csv:
        with open(a.csv, "w", encoding="utf-8") as fo:
            fo.write("kind,freq_or_amp,blocks,peak_median_hz,level_median_dbfs,proc_max_us,fft_max_us,block_us,ovf_last\n")
            for kind, x, s in results:
                if s:
                    fo.write("%s,%g,%d,%.1f,%.1f,%d,%d,%d,%d\n" % (kind, x, s["blocks"], s["peak_median"], s["level_median"],
                                                               s["proc_max_us"], s["fft_max_us"], s["block_us"], s["ovf_last"]))
                else:
                    fo.write("%s,%g,0,,,,,,\n" % (kind, x))
    return 0


if __name__ == "__main__":
    sys.exit(main())
