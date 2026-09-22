#!/usr/bin/env python3
"""모델이 블록 모양을 **측정 없이** 고를 수 있는가 (E209).

이 트랙의 목표 주장은 "단일 가속기 프로파일에는 답을 고를 정보가 없고, 코스트 모델이
그것을 공급한다"였다. 여기서 직접 시험한다:

  입력: 크기 하나에 대한 **아무 모양 하나**의 단일 가속기 시간 (T_single)
  출력: m=3에서의 모양 순위 예측
  대조: 실측 순위 (E183/E193/E200)

T_single을 모양마다 재면 반칙이다 — 그러면 이미 다 측정한 것이다.
**하나만 재고 나머지를 예측**해야 의미가 있다.
"""
import statistics, importlib.util, io, contextlib
spec = importlib.util.spec_from_file_location("cost", "experiments/model/cost.py")
cost = importlib.util.module_from_spec(spec)
with contextlib.redirect_stdout(io.StringIO()):
    spec.loader.exec_module(cost)

def rank_sq(N, shapes, t_single_ms, B, m=3):
    """정방. T_single은 하나만 주어진다(모양 무관하게 같은 값 사용)."""
    out=[]
    for (I,J) in shapes:
        tc = t_single_ms/1e3
        to = m*cost.sq_bytes(N,I,J)/B/cost.FREQ
        ws = m*3*N*N
        td = cost.sq_dram(N,I,J,m)/cost.B_dram_of(ws)/cost.FREQ
        out.append(((tc**3+to**3+td**3)**(1/3.0)*1e3, (I,J)))
    return sorted(out)

CASES = [
 # (설명, N, B_sbus, T_single(ms, 아무 모양 하나), {모양: 실측 m=3 ms})
 ("128³ (E183)", 128, 15.0, 0.222, {
   (4,8):0.304,(8,4):0.316,(4,4):0.358,(2,8):0.414,(8,2):0.420,(4,2):0.472,(2,4):0.474,(2,2):0.594}),
 ("192³ (E193)", 192, 15.0, 0.622, {
   (6,4):0.976,(4,4):1.126,(3,4):1.270,(4,3):1.372,(6,2):1.432,(3,3):1.558}),
 ("320³ (E193)", 320, 15.0, 2.668, {
   (5,4):4.596,(5,5):4.804,(4,4):5.020,(4,5):5.150,(2,10):6.032,(10,2):6.240}),
 ("512³ (E191)", 512, 15.0, 11.056, {
   (8,4):16.772,(4,8):17.464,(4,4):22.108,(8,2):29.410}),
]

print("=== 모양 하나만 측정하고 나머지를 예측 (m=3) ===")
tot_top1 = tot_top3 = tot_n = 0
sp_all = []
for name, N, B, t1, meas in CASES:
    shapes = list(meas.keys())
    pred = rank_sq(N, shapes, t1, B)
    pred_order = [s for _,s in pred]
    meas_order = [s for s,_ in sorted(meas.items(), key=lambda kv: kv[1])]
    # 순위 상관 (스피어만)
    rp = {s:i for i,s in enumerate(pred_order)}
    rm = {s:i for i,s in enumerate(meas_order)}
    n=len(shapes)
    d2 = sum((rp[s]-rm[s])**2 for s in shapes)
    rho = 1 - 6*d2/(n*(n*n-1))
    sp_all.append(rho)
    top1 = pred_order[0]==meas_order[0]
    top3 = pred_order[0] in meas_order[:3]
    tot_top1 += top1; tot_top3 += top3; tot_n += 1
    print(f"\n{name}  (T_single {t1:.3f} ms 하나만 입력)")
    print(f"  예측 순위: {' > '.join(str(s) for s in pred_order)}")
    print(f"  실측 순위: {' > '.join(str(s) for s in meas_order)}")
    print(f"  1등 적중 {'O' if top1 else 'X'} (실측1등 {meas_order[0]}, 예측1등 {pred_order[0]}"
          f" -> 실측 {meas[pred_order[0]]:.3f} vs 최선 {meas[meas_order[0]]:.3f} = "
          f"{meas[pred_order[0]]/meas[meas_order[0]]:.3f}배),  스피어만 rho={rho:+.2f}")
print()
print(f"=== 1등 적중 {tot_top1}/{tot_n},  1등이 실측 3위 안 {tot_top3}/{tot_n},  "
      f"평균 스피어만 {statistics.mean(sp_all):+.2f} ===")

# ---------------------------------------------------------------
# E209의 유보 해소: 비정방(BERT) 및 256비트에서의 순위 예측
# ---------------------------------------------------------------
def rank_gen(M,N,K, shapes, t_single_ms, B, m=3):
    out=[]
    for (I,J) in shapes:
        tc = t_single_ms/1e3
        to = m*cost.gen_bytes(M,N,K,I,J)/B/cost.FREQ
        ws = M*K + m*(K*N + M*N)
        td = cost.gen_dram(M,N,K,m)/cost.B_dram_of(ws)/cost.FREQ
        out.append(((tc**3+to**3+td**3)**(1/3.0)*1e3, (I,J)))
    return sorted(out)

print()
print("=== 유보 해소: 비정방·256비트 ===")
CASES2 = [
  # 정방 512³ 256비트 (E194)
  ("512³ 256b (E194)", None, 512,512,512, 24.0, 11.332,
   {(8,4):12.254,(4,8):14.032,(4,4):15.482}),
  # 비정방 FFN2 256비트 (E200)
  ("FFN2 256b (E200)", (128,768,3072), None,None,None, 24.0, 24.958,
   {(4,8):59.914,(8,4):60.358,(2,16):66.216}),
]
for name, mnk, N,_,__, B, t1, meas in CASES2:
    shapes=list(meas.keys())
    if mnk: pred = rank_gen(*mnk, shapes, t1, B)
    else:   pred = rank_sq(N, shapes, t1, B)
    po=[s for _,s in pred]; mo=[s for s,_ in sorted(meas.items(), key=lambda kv: kv[1])]
    n=len(shapes); rp={s:i for i,s in enumerate(po)}; rm={s:i for i,s in enumerate(mo)}
    rho = 1 - 6*sum((rp[s]-rm[s])**2 for s in shapes)/(n*(n*n-1))
    print(f"\n{name}  (T_single {t1:.3f} ms)")
    print(f"  예측 {' > '.join(map(str,po))}")
    print(f"  실측 {' > '.join(map(str,mo))}")
    print(f"  1등 {'O' if po[0]==mo[0] else 'X'},  선택 대가 {meas[po[0]]/meas[mo[0]]:.3f}배,  rho={rho:+.2f}")
