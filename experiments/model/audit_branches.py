#!/usr/bin/env python3
"""tune.py의 각 분기가 측정 코퍼스에서 몇 번 밟혔는지 센다 (E329의 교훈).

규칙의 점수(32형상 평균 0.10%)는 진짜지만 모든 분기가 검정됐다는 뜻은 아니다.
E329에서 "목표에 못 가는 경우" 분기가 코퍼스에 한 번만 나타나고 측정된 적이 없었으며,
밟아 보니 틀렸다. 그런 분기가 또 있는지 센다.
"""
import re, os, glob, importlib.util, collections
spec = importlib.util.spec_from_file_location("tune", os.path.join(os.path.dirname(__file__), "tune.py"))
tune = importlib.util.module_from_spec(spec); spec.loader.exec_module(tune)
D = '/srv/nfs/debian-riscv64/tmp'

# 측정된 (형상 -> 블록별 시간)
pts = {}
def add(K,N,b,ch,t):
    d = pts.setdefault((K,N), {}); d[(b,ch)] = min(t, d.get((b,ch), 9e9))
p1 = re.compile(r'\[\s*(\d+)x\s*(\d+)\]\s+\((\d),(\d)\) Kc=\s*(\d+) 청크\s*(\d+) nj=\s*\d+ 타일\s*\d+ \|\s+[\d.]+\s+([\d.]+)\s+\|.*PASS')
for f in ('stagetune','widetune','large','ratio','ruletest','chunkrule'):
    fp = f'{D}/{f}.log'
    if os.path.exists(fp):
        for line in open(fp):
            m = p1.match(line)
            if m: add(int(m.group(1)),int(m.group(2)),(int(m.group(3)),int(m.group(4))),int(m.group(6)),float(m.group(7)))
p3 = re.compile(r'\[\s*(\d+)x\s*(\d+)\]\s+\((\d),(\d)\) Kc=\s*(\d+) 청크\s*(\d+).*?\|\s+[\d.]+\s+([\d.]+)\s+([\d.]+)\s+\|.*PASS')
for f in ['chunkrule2','blockrule'] + [os.path.basename(x)[:-4] for x in
          glob.glob(f'{D}/br2_*.log') + glob.glob(f'{D}/br3_*.log') + glob.glob(f'{D}/br62_*.log')]:
    fp = f'{D}/{f}.log'
    if not os.path.exists(fp): continue
    for line in open(fp):
        m = p3.match(line)
        if m: add(int(m.group(1)),int(m.group(2)),(int(m.group(3)),int(m.group(4))),int(m.group(6)),
                  min(float(m.group(7)),float(m.group(8))))

measured = {k for k,v in pts.items() if len(v) >= 2}
# 0회인 분기가 조용히 사라지지 않도록 이름을 먼저 등록한다 (E321의 "0의 두 가지 뜻").
BRANCHES = ["블록: 목표 도달 가능 -> 강도 최소",
            "블록: 목표 불가 -> 청크 최소 (E329)",
            "청크 목표 4 (K·N >= 1.4M)", "청크 목표 2",
            "Kc 후퇴 (목표 청크 불가)", "강도 25% 예외 (두 번 재라)",
            "강도 동률 -> 큰 I 선택"]
cnt = collections.Counter({b: 0 for b in BRANCHES}); meas = collections.Counter({b: 0 for b in BRANCHES})
for (K,N) in sorted({(K,N) for K in range(1) for N in range(1)} | set(pts)):
    cand = [b for b in tune.blocks_for(128, N) if tune.legal_kcs(K//16, *b)]
    if not cand: continue
    ok = [b for b in cand if tune.min_chunks(K,*b) <= 4]
    t = tune.chunk_target(K,N)
    def bump(name):
        cnt[name] += 1
        if (K,N) in measured: meas[name] += 1
    bump("블록: 목표 도달 가능 -> 강도 최소" if ok else "블록: 목표 불가 -> 청크 최소 (E329)")
    bump("청크 목표 4 (K·N >= 1.4M)" if t == 4 else "청크 목표 2")
    plan = tune.plan(K,N)
    if len(plan['candidates']) > 1: bump("강도 25% 예외 (두 번 재라)")
    c0 = plan['candidates'][0]
    Ktil = K//16
    if Ktil % t or (Ktil//t)*(sum(c0['block'])) > 512: bump("Kc 후퇴 (목표 청크 불가)")
    pool = ok or cand
    tie = [b for b in pool if abs(tune.intensity(b) - tune.intensity(c0['block'])) < 1e-9]
    if len(tie) > 1: bump("강도 동률 -> 큰 I 선택")
print(f"{'분기':38s} {'해당 형상':>8s} {'그중 측정됨':>10s}")
for k in BRANCHES:
    flag = "   <== **한 번도 안 밟힘**" if cnt[k] == 0 else ("   <== 검정 부족" if meas[k] < 3 else "")
    print(f"{k:38s} {cnt[k]:8d} {meas[k]:10d}{flag}")
print(f"\n전체 형상 {len(pts)}개, 그중 블록 2종 이상 측정 {len(measured)}개")
