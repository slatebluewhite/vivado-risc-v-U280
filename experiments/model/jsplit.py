#!/usr/bin/env python3
"""J 분할 코스트 모델 — "가속기 몇 개에게 나눌 것인가"를 프로파일 없이 정한다 (E240).

E239가 "워킹셋 곡선으로 프로파일 없이 정해진다"고 적었지만 실제로는 측정으로 골랐다.
여기서 진짜로 모델이 고를 수 있는지 본다.

기존 cost.py는 **독립 복제**(작업이 m배)를 다룬다. J 분할은 작업이 고정이라 구조가 다르다:
    t_comp   = 루프라인 / m        (연산만 나뉜다)
    t_onchip = 총 바이트 / B_sbus  (m과 무관 — 같은 블록을 한 번씩 처리)
    t_dram   = DRAM 바이트 / B_dram(ws)   (m과 무관 — 같은 데이터)
    T(m)     = 세제곱 결합
공유 항이 m과 무관하다는 것은 E235의 실측이고, 그래서 T(m)에는 **바닥**이 생긴다.

다만 이 형태만으로는 T가 m에 대해 단조 감소라 "셋보다 둘이 낫다"(E238의 FFN1)를
설명할 수 없다. 빠진 것은 **부하 불균형**이다 — J 방향 블록 개수 nj를 m으로 나눌 때
나머지가 생기면 가장 많이 맡은 가속기가 전체를 결정한다:
    t_comp(m) = 루프라인 · ceil(nj/m)/nj
파라미터를 늘리지 않는다(세는 것뿐).
"""
import math
FREQ=50e6; L2=512*1024; DIM=16
_BW=[(576,15.11),(1152,10.31),(1728,6.83),(2304,4.38),(4608,2.85),(9216,2.5)]
def B_dram_of(ws):
    kb=ws/1024.0
    if kb<=_BW[0][0]: return _BW[0][1]
    for (k0,v0),(k1,v1) in zip(_BW,_BW[1:]):
        if kb<=k1: return v0+(v1-v0)*(kb-k0)/(k1-k0)
    return _BW[-1][1]

def blocks(M,K,N,BI,BJ):
    TI,TK,TN = M//DIM, K//DIM, N//DIM
    nb_i = math.ceil(TI/BI); nj = math.ceil(TN/BJ)
    tot=0
    for bi in range(nb_i):
        I=min(BI,TI-bi*BI)
        for bj in range(nj):
            J=min(BJ,TN-bj*BJ)
            tot += (I*TK + TK*J + I*J)*256
    return tot, nj

def predict(M,K,N,m,BI=4,BJ=6,B_sbus=15.0):
    byt,nj = blocks(M,K,N,BI,BJ)
    roof = M*K*N/(DIM*DIM)/FREQ                       # 초
    share = math.ceil(nj/m)/nj                        # 부하 불균형
    t_comp = roof*share
    t_on   = byt/B_sbus/FREQ
    ws     = M*K + K*N + M*N
    dram   = ws*max(0.0,1.0-L2/ws)
    t_dr   = dram/B_dram_of(ws)/FREQ
    T = (t_comp**3 + t_on**3 + t_dr**3)**(1/3)
    return T*1e3,(t_comp*1e3,t_on*1e3,t_dr*1e3)

if __name__=="__main__":
    print("=== 보정: E238/E239에서 이미 잰 BERT 형상 (J 분할) ===")
    cal=[("QKV [768x768]",128,768,768,[6.098,3.200,2.616]),
         ("FFN1 [768x3072]",128,768,3072,[25.630,16.418,17.632]),
         ("FFN2 [3072x768]",128,3072,768,[24.716,21.054,20.978])]
    for nm,M,K,N,ms in cal:
        row=[]
        for m in (1,2,3):
            p,_=predict(M,K,N,m); row.append(f"{p:6.2f}/{ms[m-1]:6.2f}({(p-ms[m-1])/ms[m-1]*100:+5.1f}%)")
        best_p=min(range(1,4),key=lambda m:predict(M,K,N,m)[0])
        best_m=min(range(1,4),key=lambda m:ms[m-1])
        print(f"{nm:18s} " + "  ".join(row) + f"   최적 예측 m={best_p} 실측 m={best_m}")
    print()
    print("=== 사전 등록: 아직 재지 않은 형상 (M=128, 블록(4,6), Kc=16, 128비트) ===")
    print(f"{'형상':20s} {'ws MB':>7s} | {'m=1':>8s} {'m=2':>8s} {'m=3':>8s} | {'최적m':>5s} {'m3/m2':>6s}")
    new=[(128,256,768),(128,1536,768),(128,768,1536),
         (128,1536,1536),(128,512,2048),(128,2048,512)]
    for M,K,N in new:
        ps=[predict(M,K,N,m)[0] for m in (1,2,3)]
        ws=(M*K+K*N+M*N)/1e6
        bm=1+min(range(3),key=lambda i:ps[i])
        print(f"[{K}x{N}]{'':>{max(0,12-len(str(K))-len(str(N)))}} {ws:6.2f}MB | "
              f"{ps[0]:8.3f} {ps[1]:8.3f} {ps[2]:8.3f} | {bm:5d} {ps[2]/ps[1]:6.3f}")
