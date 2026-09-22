#!/usr/bin/env python3
"""E426: 규칙이 요구하는 '추가 측정'의 비용과 값어치를 센다.

검증기는 "찍힌 후보 전부"를 재야 0.00%에 도달한다고 말한다. 그런데 그것이
단계당 몇 번이고, 각 후보가 실제로 이기기는 하는지는 세어 본 적이 없다.
발동하지만 **한 번도 이기지 않는** 조항이 있다면 E424의 죽은 가지와 같은 것이다.
"""
import sys, os, re, subprocess, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from measured import WORKLOADS, kc_of
from validate_output import run_tool, parse

def main(extra):
    tot_stages = tot_meas = 0
    win = collections.Counter(); fire = collections.Counter()
    rows = []
    for name, w in WORKLOADS.items():
        txt = run_tool(w['args'], w['m'], extra, w.get('M', 128))
        plan = parse(txt)
        if len(plan) != len(w['stages']): print(f"{name}: 단계 수 불일치"); continue
        first_t = best_t = 0.0; nm = 0
        for i, cands in enumerate(plan):
            avail = [(rank, blk, kc) for rank,(blk,kc) in enumerate(cands)
                     if blk in w['t'] and kc == kc_of(w, i, blk)]
            if not avail: continue
            times = [(w['t'][blk][i], rank) for rank,blk,kc in avail]
            bt, brank = min(times)
            first_t += times[0][0]; best_t += bt
            nm += len(avail); tot_stages += 1
            win[brank] += 1
            for rank,_,_ in avail: fire[rank] += 1
        tot_meas += nm
        rows.append((name, len(plan), nm, first_t, best_t))
    print(f"{'워크로드':<16}{'단계':>5}{'측정 횟수':>10}{'단계당':>8}{'1순위만':>10}{'최선':>10}{'절감':>9}")
    for name, ns, nm, f, b in rows:
        print(f"{name:<16}{ns:5d}{nm:10d}{nm/ns:8.2f}{f:10.2f}{b:10.2f}{(f/b-1)*100:+8.2f}%")
    print(f"\n총 단계 {tot_stages}개, 총 측정 {tot_meas}회 (단계당 {tot_meas/tot_stages:.2f})")
    print(f"\n{'순위':>5}{'찍힌 횟수':>10}{'이긴 횟수':>10}{'승률':>8}")
    for r in sorted(fire):
        print(f"{r+1:5d}{fire[r]:10d}{win[r]:10d}{win[r]/fire[r]*100:7.0f}%")
    dead = [r for r in sorted(fire) if win[r] == 0]
    print(f"\n한 번도 이기지 않은 순위: {[r+1 for r in dead] if dead else '없음'}")

if __name__ == '__main__':
    main(sys.argv[1:] or [])
