#!/usr/bin/env python3
"""tune.py의 선택을 이 트랙이 잰 모든 (형상 -> 블록별 최적 시간)에 대고 채점한다."""
import re, os, glob, statistics, importlib.util, sys
spec = importlib.util.spec_from_file_location("tune", os.path.join(os.path.dirname(__file__), "tune.py"))
tune = importlib.util.module_from_spec(spec); spec.loader.exec_module(tune)
D = '/srv/nfs/debian-riscv64/tmp'
pts = {}
def add(K,N,blk,ch,t):
    d = pts.setdefault((K,N),{}); d[(blk,ch)] = min(t, d.get((blk,ch), 9e9))
p1 = re.compile(r'\[\s*(\d+)x\s*(\d+)\]\s+\((\d),(\d)\) Kc=\s*(\d+) 청크\s*(\d+) nj=\s*\d+ 타일\s*\d+ \|\s+[\d.]+\s+([\d.]+)\s+\|.*PASS')
for f in ('stagetune','widetune','large','ratio','ruletest','chunkrule'):
    fp = f'{D}/{f}.log'
    if os.path.exists(fp):
        for line in open(fp):
            m = p1.match(line)
            if m: add(int(m.group(1)),int(m.group(2)),(int(m.group(3)),int(m.group(4))),int(m.group(6)),float(m.group(7)))
p3 = re.compile(r'\[\s*(\d+)x\s*(\d+)\]\s+\((\d),(\d)\) Kc=\s*(\d+) 청크\s*(\d+).*?\|\s+[\d.]+\s+([\d.]+)\s+([\d.]+)\s+\|.*PASS')
fl = ['chunkrule2','blockrule'] + [os.path.basename(x)[:-4] for x in
      glob.glob(f'{D}/br2_*.log') + glob.glob(f'{D}/br3_*.log')]
for f in fl:
    fp = f'{D}/{f}.log'
    if not os.path.exists(fp): continue
    for line in open(fp):
        m = p3.match(line)
        if m: add(int(m.group(1)),int(m.group(2)),(int(m.group(3)),int(m.group(4))),int(m.group(6)),
                  min(float(m.group(7)),float(m.group(8))))
pa = re.compile(r'^\((\d),(\d)\)\s+(\d+)\s+(\d+) \|\s+[\d.]+\s+([\d.]+)\s+([\d.]+) \| PASS')
for f in glob.glob(f'{D}/lf_*.log'):
    for line in open(f):
        m = pa.match(line)
        if m: add(2816,1024,(int(m.group(1)),int(m.group(2))),int(m.group(4)),
                  min(float(m.group(5)),float(m.group(6))))

rows = []
for (K,N), d in sorted(pts.items()):
    per = {}
    for (b,ch), t in d.items(): per[b] = min(per.get(b, 9e9), t)
    if len(per) < 2: continue
    cands = [c['block'] for c in tune.plan(K,N)['candidates']]
    have = [b for b in cands if b in per]
    if not have: continue                       # 규칙의 선택이 측정 안 됨
    bb = min(per.values())
    rows.append((K, N, min(per, key=lambda b: per[b]), have, (min(per[b] for b in have)/bb-1)*100, len(have)))
e = [r[4] for r in rows]
print(f"{'형상':16s} {'실측 최선':>8s} {'규칙 선택':>18s} {'초과':>7s}")
for K,N,bb,have,x,n in rows:
    fl2 = "  <--" if x > 3 else ""
    print(f"[{K:5d}x{N:5d}] {str(bb):>8s} {str(have):>18s} {x:+6.1f}%{fl2}")
print(f"\n형상 {len(rows)}개:  평균 초과 {statistics.mean(e):.2f}%  최대 {max(e):.1f}%  "
      f"정확(2% 안) {sum(1 for x in e if x<2)}/{len(rows)}  5% 초과 {sum(1 for x in e if x>5)}")
print(f"두 번 재는 형상: {sum(1 for r in rows if r[5]>1)}/{len(rows)}")
