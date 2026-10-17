#!/usr/bin/env python3
"""yd_f0.py — yongde 槽位声调客观判定: 纯 python 读 wav, 自相关基频, 输出归一化 F0 轮廓.
用法: python3 yd_f0.py /tmp/yd_slot_417.wav ..."""
import sys, wave

def load(path):
    w = wave.open(path, "rb")
    n = w.getnframes()
    sw = w.getsampwidth()
    raw = w.readframes(n)
    w.close()
    if sw == 2:
        samples = [raw[2*i] | (raw[2*i+1] << 8) for i in range(n)]
        samples = [s - 65536 if s >= 32768 else s for s in samples]
    else:
        samples = list(raw)
    return samples, w.getframerate()

def f0_track(x, sr, frame=320, hop=160, fmin=70, fmax=500):
    """自相关帧基频 (16k, 20ms 窗 10ms 跳). 返回 [(t_ms, f0)] 只保留有声帧."""
    out = []
    lag_min, lag_max = int(sr / fmax), int(sr / fmin)
    for start in range(0, len(x) - frame, hop):
        seg = x[start:start + frame]
        m = sum(seg) / len(seg)
        seg = [s - m for s in seg]
        e = sum(s * s for s in seg)
        if e < 1e6 * len(seg) / 320 * 40:  # 能量门限 (相对静音)
            continue
        best, best_r = 0, 0.0
        for lag in range(lag_min, lag_max):
            if start + lag + frame > len(x):
                break
            r = sum(seg[i] * x[start + lag + i] for i in range(frame - lag))
            r /= (frame - lag)
            if r > best_r:
                best_r, best = r, lag
        if best:
            out.append((start * 1000.0 / sr, sr / best))
    return out

def describe(path):
    x, sr = load(path)
    tr = f0_track(x, sr)
    if not tr:
        return f"{path}: 无有声帧"
    fs = [f for _, f in tr]
    # 轮廓摘要: 前1/3 中1/3 后1/3 均值 + 首尾差
    k = len(fs)
    a = sum(fs[:k//3]) / max(k//3, 1)
    b = sum(fs[k//3:2*k//3]) / max(k//3, 1)
    c = sum(fs[2*k//3:]) / max(k - 2*k//3, 1)
    trend = c - a
    verdict = []
    if trend > 25: verdict.append("升↗(t2)")
    elif trend < -25: verdict.append("降↘(t4)")
    else:
        dip = min(fs[k//4:3*k//4]) if k > 4 else min(fs)
        if dip < a * 0.82: verdict.append("降升∨(t3)")
        else: verdict.append("平→(t1)")
    return f"{path}: {k}帧 F0前/中/后={a:.0f}/{b:.0f}/{c:.0f}Hz Δ={trend:+.0f} → {' '.join(verdict)}"

for p in sys.argv[1:]:
    print(describe(p))
