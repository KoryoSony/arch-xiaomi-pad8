#!/usr/bin/env python3
# Touch decoder tests: python3 tools/touchsim.py <typing|adjacent|short|edges|flick|swipe> out.rec
# then: yupei-touchd --replay out.rec  (each scenario prints the touches it expects)
# Synthetic THP recordings: Gaussian finger blobs on a noisy baseline.
# Writes the yupei-touchd recording format and the expected touches.
import math, random, struct, sys
C, R, W, H, FPS = 60, 40, 3200, 2136, 120
random.seed(7)
def frame(contacts):
    m = []
    for r in range(R):
        for c in range(C):
            v = 10000 + random.gauss(0, 18)
            for (x, y, amp) in contacts:
                cx = x / (W - 1) * (C - 1); cy = (R - 1) - y / (H - 1) * (R - 1)
                d2 = (c - cx) ** 2 + (r - cy) ** 2
                v += amp * math.exp(-d2 / (2 * 0.75 ** 2))
            m.append(int(v))
    return m
def write(path, frames):
    with open(path, 'wb') as f:
        for i, m in enumerate(frames):
            f.write(struct.pack('<qBB', int(i * 1e9 / FPS), C, R))
            f.write(struct.pack('<%dh' % len(m), *m))
def amp_at(k, n, peak=1000):   # landing/lifting ramps: 2 frames up, 2 down
    up = min(1.0, (k + 1) / 2.0); down = min(1.0, (n - k) / 2.0)
    return peak * min(up, down)
scen = sys.argv[1]; out = sys.argv[2]
frames, expect = [], []
idle = lambda n: [frame([]) for _ in range(n)]
frames += idle(5)
if scen == 'typing':
    # fast two-thumb typing: 12 taps, 7 frames (58 ms) each, the next tap
    # lands 3 frames before the previous lifts, keys 160-450 px apart
    keys = [(400 + 260 * (i % 6), 1500 + 140 * (i % 3)) for i in range(12)]
    taps = [(4 + 4 * i, 7, keys[i]) for i in range(12)]
    n = 4 + 4 * 12 + 10
    for t in range(n):
        cs = [(x, y, amp_at(t - s, d)) for (s, d, (x, y)) in taps if s <= t < s + d]
        frames.append(frame(cs))
    expect = [k for _, _, k in taps]
elif scen == 'short':
    # very short taps: 3 frames (25 ms), light (peak 500)
    pos = [(800, 1000), (1600, 1200), (2400, 900), (1200, 1900)]
    for x, y in pos:
        frames += [frame([(x, y, amp_at(k, 3, 500))]) for k in range(3)] + idle(4)
    expect = pos
elif scen == 'edges':
    pos = [(5, 1000), (3194, 1000), (1600, 5), (1600, 2130), (40, 2100), (60, 2110)]
    for x, y in pos:
        frames += [frame([(x, y, amp_at(k, 8))]) for k in range(8)] + idle(5)
    expect = pos
elif scen == 'flick':
    # realistic fast flick: 1200 px in 200 ms (6000 px/s), one weak frame
    n = 24
    for k in range(n):
        x = 800 + 1200 * k / (n - 1)
        a = amp_at(k, n) * (0.35 if k == 12 else 1.0)
        frames.append(frame([(x, 1200, a)]))
    expect = [(800, 1200)]
elif scen == 'adjacent':
    # same-row neighbours 250 px apart, next lands the frame the previous lifts
    keys = [(1000 + 250 * i, 1700) for i in range(6)]
    t0 = 4
    for i, (x, y) in enumerate(keys):
        pass
    n = 4 + 6 * 6 + 8
    taps = [(4 + 6 * i, 6, keys[i]) for i in range(6)]
    for t in range(n):
        cs = [(x, y, amp_at(t - s, d)) for (s, d, (x, y)) in taps if s <= t < s + d]
        frames.append(frame(cs))
    expect = keys
elif scen == 'hold':
    # finger resting still for 1 s (jitter), then dragged at 1500 px/s (lag)
    for k in range(120):
        frames.append(frame([(1600.0, 1000.0, 900)]))
    for k in range(60):
        frames.append(frame([(1600.0 + 1500.0 * k / 120, 1000.0, 900)]))
    frames.append(frame([]))
    expect = [(1600, 1000)]
elif scen == 'swipe':
    # fast swipe 2400 px in 150 ms (16000 px/s), with one weak frame
    n = 18
    for k in range(n):
        x = 400 + 2400 * k / (n - 1)
        a = amp_at(k, n) * (0.35 if k == 9 else 1.0)
        frames.append(frame([(x, 1000, a)]))
    expect = [(400, 1000)]
frames += idle(5)
write(out, frames)
print(len(frames), 'frames; expected touches:', expect)
