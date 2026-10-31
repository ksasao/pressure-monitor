#!/usr/bin/env python3
"""
logbin.py - 基板の記録 (/log.bin の形) を、CSV に戻す

基板 (esp32c3-bmp581) の /log.csv と同じ内容 (1 バイトも違わない) の CSV を作ります。
計算は、表示ページの JavaScript (monitor_page.cpp の binToCsv) と、基板 (devlog.cpp の nextSample、
makeLine) と同じです。記録の形式は firmware/esp32c3-bmp581/devlog.h を見てください。

  python tools/logbin.py log.bin                 log.csv を作る
  python tools/logbin.py log.bin out.csv         出力のファイル名を指定する

serial-command.py の save は、これを使って、シリアルで受け取った記録を CSV にします。
"""
import struct
import sys
from datetime import datetime, timezone


class LogBinError(Exception):
    pass


def _tdiv(a, b):
    """C の整数の割り算 (0 の方向に切り捨て)"""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


def _cmod(a, b):
    """C の % (符号は割られる数と同じ)"""
    return a - b * _tdiv(a, b)


def _z(v, w):
    """C の %0Nd (幅は符号を含む)"""
    s = str(abs(v))
    return ('-' + s.zfill(w - 1)) if v < 0 else s.zfill(w)


def _i16(v):
    return ((v + 0x8000) & 0xFFFF) - 0x8000


def _rsa(v, s):
    """ゼロから遠ざかる方向に丸めた右シフト (high_pass_filter.cpp の roundShiftAway)"""
    r = (abs(v) + (1 << (s - 1))) >> s
    return -r if v < 0 else r


