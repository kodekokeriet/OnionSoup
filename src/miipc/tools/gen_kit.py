#!/usr/bin/env python3
"""Synthesizes a small, original drum kit (no third-party samples)."""
import math, os, random, struct, sys, wave

RATE = 44100
out = sys.argv[1] if len(sys.argv) > 1 else "samples/kit01"
os.makedirs(out, exist_ok=True)
rnd = random.Random(7)

def write(name, data):
    peak = max(1e-9, max(abs(x) for x in data))
    with wave.open(os.path.join(out, name + ".wav"), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(x / peak * 30000)) for x in data))

def env(t, d): return math.exp(-t / d)
def noise(): return rnd.uniform(-1, 1)

def kick(f0=150, f1=45, dur=0.6, click=0.3):
    ph, o = 0, []
    for i in range(int(RATE * dur)):
        t = i / RATE
        ph += 2 * math.pi * (f1 + (f0 - f1) * env(t, 0.04)) / RATE
        o.append(math.sin(ph) * env(t, 0.18) + click * noise() * env(t, 0.003))
    return o

def snare(tone=190, dur=0.35):
    lp, o = 0, []
    for i in range(int(RATE * dur)):
        t = i / RATE
        n = noise(); lp += 0.5 * (n - lp)
        o.append(0.6 * math.sin(2 * math.pi * tone * t) * env(t, 0.05) + (n - lp) * env(t, 0.09))
    return o

def hat(dur, decay):
    prev, o = 0, []
    for i in range(int(RATE * dur)):
        t = i / RATE
        # metallic: sum of square waves + noise, high-passed
        m = sum(1 if math.sin(2 * math.pi * f * t) > 0 else -1 for f in (2050, 3010, 4120, 5370, 6820))
        x = 0.3 * m + noise()
        o.append((x - prev) * env(t, decay)); prev = x
    return o

def clap():
    lp, o = 0, []
    for i in range(int(RATE * 0.4)):
        t = i / RATE
        n = noise(); lp += 0.3 * (n - lp)
        burst = max(env(max(0, t - d), 0.008) if t >= d else 0 for d in (0, 0.011, 0.022))
        o.append((n - lp) * (burst + 0.5 * env(max(0, t - 0.03), 0.12) * (t > 0.03)))
    return o

def tom(f):
    return [math.sin(2 * math.pi * (f * (1 + 0.5 * env(i / RATE, 0.03))) * i / RATE) * env(i / RATE, 0.2)
            for i in range(int(RATE * 0.5))]

def bass(f, dur=0.8):
    o = []
    for i in range(int(RATE * dur)):
        t = i / RATE
        o.append(math.tanh(2.5 * math.sin(2 * math.pi * f * t)) * env(t, 0.35))
    return o

def stab(freqs, dur=0.6):
    o = []
    for i in range(int(RATE * dur)):
        t = i / RATE
        s = sum(((f * t) % 1.0) * 2 - 1 for f in freqs) / len(freqs)
        o.append(s * env(t, 0.15))
    return o

kit = [
    ("01_kick", kick()), ("02_kick_punch", kick(220, 55, 0.4, 0.6)),
    ("03_snare", snare()), ("04_snare_hi", snare(260, 0.25)),
    ("05_clap", clap()), ("06_rim", [math.sin(2*math.pi*1700*i/RATE)*env(i/RATE,0.012) for i in range(int(RATE*0.1))]),
    ("07_hat_closed", hat(0.12, 0.02)), ("08_hat_open", hat(0.6, 0.15)),
    ("09_tom_lo", tom(110)), ("10_tom_mid", tom(160)), ("11_tom_hi", tom(220)),
    ("12_shaker", [noise() * math.sin(math.pi * i / (RATE * 0.08)) for i in range(int(RATE * 0.08))]),
    ("13_bass_c", bass(65.41)), ("14_bass_g", bass(98.0)),
    ("15_stab_min", stab([261.6, 311.1, 392.0])), ("16_stab_maj", stab([261.6, 329.6, 392.0])),
]
for name, data in kit:
    write(name, data)
print("wrote %d samples to %s" % (len(kit), out))

# A 2-bar drum loop (95 BPM) to practise chopping on.
loop_dir = os.path.join(os.path.dirname(out.rstrip("/")), "loops")
os.makedirs(loop_dir, exist_ok=True)
bpm, bars = 95, 2
step = int(RATE * 60 / bpm / 4)
loop = [0.0] * (step * 16 * bars + RATE // 2)
k, sn, hh, oh = dict(kit)["01_kick"], dict(kit)["03_snare"], dict(kit)["07_hat_closed"], dict(kit)["08_hat_open"]
def mix(sig, at, gain):
    for i, v in enumerate(sig):
        if at + i < len(loop): loop[at + i] += v * gain
for b in range(bars):
    o = b * 16
    for s in (0, 7, 10) if b == 0 else (0, 3, 10, 13): mix(k, (o + s) * step, 1.0)
    for s in (4, 12): mix(sn, (o + s) * step, 0.8)
    for s in range(0, 16, 2): mix(hh if s != 14 else oh, (o + s) * step, 0.35)
loop = loop[: step * 16 * bars]
peak = max(abs(x) for x in loop)
with wave.open(os.path.join(loop_dir, "loop01_95bpm.wav"), "wb") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
    w.writeframes(b"".join(struct.pack("<h", int(x / peak * 30000)) for x in loop))
print("wrote loop to", loop_dir)
