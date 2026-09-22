#!/usr/bin/env python3
"""E394: tune.py의 **출력 텍스트**를 파싱해 채점한다.

`validate_tune.py`는 파이썬 API(`pick_blocks`)를 직접 부르지만 사용자는 화면의 글자를
읽고 그대로 잰다. 이 트랙에서 "계산은 맞는데 화면에 안 나와서 못 쓰는" 결함이 두 번
있었다(E380의 셋째 후보 누락, E393의 차점 Kc 누락). 둘 다 API 채점은 통과했다.

여기서는 서브프로세스로 tune.py를 돌리고, 찍힌 (블록, Kc) 조합만 써서 실측표와 대조한다.
"""
import re, subprocess, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from measured import WORKLOADS, kc_of

HERE = os.path.dirname(os.path.abspath(__file__))
LINE = re.compile(r'^(\S+)\s+\[\s*(\d+)x\s*(\d+)\]\s+비중\s+[\d.]+%\s+\((\d), (\d)\) Kc=(\d+)')
# 중첩 괄호가 있으므로 정규식 대신 구간을 잘라낸다 ("(+" ... " 도 재라)").
def alt_span(line):
    i = line.find('(+')
    if i < 0: return ''
    j = line.find(' 도 재라)', i)
    return line[i+2:j] if j > 0 else ''
# E424: (16,4) 힌트는 다른 형식으로 찍힌다 — `**(16, 4) Kc=NN 청크N 도 재라**`.
# 이 조항이 파서에 안 잡혀서 검증기가 **한 번도 채점하지 않고 있었다.**
WIDE = re.compile(r'\*\*\((\d+), (\d)\) Kc=(\d+) 청크\d+ 도 재라\*\*')
BLK  = re.compile(r'\((\d+), (\d)\) Kc=(\d+)')

def run_tool(args, m, extra, M=128):
    cmd = ([sys.executable, os.path.join(HERE,'tune.py')] + args
           + [f'-m{m}', f'-M{M}'] + extra)
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode: raise RuntimeError(out.stderr[:400])
    return out.stdout

def parse(text):
    """단계별 [(블록, Kc), ...] — 1순위와 찍힌 차점 전부."""
    plan = []
    for line in text.splitlines():
        m = LINE.match(line)
        if not m: continue
        cands = [((int(m.group(4)), int(m.group(5))), int(m.group(6)))]
        for b in BLK.finditer(alt_span(line)):
            cands.append(((int(b.group(1)), int(b.group(2))), int(b.group(3))))
        for b in WIDE.finditer(line):
            cands.append(((int(b.group(1)), int(b.group(2))), int(b.group(3))))
        plan.append(cands)
    return plan

def score(name, w, extra):
    txt = run_tool(w['args'], w['m'], extra, w.get('M', 128))
    plan = parse(txt)
    if len(plan) != len(w['stages']):
        return name, None, f"단계 수 불일치 ({len(plan)} 대 {len(w['stages'])})"
    tot_rule = tot_first = tot_opt = 0.0
    missing = 0
    for i, cands in enumerate(plan):
        avail = []
        for blk, kc in cands:
            if blk in w['t'] and kc == kc_of(w, i, blk): avail.append(w['t'][blk][i])
            else: missing += 1
        if not avail: return name, None, f"{w['stages'][i]}: 찍힌 조합이 모두 미측정"
        tot_rule += min(avail)
        f = cands[0]
        tot_first += w['t'][f[0]][i] if (f[0] in w['t'] and f[1]==kc_of(w,i,f[0])) else min(avail)
        tot_opt += min(w['t'][b][i] for b in w['t'])
    return name, (tot_first, tot_rule, tot_opt, missing, sum(len(c) for c in plan)), None

if __name__ == '__main__':
    extra = sys.argv[1:] or []
    lbl = ' '.join(extra) if extra else '표준 한계 (32/512)'
    print(f"tune.py **출력 텍스트** 채점 — {lbl}\n")
    print(f"{'워크로드':<12} {'1순위만':>9} {'찍힌 후보 전부':>13} {'전수 최적':>10} "
          f"{'초과':>8} {'미측정/전체':>11}")
    rows = []
    for name, w in WORKLOADS.items():
        n, r, err = score(name, w, extra)
        if err: print(f"{n:<12} {err}"); continue
        first, rule, opt, miss, tot = r
        rows.append((rule/opt-1)*100)
        print(f"{n:<12} {first:9.2f} {rule:13.2f} {opt:10.2f} "
              f"{(rule/opt-1)*100:+7.2f}% {f'{miss}/{tot}':>11}")
    if rows:
        print(f"\n{len(rows)}개 워크로드: 평균 초과 {sum(rows)/len(rows):+.2f}%, 최대 {max(rows):+.2f}%")
