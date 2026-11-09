"""离线 KWS 可行性试探：MFCC + DTW 能否分开「开灯 / 关灯」。
只读 server/kaitest.wav 与 guantest.wav，不写任何工程文件。
"""
import wave
import numpy as np

SR = 16000
NFFT = 512
FRAME = 400        # 25 ms
HOP = 160          # 10 ms
NMEL = 26
NCEP = 13


def load(path):
    with wave.open(path) as w:
        raw = w.readframes(w.getnframes())
    x = np.frombuffer(raw, dtype="<i2").astype(np.float32)
    return x


def trim(x, thresh_ratio=0.08):
    """按短时能量裁掉首尾静音，返回有效段。"""
    e = np.convolve(x ** 2, np.ones(160) / 160, mode="same")
    peak = e.max()
    if peak <= 0:
        return x
    idx = np.where(e > peak * thresh_ratio)[0]
    if len(idx) == 0:
        return x
    a, b = idx[0], idx[-1]
    pad = 800
    return x[max(0, a - pad): min(len(x), b + pad)]


def mel_filterbank(sr, nfft, nmel, fmin=60, fmax=7600):
    def hz2mel(f):
        return 2595 * np.log10(1 + f / 700.0)

    def mel2hz(m):
        return 700 * (10 ** (m / 2595.0) - 1)

    mels = np.linspace(hz2mel(fmin), hz2mel(fmax), nmel + 2)
    hz = mel2hz(mels)
    bins = np.floor((nfft + 1) * hz / sr).astype(int)
    fb = np.zeros((nmel, nfft // 2 + 1))
    for i in range(nmel):
        l, c, r = bins[i], bins[i + 1], bins[i + 2]
        if c == l:
            c = l + 1
        if r == c:
            r = c + 1
        for k in range(l, c):
            fb[i, k] = (k - l) / (c - l)
        for k in range(c, r):
            fb[i, k] = (r - k) / (r - c)
    return fb


FB = mel_filterbank(SR, NFFT, NMEL)
HAMM = np.hamming(FRAME)


def mfcc(x):
    x = np.append(x[0], x[1:] - 0.97 * x[:-1])          # 预加重
    n = 1 + max(0, (len(x) - FRAME) // HOP)
    frames = np.stack([x[i * HOP: i * HOP + FRAME] for i in range(n)])
    frames = frames * HAMM
    spec = np.abs(np.fft.rfft(frames, NFFT)) ** 2
    mel = spec @ FB.T
    logmel = np.log(mel + 1e-8)
    # DCT-II
    k = np.arange(NCEP)[:, None]
    nn = np.arange(NMEL)[None, :]
    dct = np.cos(np.pi * k * (2 * nn + 1) / (2 * NMEL)) * np.sqrt(2.0 / NMEL)
    c = logmel @ dct.T
    c[:, 0] = np.log(mel.sum(axis=1) + 1e-8)            # c0 换成帧能量
    c = c - c.mean(axis=0, keepdims=True)               # CMN
    return c


def dtw(a, b, band_ratio=0.25):
    na, nb = len(a), len(b)
    band = max(10, int(max(na, nb) * band_ratio))
    INF = 1e18
    prev = np.full(nb + 1, INF)
    prev[0] = 0.0
    for i in range(1, na + 1):
        cur = np.full(nb + 1, INF)
        lo = max(1, i - band)
        hi = min(nb, i + band)
        for j in range(lo, hi + 1):
            d = np.linalg.norm(a[i - 1] - b[j - 1])
            cur[j] = d + min(prev[j], cur[j - 1], prev[j - 1])
        prev = cur
    return prev[nb] / (na + nb)


def add_noise(x, snr_db):
    p = np.mean(x ** 2)
    np.random.seed(0)
    n = np.random.randn(len(x)).astype(np.float32)
    n *= np.sqrt(p / (10 ** (snr_db / 10)) / np.mean(n ** 2))
    return x + n


kai = trim(load("kaitest.wav"))
guan = trim(load("guantest.wav"))
print(f"kai {len(kai)/SR:.2f}s  guan {len(guan)/SR:.2f}s")

K, G = mfcc(kai), mfcc(guan)
print(f"frames kai={len(K)} guan={len(G)}")

d_cross = dtw(K, G)
# 自相似：同一段前后半各自做模板（模拟"同一个人说两遍"的下限）
h = len(kai) // 2
d_self_k = dtw(mfcc(kai[:h]), mfcc(kai[h:]))
d_self_g = dtw(mfcc(guan[:h]), mfcc(guan[h:]))
print(f"DTW 跨词(开灯 vs 关灯) = {d_cross:.2f}")
print(f"DTW 同词(开灯 前半 vs 后半) = {d_self_k:.2f}")
print(f"DTW 同词(关灯 前半 vs 后半) = {d_self_g:.2f}")
print(f"区分度倍数 = {d_cross / max(d_self_k, d_self_g):.2f}x")

print("\n-- 加噪鲁棒性（噪声加在语音上，看跨词距离是否仍显著大于同词）--")
for snr in (20, 10, 5, 0):
    kn = mfcc(add_noise(kai, snr))
    gn = mfcc(add_noise(guan, snr))
    cross = dtw(kn, gn)
    same = dtw(kn, K)
    print(f"SNR {snr:2d} dB: 跨词={cross:6.2f}  同词={same:6.2f}  倍数={cross/max(same,1e-6):5.2f}x")

print("\n-- 语速扰动（±15% 重采样，看跨词是否仍可区分）--")
for rate in (0.85, 1.15):
    idx = np.arange(0, len(kai), rate)
    kw = mfcc(np.interp(idx, np.arange(len(kai)), kai).astype(np.float32))
    print(f"速率 {rate:.2f}: 跨词={dtw(kw, G):6.2f}  同词={dtw(kw, K):6.2f}")
