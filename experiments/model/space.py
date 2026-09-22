#!/usr/bin/env python3
"""n코어–m가속기–버스폭 설계 공간, 워크로드 측정 없이 (E213).

E212로 N>=192에서 T_single을 연산 바닥으로 대체할 수 있음이 확인됐으므로,
**빌드하지 않은 구성의 성능을 워크로드 측정 없이** 계산할 수 있다.

명령 입도(ISA)를 같이 훑어 "최적 (n,m)이 ISA에 의존하는가"를 본다 —
하드웨어/컴파일러 공동 설계 주장의 핵심이다.
"""
import importlib.util, io, contextlib
spec = importlib.util.spec_from_file_location("cost","experiments/model/cost.py")
cost = importlib.util.module_from_spec(spec)
with contextlib.redirect_stdout(io.StringIO()): spec.loader.exec_module(cost)
FREQ, L2 = cost.FREQ, cost.L2
PNORM, C_ISSUE = 3.0, 17.0
A_CORE, A_ACCEL, A_BUS = 2.62, 14.2, 1.1   # A_CORE는 E216 실측
A_BASE = 10.12 - 2*A_CORE
CLB_PER_LUT, CLB_LIMIT = 1.45, 95.0

def legal_shapes(TI,TJ):
    out=[]
    for I in range(1,33):
        for J in range(1,33):
            if I*J>32: continue
            if TI%I or TJ%J: continue
            out.append((I,J))
    return out

def eval_cfg(M,N,K, n,m,wide, cpto):
    """모델이 블록 모양까지 고른다(E209). T_single은 연산 바닥(E212)."""
    B = 24.0 if wide else 15.0
    TI,TJ,TK = M//16, N//16, K//16
    t1 = (M*N*K/256.0)/FREQ
    best=None
    for (I,J) in legal_shapes(TI,TJ):
        # K 분할(E199): 청크 Kc로 끊으면 스크래치패드 제약이 Kc 기준이 된다.
        # 이동 바이트 총량은 K 분할과 무관하므로 by는 그대로다.
        Kc = min(TK, 16)
        if I*Kc + Kc*J > 512: continue
        by = cost.gen_bytes(M,N,K,I,J)
        tops = TI*TJ*TK
        per_core = -(-m//n)
        ti = per_core*tops*cpto*C_ISSUE/FREQ
        to = m*by/B/FREQ
        ws = M*K + m*(K*N + M*N)
        td = (0.0 if ws<=L2 else ws*(1-L2/ws))/cost.B_dram_of(ws)/FREQ
        t = (t1**PNORM + ti**PNORM + to**PNORM + td**PNORM)**(1/PNORM)
        if best is None or t<best[0]: best=(t,(I,J))
    return best

def area(n,m,wide):
    lut = A_BASE + n*A_CORE + m*A_ACCEL + (A_BUS if wide else 0)
    return lut, lut*CLB_PER_LUT

WORK = [("정방 512³", 512,512,512), ("BERT proj", 128,768,768), ("BERT FFN2", 128,768,3072)]
ISAS = [("루프 FSM", None), ("손수 발행", 3.0)]

for wname, M,N,K in WORK:
    print(f"\n########## {wname}  [{M}x{K}]x[{K}x{N}] ##########")
    for iname, cp in ISAS:
        print(f"\n--- ISA: {iname} ---")
        print(f"{'n':>2s} {'m':>2s} {'버스':>5s} | {'LUT%':>6s} {'CLB%':>6s} | {'ms':>8s} "
              f"{'처리량':>7s} {'처리량/LUT':>10s} | 블록")
        rows=[]
        for n in (1,2,4):
            for m in (1,2,3,4):
                for wide in (False,True):
                    lut,clb = area(n,m,wide)
                    if clb > CLB_LIMIT: continue
                    tops=(M//16)*(N//16)*(K//16)
                    cpto = cp if cp else 6.0/tops*( (M//16)*(N//16)*(K//16) / max(1,tops) )
                    if cp is None:
                        # 루프 FSM: 블록당 명령 6개. 블록 수는 모양에 따라 다르므로 근사로
                        # 타일연산당 6/(I*J*K) ~ 매우 작음. 0으로 둔다.
                        cpto = 0.0
                    r = eval_cfg(M,N,K,n,m,wide,cpto)
                    if r is None: continue
                    t,(I,J) = r
                    t1 = (M*N*K/256.0)/FREQ
                    rows.append((t, n,m,wide,lut,clb,(I,J)))
        # 처리량은 **같은 n·버스의 m=1** 대비로 잰다 (연산 바닥 대비가 아니라).
        base = {(n,w): t for t,n,m,w,_,_,_ in rows if m==1}
        out=[]
        for t,n,m,wide,lut,clb,IJ in rows:
            thr = m*base[(n,wide)]/t
            out.append((thr,n,m,wide,lut,clb,t,IJ))
        best_eff = max(out, key=lambda r: r[0]/r[4])
        for thr,n,m,wide,lut,clb,t,IJ in sorted(out, key=lambda r:-r[0])[:6]:
            star = " *" if (thr,n,m,wide,lut,clb,t,IJ)==best_eff else ""
            print(f"{n:2d} {m:2d} {'256b' if wide else '128b':>5s} | {lut:6.1f} {clb:6.1f} | "
                  f"{t*1e3:8.3f} {thr:6.2f}x {thr/lut:10.4f} | {IJ}{star}")
        print(f"  (* = 처리량/LUT 최대)")
