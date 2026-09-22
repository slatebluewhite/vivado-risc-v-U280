#!/usr/bin/env python3
"""max() 대신 p-노름 — 자원이 동시에 포화할 때의 낙관을 고친다 (E208).

E203의 경합 계수 0.085는 유일한 자유 파라미터인데, 역산하면 상수가 아니다:
512³ (8,4) m=3에서 0.037, (4,4) m=3에서 0.183. 후자는 온칩 11.14 ms와 연산 11.33 ms가
거의 같은 **교차점**이고, max()는 거기서 구조적으로 낙관적이다.

    T = ( Σ t_i^p )^(1/p)

p→∞면 max(), p=1이면 합이다. 파라미터 수는 똑같이 하나인데 물리적 의미가 낫다 —
"자원들이 얼마나 겹치는가"를 나타낸다.
"""
import statistics, importlib.util, sys
spec = importlib.util.spec_from_file_location("cost", "experiments/model/cost.py")
# cost.py는 실행 시 출력하므로 stdout을 잠시 막는다
import io, contextlib
cost = importlib.util.module_from_spec(spec)
with contextlib.redirect_stdout(io.StringIO()):
    spec.loader.exec_module(cost)

def combine(parts, p):
    if p >= 50: return max(parts)
    return sum(t**p for t in parts) ** (1.0/p)

def eval_p(p):
    errs = []
    for label, N, I, J, B, ts in cost.DATA:
        t1 = ts[0]/1e3
        for m in (2,3):
            meas = ts[m-1]/1e3
            tc = t1
            to = m*cost.sq_bytes(N,I,J)/B/cost.FREQ
            ws = m*3*N*N
            td = cost.sq_dram(N,I,J,m)/cost.B_dram_of(ws)/cost.FREQ
            errs.append(abs((combine([tc,to,td],p)-meas)/meas*100))
    oos = []
    for label, M, N, K, I, J, B, ts in cost.BERT:
        t1 = ts[0]/1e3
        for m in (2,3):
            if ts[m-1] is None: continue
            meas = ts[m-1]/1e3
            tc = t1
            to = m*cost.gen_bytes(M,N,K,I,J)/B/cost.FREQ
            ws = M*K + m*(K*N + M*N)
            td = cost.gen_dram(M,N,K,m)/cost.B_dram_of(ws)/cost.FREQ
            oos.append(abs((combine([tc,to,td],p)-meas)/meas*100))
    return statistics.mean(errs), statistics.mean(oos), errs, oos

print(f"{'p':>5s} | {'표본내 평균':>10s} {'10%이내':>8s} | {'표본밖 평균':>10s} {'10%이내':>8s}")
print("-"*54)
best=None
for p in [1.0,1.5,2.0,2.5,3.0,4.0,6.0,10.0,100.0]:
    a,b,ea,eb = eval_p(p)
    tag = "  <- max()" if p>=50 else ""
    print(f"{p:5.1f} | {a:10.1f}% {sum(1 for e in ea if e<=10):5d}/40 | "
          f"{b:10.1f}% {sum(1 for e in eb if e<=10):5d}/13{tag}")
    if best is None or (a+b)/2 < best[0]: best=((a+b)/2,p,a,b,ea,eb)
print("-"*54)
_,p,a,b,ea,eb = best
print(f"최적 p = {p}:  표본내 {a:.1f}% (중앙값 {statistics.median(ea):.1f}%, "
      f"20%이내 {sum(1 for e in ea if e<=20)}/40)")
print(f"            표본밖 {b:.1f}% (중앙값 {statistics.median(eb):.1f}%, "
      f"20%이내 {sum(1 for e in eb if e<=20)}/13)")
print(f"대조: 경합계수판 표본내 7.6% / 표본밖 10.7%")
