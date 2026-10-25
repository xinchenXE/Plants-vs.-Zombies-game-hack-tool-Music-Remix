#!/usr/bin/env python3
"""XM (FastTracker 2) 模块解析 / 统计工具

用途：把要移植的 XM 摸清楚——通道数、采样数据量、用到了哪些效果、
包络与音量列的用法，以及每个 pattern 的起始位置（方便做数据段布局）。

用法：
  python xminfo.py module.xm            # 概要
  python xminfo.py module.xm --json out.json
"""
import sys, json, struct
from collections import Counter


def read_xm(path):
    d = open(path, 'rb').read()
    assert d[:17] == b'Extended Module: ', 'not an XM'
    o = 17
    name = d[o:o+20].split(b'\0')[0].decode('latin1'); o += 20
    assert d[o] == 0x1A; o += 1
    tracker = d[o:o+20].split(b'\0')[0].decode('latin1'); o += 20
    ver = struct.unpack_from('<H', d, o)[0]; o += 2
    hsize = struct.unpack_from('<I', d, o)[0]; o += 4
    songlen, restart, channels, npat, ninst, flags, tempo, bpm = struct.unpack_from('<8H', d, o); o += 16
    orders = list(d[o:o+256]); o += 256
    if hsize > 276:
        o += hsize - 276

    patterns = []
    for _ in range(npat):
        p = {}
        p['hsize'] = struct.unpack_from('<I', d, o)[0]
        p['pack'] = d[o+4]
        p['rows'] = struct.unpack_from('<H', d, o+5)[0]
        p['size'] = struct.unpack_from('<H', d, o+7)[0]
        p['off'] = o + p['hsize']
        patterns.append(p)
        o = p['off'] + p['size']

    insts = []
    for _ in range(ninst):
        ins = {}
        ibase = o
        ins['size'] = struct.unpack_from('<I', d, ibase)[0]
        ins['name'] = d[ibase+4:ibase+26].split(b'\0')[0].decode('latin1')
        ins['type'] = d[ibase+26]
        nsamp = struct.unpack_from('<H', d, ibase+27)[0]
        ins['nsamples'] = nsamp
        o = ibase + ins['size']
        if nsamp:
            shsize = struct.unpack_from('<I', d, ibase+29)[0]
            keymap = list(d[ibase+33:ibase+33+96])
            vol_env = list(d[ibase+129:ibase+129+48])
            pan_env = list(d[ibase+177:ibase+177+48])
            nvol = vol_env[0]; npan = pan_env[0]
            vs = vol_env[1]; vp = vol_env[2]
            ps = pan_env[1]; pp = pan_env[2]
            vpts = [tuple(vol_env[4+i*4:8+i*4]) for i in range(min(vs, 12))]
            ppts = [tuple(pan_env[4+i*4:8+i*4]) for i in range(min(ps, 12))]
            ins.update(dict(shsize=shsize, nvol=nvol, vol_type=vol_env[3], vol_pts=vpts,
                            npan=npan, pan_type=pan_env[3], pan_pts=ppts,
                            vol_sus=vs, vol_loop_s=vp, vol_loop_e=vol_env[3],
                            vib_type=d[ibase+235], vib_sweep=d[ibase+236],
                            vib_depth=d[ibase+237], vib_rate=d[ibase+238],
                            vol_fade=struct.unpack_from('<H', d, ibase+239)[0],
                            keymap=keymap))
            samples = []
            so = o
            for _ in range(nsamp):
                slen, sloop, slooplen = struct.unpack_from('<3I', d, so)
                samples.append(dict(
                    name=d[so+18:so+40].split(b'\0')[0].decode('latin1'),
                    len=slen, loop=sloop, looplen=slooplen,
                    vol=d[so+12],
                    fine=struct.unpack_from('<b', d, so+13)[0],
                    type=d[so+14],
                    pan=d[so+15],
                    relnote=struct.unpack_from('<b', d, so+16)[0],
                    bits=16 if d[so+14] & 16 else 8,
                    looped=bool(d[so+14] & 3)))
                so += shsize
            data_off = so
            ins['data_off'] = data_off
            ins['samples'] = samples
            o = data_off + sum(s['len'] for s in samples)
        insts.append(ins)

    return dict(name=name, tracker=tracker, version='%X.%02X' % (ver >> 8, ver & 0xFF),
                songlen=songlen, restart=restart, channels=channels, npat=npat,
                ninst=ninst, flags=flags, tempo=tempo, bpm=bpm, orders=orders[:songlen],
                patterns=patterns, instruments=insts, file_size=len(d), data=d)


