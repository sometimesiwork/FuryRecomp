import struct, math, sys
fn = sys.argv[1] if len(sys.argv) > 1 else 'wav_wide.wav'
b = open(fn, 'rb').read()[44:]
vals = struct.unpack('<%dh' % (len(b) // 2), b)
step = 2 * 49716
pk_all = 0
clip = 0
for i in range(0, len(vals), step):
    ch = vals[i:i + step]
    L = ch[0::2]
    R = ch[1::2]
    pk = max(max(L), -min(L), max(R), -min(R))
    pk_all = max(pk_all, pk)
    if pk > 32500:
        clip += 1
    rms = math.sqrt(sum(x * x for x in L[::37]) / len(L[::37]))
    print('t=%2ds pk=%5d rms=%6.0f' % (i // (2 * 49716), pk, rms))

# band-limited side analysis over the whole file (decimate x16)
L = vals[0::2][::16]
R = vals[1::2][::16]
side = [l - r for l, r in zip(L, R)]
mono = [(l + r) / 2 for l, r in zip(L, R)]

def onepole(sig, k):
    out = []
    y = 0.0
    for x in sig:
        y += k * (x - y)
        out.append(y)
    return out

lows = onepole(side, 0.0373)
lows2 = onepole(lows, 0.0373)
highb = [a - b for a, b in zip(lows2, side)]
high_side_rms = math.sqrt(sum(x * x for x in highb) / len(highb))
side_rms = math.sqrt(sum(x * x for x in side) / len(side))
mono_rms = math.sqrt(sum(x * x for x in mono) / len(mono))
low_side_rms = math.sqrt(max(side_rms * side_rms - high_side_rms * high_side_rms, 0.0))
print('PEAK:%d clip_secs:%d whole-file: sideRMS=%.0f hiSideRMS(>300Hz)=%.0f lowSideRMS(<300Hz)=%.0f lowside-share=%.1f%% monoRMS=%.0f'
      % (pk_all, clip, side_rms, high_side_rms, low_side_rms, 100 * low_side_rms / max(side_rms, 1e-9), mono_rms))
