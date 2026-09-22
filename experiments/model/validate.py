#!/usr/bin/env python3
"""최종 형태 모델을 **모든 데이터셋**에 검증 (E219).

E218에서 가속기 결합 지수를 철회했으므로 형태가 확정됐다:

    T = ( max_a(t_a)³ + T_issue³ + T_onchip³ + T_dram³ )^(1/3)

자유 파라미터는 p=3 하나. 나머지는 전부 독립 실측:
    c_issue   17 cycle/작업명령        (E206)
    B_sbus    15.0 / 24.0 B/cycle      (E180, E194)
    B_dram(ws) 곡선                    (E207)
"""
import statistics, importlib.util, io, contextlib, math
spec = importlib.util.spec_from_file_location("cost","experiments/model/cost.py")
cost = importlib.util.module_from_spec(spec)
with contextlib.redirect_stdout(io.StringIO()): spec.loader.exec_module(cost)
FREQ, L2, P = cost.FREQ, cost.L2, 3.0
# E220: c_issue는 상수가 아니다 — 큐가 많을수록 명령당 비용이 상각된다.
# 실측(12점): m=2에서 17.16, m=3에서 15.12. m=1은 발행이 지배하지 않아 측정 불가.
_CI = {1:17.16, 2:17.16, 3:15.12}
def C_issue(m): return _CI.get(m, 15.12)

def comb(parts): return sum(x**P for x in parts)**(1.0/P)

def predict(t_par, bytes_tot, ws, B, cmds=0.0, m=1):
    """t_par = max_a(t_a) [s], bytes_tot = 온칩 총 바이트, ws = 작업셋 바이트.

    E219: 발행과 연산은 **max로** 묶는다. 디커플드 가속기에서 코어가 발행하는 동안
    가속기가 계산하는 것은 설계 의도이므로 두 항은 거의 완전히 겹친다.
    반면 대역폭 항은 연산과 진짜로 경합하므로 p-노름으로 묶는다."""
    ti = cmds*C_issue(m)/FREQ
    to = bytes_tot/B/FREQ
    td = (0.0 if ws<=L2 else ws*(1-L2/ws))/cost.B_dram_of(ws)/FREQ
    return comb([max(t_par, ti), to, td])

R = {}
def run(tag, rows):
    errs=[]
    for meas, pred in rows:
        errs.append(abs((pred-meas)/meas*100))
    R[tag]=errs
    print(f"{tag:26s} n={len(errs):3d}  평균 {statistics.mean(errs):5.1f}%  "
          f"중앙 {statistics.median(errs):5.1f}%  최대 {max(errs):5.1f}%  "
          f"10%내 {sum(1 for e in errs if e<=10):3d}  20%내 {sum(1 for e in errs if e<=20):3d}")

# 1) 정방 (루프 FSM, 발행 무시)
rows=[]
for label,N,I,J,B,ts in cost.DATA:
    # E212: N>=192에서는 연산 바닥이 실측 T_single보다 정확하다(자기 비효율이
    # 공유 상황에서 사라지므로). 일관되게 적용한다.
    t1 = (N**3/256.0/FREQ) if N>=192 else ts[0]/1e3
    for m in (2,3):
        rows.append((ts[m-1]/1e3, predict(t1, m*cost.sq_bytes(N,I,J), m*3*N*N, B, m=m)))
run("정방 (E180~E194)", rows)

# 2) BERT (루프 FSM)
rows=[]
for label,M,N,K,I,J,B,ts in cost.BERT:
    t1=ts[0]/1e3
    for m in (2,3):
        if ts[m-1] is None: continue
        rows.append((ts[m-1]/1e3, predict(t1, m*cost.gen_bytes(M,N,K,I,J),
                                          M*K+m*(K*N+M*N), B, m=m)))
run("BERT 비정방 (E195~E204)", rows)

# 3) blf50 비대칭 (순진 스테퍼: 작업명령 4/타일연산, 128비트)
#    바이트: 타일연산당 2타일 x 256B, 두 가속기 합
rows=[]
for NI,ta,tb,tm in [(64,0.182,0.156,0.222),(128,1.294,1.086,1.388),
                    (192,4.348,4.030,4.554),(256,10.972,9.062,11.924)]:
    T=NI//16; tops=T**3
    by = 2*tops*2*256                      # 두 가속기, 타일연산당 2타일
    ws = 3*NI*NI + 3*(NI//2)*(NI//2)*4     # INT8 3행렬 + FP32 3행렬(4B)
    rows.append((tm/1e3, predict(max(ta,tb)/1e3, by, ws, 15.0, cmds=2*tops*4, m=2)))
run("blf50 비대칭 (E217)", rows)

# 4) 동일 가속기 불균등 (루프 FSM, 256비트)
rows=[]
for na,nb,ta,tb,tm in [(256,128,1.392,0.218,1.388),(256,192,1.392,0.622,1.394),
                       (192,128,0.624,0.220,0.620),(320,256,2.664,1.398,2.680),
                       (256,64,1.390,0.066,1.392),(192,192,0.624,0.622,0.628)]:
    by = cost.sq_bytes(na,4,4)+cost.sq_bytes(nb,4,4)
    ws = 3*na*na + 3*nb*nb
    rows.append((tm/1e3, predict(max(ta,tb)/1e3, by, ws, 24.0, m=2)))
run("동일+불균등 (E218)", rows)

# 5) 1코어 사전 등록 (E216, 바닥 입력)
rows=[]
for N,m,meas in [(192,2,0.628),(192,3,0.656),(320,2,2.708),(320,3,3.260),
                 (384,2,4.592),(384,3,5.924),(448,2,7.290),(448,3,10.150),
                 (512,2,11.334),(512,3,15.386)]:
    fl = N**3/256.0/FREQ
    rows.append((meas/1e3, predict(fl, m*cost.sq_bytes(N,4,4), m*3*N*N, 24.0, m=m)))
run("1코어 사전등록 (E216)", rows)

# 6) 발행 지배 (E220): 순진/재사용 스테퍼 x 128/192/256 x m=2,3
rows=[]
ISS = [(128,4.0,2.0,[1.294,1.356,1.734]), (128,3.0,1.125,[0.768,1.106,1.542]),
       (192,4.0,2.0,[4.350,4.576,5.680]), (192,3.0,1.083,[2.972,3.482,4.838]),
       (256,4.0,2.0,[10.890,11.596,14.492]),(256,3.0,1.0625,[6.316,8.528,11.720])]
for N,wc,ld,ts in ISS:
    tops=(N//16)**3; t1=ts[0]/1e3
    for m in (2,3):
        rows.append((ts[m-1]/1e3, predict(t1, m*tops*ld*256, m*3*N*N, 24.0,
                                          cmds=m*tops*wc, m=m)))
run("발행 지배 (E220)", rows)

allerrs=[e for v in R.values() for e in v]
print("-"*96)
print(f"{'전체':26s} n={len(allerrs):3d}  평균 {statistics.mean(allerrs):5.1f}%  "
      f"중앙 {statistics.median(allerrs):5.1f}%  "
      f"10%내 {sum(1 for e in allerrs if e<=10):3d}  20%내 {sum(1 for e in allerrs if e<=20):3d}")
