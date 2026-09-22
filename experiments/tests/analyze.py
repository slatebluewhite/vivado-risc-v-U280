#!/usr/bin/env python3
"""sspair 로그 분석기 — 이 세션에서 세 번 밟은 오류를 자동으로 잡는다.

  ① 두 팔의 작업집합 불일치        (E742, E781: 조용한 clamp)
  ② 작업집합이 주장하는 모델과 다름 (E786, E802b: 라벨 오류)
  ③ 계획기가 고른 shape 을 안 읽음  (E801: 접기 불가인데 접힌 줄 앎)

사용:
  analyze.py --logs e802_b*.log --cells 64x64:4 64x256:1 256x64:1 --expect-kb 384
"""
import re, sys, glob, argparse, collections

def parse(path):
    """(mode,K,N,M) -> [cyc...], 그리고 셀별 팔별 작업집합·shape."""
    d = collections.defaultdict(list); ws = {}; shp = {}
    arm_ws = {}; cur = None
    for line in open(path, errors='ignore'):
        w = re.search(r'\[(ws/tile|ss) 팔\] nrot=(\d+), 회전 작업집합 (\d+) KB', line)
        if w: arm_ws[w.group(1)] = (int(w.group(2)), int(w.group(3)))
        m = re.search(r'mode=(\S+) K=(\d+) N=(\d+) M=(\d+)\s+([\d.]+) cyc\s+bad=(-?\d+)', line)
        if m:
            mode = m.group(1).split('/')[0]; K, N, M = int(m.group(2)), int(m.group(3)), int(m.group(4))
            cur = (K, N)
            if int(m.group(6)) == 0:
                d[(mode, K, N, M)].append(float(m.group(5)))
                ws.setdefault((mode, K, N), dict(arm_ws))
            # E804: 측정 하나가 끝나면 팔 기록을 비운다. 안 그러면 `tile` 실행(ss 줄이
            # 없다)이 **직전 `plan` 실행의 ss 값**을 물고 가짜 불일치를 낸다 — 도구의
            # 첫 실사용에서 이 오탐이 났다.
            arm_ws = {}
        s = re.search(r'plan shape=(\d+)', line)
        if s and cur: shp[cur] = int(s.group(1))
    return d, ws, shp

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--logs', nargs='+', required=True)
    ap.add_argument('--cells', nargs='+', required=True, help='KxN:weight')
    ap.add_argument('--M', type=int, default=1)
    ap.add_argument('--expect-kb', type=int, help='의도한 모델/작업집합 크기 (KB)')
    a = ap.parse_args()

    files = [f for p in a.logs for f in sorted(glob.glob(p))] or a.logs
    per = []; ws_all = {}; shp_all = {}
    for f in files:
        d, ws, shp = parse(f)
        per.append({k: min(v) for k, v in d.items() if v})
        ws_all.update(ws); shp_all.update(shp)

    bad = []
    for (mode, K, N), arms in ws_all.items():
        if 'ws/tile' in arms and 'ss' in arms and arms['ws/tile'][1] != arms['ss'][1]:
            bad.append(f"  ① [{K}x{N}] 두 팔 불일치: tile {arms['ws/tile'][1]} KB vs ss {arms['ss'][1]} KB")
        if a.expect_kb:
            for an, (nr, kb) in arms.items():
                if kb != a.expect_kb:
                    bad.append(f"  ② [{K}x{N}] {an}: 작업집합 {kb} KB != 의도한 {a.expect_kb} KB (nrot {nr})")
    for (K, N), s in sorted(shp_all.items()):
        legal4 = (N % 64 == 0); legal2 = (N % 32 == 0)
        note = "" if (s == 2 and legal4) or (s == 1 and legal2) or s == 0 else " ??"
        print(f"  shape [{K:4d}x{N:4d}] = {s}  (S=4 {'가능' if legal4 else '불가'}, "
              f"S=2 {'가능' if legal2 else '불가'}){note}")
        if s == 0 and not legal2:
            print(f"      -> ③ 접기 불가 셀이다. 이 셀의 이득은 **FSM 만의 몫**이다.")

    if bad:
        print("\n" + "!"*60); [print(b) for b in bad]; print("!"*60 + "\n")
    else:
        print("\n  검사 ①② 통과\n")

    mn = collections.defaultdict(list)
    for p in per:
        for k, v in p.items(): mn[k].append(v)
    best = {k: min(v) for k, v in mn.items()}
    cells = [((int(c.split(':')[0].split('x')[0]), int(c.split(':')[0].split('x')[1])),
              int(c.split(':')[1])) for c in a.cells]
    tt = pp = 0
    print("  단계        기본      접힘     접기")
    for (K, N), w in cells:
        t = best.get(('tile', K, N, a.M)); p_ = best.get(('plan', K, N, a.M))
        if t is None or p_ is None: print(f"  [{K}x{N}] 데이터 없음"); continue
        tt += t*w; pp += p_*w
        print(f"  [{K:3d}x{N:3d}] {t:8.1f} {p_:8.1f} {t/p_:7.3f}x")
    if pp: print(f"\n  레이어 접기 이득: **{tt/pp:.3f}x**  (배치 {len(files)} 개, 배치 간 최소값)")

main()
