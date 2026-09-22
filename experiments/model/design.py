#!/usr/bin/env python3
"""n코어–m가속기 설계 공간 탐색 (E205).

E203/E204 모델에 **발행 항**을 더한다. 지금까지는 루프 FSM을 전제해 뺐지만,
명령 입도가 굵지 않은 ISA에서는 이 항이 지배하고, n코어 일반화의 핵심이다.

    T = max( T_compute, T_issue, T_onchip, T_dram ) · (1 + c·(m−1))
    T_issue = max over cores:  (해당 코어에 붙은 가속기들의 명령 수) · c_issue

작업명령 수 = 타일연산 × cpto,  cpto = 4(순진) / 3(재사용) / 6/(I·J·K)(루프 FSM)
c_issue ≈ 13 cycle/명령 (E172에서 포화 시 11~13.5 실측)

발행 항을 제약하는 데이터는 네 점뿐이다(d9의 2코어, 2i8f50의 1코어). **약하게 제약된
항이므로 사전 등록 예측 후 빌드로 검증해야 한다** — 이 파일의 목적이 그 예측을 만드는 것.
"""
FREQ = 50e6
L2 = 512*1024
B_DRAM = 2.91
# E206: 명령 수 × 단가 가정이 틀렸다. 순진 스테퍼는 재사용보다 명령이 2배인데 시간은
# 1.15배뿐이다 — 6개 중 2개가 config_ld(메모리 작업 없음)이기 때문이다.
# **작업을 수반하는 명령**(mvin/preload/compute)만 세면 16~18 cycle로 수렴한다.
C_ISSUE = 17.0          # cycle/작업명령 (E206)
PNORM = 3.0             # E208 (경합 계수를 대체)

# 면적 (LUT %p, 실측)
A_CORE   = 2.62         # Rocket big core — E216에서 실측(4.5는 과대였다)
A_ACCEL  = 14.2         # INT8 16x16
A_BUS256 = 1.1          # SystemBus 128->256
A_BASE   = 10.12 - 2*A_CORE   # 코어 뺀 나머지 (rocket64b2 = 10.12%)
CLB_PER_LUT = 1.45
CLB_LIMIT = 95.0        # 배치 가능 상한 (E-초반: 97% 초과 불가)

def bsbus(wide): return 24.0 if wide else 15.0

def sq_bytes(N, I, J):
    T=N//16; tot=0; i0=0
    while i0<T:
        Ii=min(I,T-i0); j0=0
        while j0<T:
            Jj=min(J,T-j0); tot += (Ii*T+T*Jj+Ii*Jj)*256; j0+=J
        i0+=I
    return tot

def predict(N, I, J, n_core, m_acc, wide, cpto, t_single_1acc):
    """m개 가속기가 각자 matmul 하나씩. 가속기는 코어에 고르게 배분."""
    per_core = -(-m_acc // n_core)                    # 한 코어가 맡는 최대 가속기 수
    T = N//16
    tile_ops = T**3
    cmds_per_core = per_core * tile_ops * cpto
    t_comp   = t_single_1acc
    t_issue  = cmds_per_core * C_ISSUE / FREQ
    t_onchip = m_acc * sq_bytes(N,I,J) / bsbus(wide) / FREQ
    ws = m_acc * 3 * N * N
    t_dram   = (0.0 if ws<=L2 else ws*(1-L2/ws)) / B_DRAM / FREQ
    t = sum(x**PNORM for x in (t_comp,t_issue,t_onchip,t_dram)) ** (1.0/PNORM)
    return t, dict(연산=t_comp, 발행=t_issue, 온칩=t_onchip, DRAM=t_dram)

def area(n_core, m_acc, wide):
    lut = A_BASE + n_core*A_CORE + m_acc*A_ACCEL + (A_BUS256 if wide else 0)
    return lut, lut*CLB_PER_LUT

# --- 검증: 발행 항이 기존 n=1/n=2 비교를 재현하는가 ---
print("=== 발행 항 검증 (재사용 스테퍼, cpto=3) ===")
print(f"{'구성':22s} {'실측ms':>8s} {'예측ms':>8s} {'오차':>7s}  지배항")
CHECK = [
  # (설명, N, n, m, wide, t_single(ms), 실측 총시간(ms))
  ("d9 2코어2가속 128³",   128, 2, 2, False, 0.958*50/40, 0.975*50/40),  # 40MHz -> 50MHz 환산
  ("d9 2코어2가속 256³",   256, 2, 2, False, 7.403*50/40, 7.542*50/40),
  ("2i8 1코어2가속 128³",  128, 1, 2, False, 0.766,        1.130),
  ("w256 1코어2가속 128³", 128, 1, 2, True,  0.768,        1.100),
  ("w256 1코어3가속 128³", 128, 1, 3, True,  0.768,        1.546),
]
for name,N,n,m,wide,t1,meas in CHECK:
    p,parts = predict(N,4,4,n,m,wide,3,t1/1e3)
    dom = max(parts.items(), key=lambda kv: kv[1])[0]
    print(f"{name:22s} {meas:8.3f} {p*1e3:8.3f} {(p*1e3-meas)/meas*100:+6.1f}%  {dom}")

# --- 설계 공간 ---
print()
print("=== 면적 예산별 최적 구성 (192³ 워크로드, 루프 FSM cpto=6/(I·J·K)) ===")
print(f"{'n':>2s} {'m':>2s} {'버스':>5s} | {'LUT%':>6s} {'CLB%':>6s} | {'예측ms':>8s} {'처리량':>7s} | 지배항")
print("-"*70)
N=192; I=J=4; T=N//16
cpto_fsm = 6.0/(I*J*T)
t1 = 0.622e-3          # 192³ 가속기 하나 실측 (E194, 256비트)
rows=[]
for n in (1,2):
    for m in (1,2,3,4):
        for wide in (False,True):
            lut,clb = area(n,m,wide)
            if clb > CLB_LIMIT: continue
            t,parts = predict(N,I,J,n,m,wide,cpto_fsm,t1)
            thr = m*t1/t
            dom = max(parts.items(), key=lambda kv: kv[1])[0]
            rows.append((thr/lut, n,m,wide,lut,clb,t,thr,dom))
for eff,n,m,wide,lut,clb,t,thr,dom in sorted(rows, key=lambda r:-r[7]):
    print(f"{n:2d} {m:2d} {'256b' if wide else '128b':>5s} | {lut:6.1f} {clb:6.1f} | "
          f"{t*1e3:8.3f} {thr:6.2f}x | {dom}")