def bin_to_csv(buf, progress=None):
    """/log.bin の中身 (bytes) から、CSV (bytes) を作る。progress(割合) を、ときどき呼ぶ"""
    u = buf
    if len(u) < 20 or u[0:4] != b'PMLB' or u[4] != 1:
        raise LogBinError('形式が違います (/log.bin ではありません)')
    L = u[5]
    head_len, lines, end = struct.unpack_from('<III', u, 8)
    B = 20 + head_len                   # 記録 (フラッシュの先頭) の位置
    if B + end > len(u) or end < 4096 + 64:
        raise LogBinError('記録が途中で切れています')
    head = bytes(u[20:B])

    # ヘッダ (LogHeader) と、停止の情報 (StopInfo)
    ver, ses = u[B + 4], u[B + 5]
    tz, = struct.unpack_from('<h', u, B + 6)
    start, = struct.unpack_from('<q', u, B + 8)
    iv = struct.unpack_from('<I', u, B + 53)[0] if ver >= 4 else 0
    if iv == 0xFFFFFFFF:
        iv = 0
    stop_rel, = struct.unpack_from('<I', u, B + 68)
    stop_phone, = struct.unpack_from('<q', u, B + 72)
    corr = u[B + 64] == 0 and stop_phone > 0 and stop_rel > 0
    err = stop_phone - (start + stop_rel) if corr else 0

    # HPF (high_pass_filter.cpp と同じ)
    hp = {'px': struct.unpack_from('<q', u, B + 16)[0], 'y': struct.unpack_from('<q', u, B + 24)[0],
          'init': u[B + 32] != 0}

    def hpf(x, sh):
        sh = min(max(sh, 1), 30)
        cx = x * 256
        if not hp['init']:
            hp['px'], hp['y'], hp['init'] = cx, 0, True
        dec = _rsa(hp['y'], sh)
        hp['y'] = hp['y'] + (cx - hp['px']) - dec
        hp['px'] = cx
        return _rsa(hp['y'], 8)

    def u24(o):
        return u[o] | (u[o + 1] << 8) | (u[o + 2] << 16)

    # 次のサンプル (S に入れる)。終わりなら False
    S = dict(off=4096, inF=False, n=0, idx=0, kind=0, pos=0, pe=0, rel=0, raw=0, sh=8, t=0, ah=0,
             avgHave=False, bat=0, r0=0, per=0)

    def nxt():
        while not S['inF'] or S['idx'] > S['n']:
            S['inF'] = False
            if S['off'] + 6 > end:
                return False
            o = B + S['off']
            tag = u[o]
            if u[o + 1] != ses:
                return False
            if tag == 0xB0:                                     # 再起動
                hp['px'], hp['y'], hp['init'] = 0, 0, False
                S['off'] += 6
                continue
            if tag == 0xBA:                                     # 電池電圧
                S['bat'], = struct.unpack_from('<H', u, o + 2)
                S['off'] += 4
                continue
            if tag == 0xA7:                                     # 平均
                if S['off'] + 14 > end:
                    return False
                S['rel'], = struct.unpack_from('<I', u, o + 2)
                S['raw'] = u24(o + 6)
                S['t'], = struct.unpack_from('<h', u, o + 9)
                a = u24(o + 11)
                S['ah'] = a - (1 << 24) if a & 0x800000 else a
                S.update(kind=2, avgHave=True, n=0, idx=0, inF=True)
                S['off'] += 14
                continue
            if tag == 0xA8:                                     # 平均 (前との差)
                if not S['avgHave'] or S['off'] + 8 > end:
                    return False
                d_rel, d_raw, d_t, a = struct.unpack_from('<bhbh', u, o + 2)
                S['rel'] = (S['rel'] + d_rel + iv) & 0xFFFFFFFF
                S['raw'] += d_raw
                S['t'] = _i16(S['t'] + d_t)
                S['ah'] = a
                S.update(kind=2, n=0, idx=0, inF=True)
                S['off'] += 8
                continue
            if tag == 0xA9:                                     # 毎サンプル (版 6。センサの間隔)
                if S['off'] + 17 > end:
                    return False
                fl, = struct.unpack_from('<H', u, o + 13)
                S['n'] = u[o + 2]
                if S['n'] > 239 or fl > 733 or S['off'] + 17 + fl > end:
                    return False
                S.update(kind=3, pos=o + 17, pe=o + 17 + fl, sh=u[o + 3], idx=0, inF=True)
                S['r0'], = struct.unpack_from('<I', u, o + 4)
                S['per'], = struct.unpack_from('<H', u, o + 15)
                S['rel'] = S['r0']
                S['raw'] = u24(o + 8)
                S['t'], = struct.unpack_from('<h', u, o + 11)
                S['off'] += 17 + fl
                continue
            if tag == 0xA6:                                     # 毎サンプル (版 5)
                if S['off'] + 15 > end:
                    return False
                bl, = struct.unpack_from('<H', u, o + 13)
                S['n'] = u[o + 2]
                if S['n'] > 119 or bl > 484 or S['off'] + 15 + bl > end:
                    return False
                S.update(kind=1, pos=o + 15, pe=o + 15 + bl, sh=u[o + 3], idx=0, inF=True)
                S['rel'], = struct.unpack_from('<I', u, o + 4)
                S['raw'] = u24(o + 8)
                S['t'], = struct.unpack_from('<h', u, o + 11)
                S['off'] += 15 + bl
                continue
            if tag != 0xA5:                                     # 毎サンプル (版 4 まで)
                return False
            S['n'] = u[o + 2]
            if S['n'] > 29:
                return False
            ln = 13 + S['n'] * 3
            if S['off'] + ln > end:
                return False
            S.update(kind=0, pos=o + 13, sh=u[o + 3], idx=0, inF=True)
            S['rel'], = struct.unpack_from('<I', u, o + 4)
            S['raw'] = u24(o + 8)
            S['t'], = struct.unpack_from('<h', u, o + 11)
            S['off'] += ln

        if S['idx'] > 0 and S['kind'] in (1, 3):
            # 符号を 1 つ読む (devlog.cpp の unfifoCode、unpackSample)
            p, e = S['pos'], S['pe']
            while e - p >= 2 and u[p] == 0xC1:
                S['t'] = _i16(S['t'] + struct.unpack_from('<b', u, p + 1)[0])
                p += 2
            if p >= e:
                return False
            c = u[p]
            if S['kind'] == 3:
                if c < 0x80:
                    d = c - 128 if c & 0x40 else c
                    p += 1
                elif c < 0xC0:
                    if e - p < 2:
                        return False
                    w = ((c & 0x3F) << 8) | u[p + 1]
                    d = w - 16384 if w >= 8192 else w
                    p += 2
                elif c == 0xC0:
                    if e - p < 3:
                        return False
                    d, = struct.unpack_from('<h', u, p + 1)
                    p += 3
                else:
                    return False
                S['rel'] = (S['r0'] + (S['idx'] * S['per'] + 500) // 1000) & 0xFFFFFFFF
            else:
                if c < 0x80:
                    dt, d = 33, (c - 128 if c & 0x40 else c)
                    p += 1
                elif c < 0xC0:
                    if e - p < 2:
                        return False
                    w = ((c & 7) << 8) | u[p + 1]
                    dt, d = 32 + ((c >> 3) & 7), (w - 2048 if w >= 1024 else w)
                    p += 2
                elif c == 0xC0:
                    if e - p < 4:
                        return False
                    dt = u[p + 1]
                    d, = struct.unpack_from('<h', u, p + 2)
                    p += 4
                else:
                    return False
                S['rel'] = (S['rel'] + dt) & 0xFFFFFFFF
            S['pos'] = p
            S['raw'] += d
        elif S['idx'] > 0:
            S['rel'] = (S['rel'] + u[S['pos']]) & 0xFFFFFFFF
            S['raw'] += struct.unpack_from('<h', u, S['pos'] + 1)[0]
            S['pos'] += 3
        S['idx'] += 1
        return True

    blank = ' ' * (L - 2) + '\r\n'
    cache = {'sec': None, 'ds': ''}

    def line():
        if not nxt():
            return blank
        h = S['ah'] if S['kind'] == 2 else hpf(S['raw'], S['sh'])
        rel = S['rel']
        unix = start + rel
        if corr:
            unix += _tdiv(err * rel, stop_rel)
        loc = unix - tz * 60000
        sec = _tdiv(loc, 1000)
        if sec != cache['sec']:
            cache['sec'] = sec
            cache['ds'] = datetime.fromtimestamp(sec, timezone.utc).strftime('%Y-%m-%d %H:%M:%S')
        pk = _tdiv(S['raw'] * 25 + 8, 16)
        hc = max(-9999999, min(9999999, h))
        tc = max(-9999, min(9999, S['t']))
        ta = abs(tc)
        s = (cache['ds'] + '.' + _z(_cmod(loc, 1000), 3) + ',' + _z(unix, 13) + ','
             + _z(min(rel, 99999999), 8) + ',' + _z(_cmod(_tdiv(pk, 10000), 10000), 4) + '.'
             + _z(_cmod(pk, 10000), 4) + ',' + _z(S['raw'], 8) + ','
             + ('-' if hc < 0 else '+') + _z(abs(hc), 7) + ','
             + ('-' if tc < 0 else '+') + _z(ta // 100, 2) + '.' + _z(ta % 100, 2) + ',')
        if ver >= 3 and len(s) < L + 8:
            s += _z(min(S['bat'], 9999), 4)             # 版 2 は空欄
        s = s[:L - 2]
        return s + ' ' * (L - 2 - len(s)) + '\r\n'

    out = [head]
    chunk = []
    for i in range(lines):
        chunk.append(line())
        if len(chunk) >= 50000:
            out.append(''.join(chunk).encode('ascii'))
            chunk = []
            if progress:
                progress((i + 1) / lines)
    out.append(''.join(chunk).encode('ascii'))
    if progress:
        progress(1.0)
    return b''.join(out)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else (src[:-4] if src.lower().endswith('.bin') else src) + '.csv'
    data = open(src, 'rb').read()
    try:
        csv = bin_to_csv(data)
    except LogBinError as e:
        sys.exit(f'{src}: {e}')
    open(dst, 'wb').write(csv)
    print(f'{dst} ({len(csv):,} バイト)')


if __name__ == '__main__':
    main()