def scan_effects(xm):
    d = xm['data']
    eff = Counter(); volcol = Counter(); notes = 0; cells = 0
    for p in xm['patterns']:
        if p['pack'] != 0:
            continue
        o = p['off']; end = o + p['size']
        for r in range(p['rows']):
            for c in range(xm['channels']):
                if o >= end:
                    break
                b = d[o]
                if b & 0x80:
                    note = b & 1; ins = b & 2; vc = b & 4; ef = b & 8; prm = b & 16
                    o += 1
                    n = i = v = e = pa = 0
                    if note: n = d[o]; o += 1
                    if ins:  i = d[o]; o += 1
                    if vc:   v = d[o]; o += 1
                    if ef:   e = d[o]; o += 1
                    if prm:  pa = d[o]; o += 1
                else:
                    n, i, v, e, pa = d[o], d[o+1], d[o+2], d[o+3], d[o+4]; o += 5
                cells += 1
                if n and n != 97:
                    notes += 1
                if v:
                    volcol[v >> 4] += 1
                if e:
                    eff[e] += 1
                    eff['%02X:%X' % (e, pa >> 4)] += 1
    return dict(cells=cells, notes=notes,
                effects={k: v for k, v in eff.items() if isinstance(k, int)},
                effects_ext={k: v for k, v in eff.items() if not isinstance(k, int)},
                volcol={k: v for k, v in volcol.items()})


def main():
    path = sys.argv[1]
    xm = read_xm(path)
    eff = scan_effects(xm)
    samp = [s for i in xm['instruments'] for s in i.get('samples', [])]
    tot = sum(s['len'] for s in samp)
    print('文件        : %s (%.1f KB)' % (path, xm['file_size'] / 1024))
    print('标题        : %s' % xm['name'])
    print('跟踪器      : %s  fmt %s' % (xm['tracker'], xm['version']))
    print('通道/pattern: %d ch, %d patterns, 顺序 %d 项, 循环到 %d' % (
        xm['channels'], xm['npat'], xm['songlen'], xm['restart']))
    print('速度        : tempo %d, bpm %d, flags 0x%X' % (xm['tempo'], xm['bpm'], xm['flags']))
    print('乐器/采样   : %d inst, %d samples, 采样数据 %d 字节' % (xm['ninst'], len(samp), tot))
    print('顺序表      : %s' % xm['orders'])
    print('单元/音符   : %d / %d (%.0f%% 有音符)' % (eff['cells'], eff['notes'],
                                                   100.0 * eff['notes'] / max(1, eff['cells'])))
    print('效果        : %s' % sorted(eff['effects'].items(), key=lambda kv: -kv[1]))
    print('效果细项    : %s' % sorted(eff['effects_ext'].items(), key=lambda kv: -kv[1])[:20])
    print('音量列      : %s' % sorted(eff['volcol'].items(), key=lambda kv: -kv[1]))
    print('--- 乐器 ---')
    for i, ins in enumerate(xm['instruments']):
        if not ins['nsamples']:
            print('%2d %-20s (空)' % (i + 1, ins['name'])); continue
        print('%2d %-20s smp=%d vol_env n=%d type=%d pts=%s fade=%d vib %d/%d/%d/%d pan_env n=%d' % (
            i + 1, ins['name'], ins['nsamples'], ins['nvol'], ins['vol_type'],
            ins['vol_pts'][:3], ins['vol_fade'], ins['vib_type'], ins['vib_sweep'],
            ins['vib_depth'], ins['vib_rate'], ins['npan']))
        for s in ins['samples']:
            print('     %-18s %6d %2dbit loop %d+%d vol %3d fine %+d pan %3d rel %+d' % (
                s['name'], s['len'], s['bits'], s['loop'], s['looplen'],
                s['vol'], s['fine'], s['pan'], s['relnote']))
    if '--json' in sys.argv:
        out = sys.argv[sys.argv.index('--json') + 1]
        meta = {k: v for k, v in xm.items() if k != 'data'}
        json.dump(dict(meta, stats=eff), open(out, 'w'), ensure_ascii=False, indent=1)
        print('json ->', out)


if __name__ == '__main__':
    main()
