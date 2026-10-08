import gzip, struct, numpy as np, sys, os
def zz(a):
    a = a.astype(np.int32); return (a >> 1) ^ (-(a & 1))
def decode(path):
    b = gzip.open(path).read()
    mn, mx = struct.unpack_from('<ff', b, 24); n = struct.unpack_from('<I', b, 88)[0]; o = 92
    arr = [np.frombuffer(b, '<u2', n, o + i * 2 * n) for i in range(3)]
    u, v, h = (np.cumsum(zz(a)) for a in arr)
    return u, v, mn + h / 32767 * (mx - mn)
def bounds(z, x, y):
    s = 180.0 / (1 << z); return (-180 + x * s, -90 + y * s, -180 + (x + 1) * s, -90 + (y + 1) * s)
if __name__ == '__main__':
    root = sys.argv[1]; z = 16
    xs = sorted(int(d) for d in os.listdir(f'{root}/{z}')); x = xs[len(xs)//2]
    ys = sorted(int(f.split('.')[0]) for f in os.listdir(f'{root}/{z}/{x}')); y = ys[len(ys)//2]
    uA, vA, hA = decode(f'{root}/{z}/{x}/{y}.terrain'); uB, vB, hB = decode(f'{root}/{z}/{x+1}/{y}.terrain')
    print('tile', z, x, y, 'u range', uA.min(), uA.max(), 'v range', vA.min(), vA.max(), 'n', len(uA))
    eA = sorted(zip(vA[uA == 32767], hA[uA == 32767])); eB = sorted(zip(vB[uB == 0], hB[uB == 0]))
    print('east edge A', [(int(a), round(float(b), 2)) for a, b in eA][:8]); print('west edge B', [(int(a), round(float(b), 2)) for a, b in eB][:8])
    print('h range A', hA.min(), hA.max())
