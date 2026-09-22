#!/usr/bin/env python3
"""U280 Gemmini 컴파일 규칙 — 하나의 실행 가능한 선택기 (E278~E318 종합).

각 조각은 따로 검정됐지만 합쳐서 돌려 본 적이 없다. 여기서 합치고, 이 트랙이
측정한 모든 (형상 -> 실측 최적)에 대고 채점한다.

규칙 (근거 실험):
  블록   E317/E318/E329 : 청크 4 이하로 갈 수 있는 블록 중 강도 16(1/I+1/J) 최소.
                     그런 블록이 없으면 **청크 수 최소**를 먼저 보고 강도로 동률을 깬다.
                     동률이면 큰 I. 청크 때문에 탈락한 블록 중 강도가 25% 이상
                     낮은 것이 있으면 그것도 재고 빠른 쪽.
  청크   E302/E303 : K·N >= 1.4 MB 이면 4, 아니면 2.
  Kc     E300/E331 : Ktil/청크수 (단 Kc >= 4타일). 스크래치패드 Kc*(I+J) <= 512를 넘으면
                     청크 수가 목표에 가장 가까운 합법 Kc.
  배분   E315      : 불균형(하나씩) = m*ceil(k/m)/k, 불균형(J분할) = m*ceil(nj/m)/nj.
                     작은 쪽. 둘 다 1.0이면 정확히 동률.
  가속기 E265/E291: K <= 1024면 세 번째가 값을 한다. K >= 1536이면 안 한다.

알려진 한계 (E319):
  - [3072x768]류에서 (4,6)을 고르지만 실측은 (4,4)가 빠르다: 단계 6.7%,
    BERT-base 층에서 **실측 3.9%** (E325). 한 단계가 층의 1/3을 넘으면 차점 블록도 재라.
  - Kc가 Ktil을 나눈다고 가정한다. E274는 불균등 Kc(48을 32+16)가 낫다고 쟀지만
    그건 50 MHz·3가속기 판이었고, 62.5 MHz·2가속기 판에서는 균등 쪽이 0.62% 빠르다
    (E327). 이 제약은 현재 권장 구성에서 한계가 아니다.
  - "상위 두 후보가 강도 25% 안이면 둘 다 재라"는 시도했다가 기각했다:
    측정 4.6배에 최악 오차 개선 없음.

주의(E369): 아래 조항들은 `ACC_LIMIT=32`, `SP_LIMIT=512`, m=3(청크는 m=2에서 K<=1280까지)
에 맞춰져 있다. 한계를 올린 판에서는 1순위와 차점을 둘 다 재고, m=2이고 K가 2048 근처면
청크 2와 4를 둘 다 재라 — 각각 측정 한 번이고, 안 하면 최대 10%를 잃는다.

제약: I*J <= 32 (누산기 절반 규칙), I >= 4, **J는 4의 배수**(런 64바이트 정렬, E326), Ntil % J == 0.
"""
import math

# I는 TI = M/16 을 나눠야 한다 (안 그러면 I 방향 꼬리가 생겨 2~8% 손해, E246).
# J는 Ntil 을 나눠야 한다 (J 방향 꼬리).  이 두 조항은 E278~E318에서 **암묵적**이었다 —
# 재본 블록들이 전부 M=128 (TI=8) 에서 I in {4,8} 이었기 때문이다. 명시하지 않으면
# 선택기가 (5,6)·(6,4) 같은 미측정 블록을 고른다 (E319).
# J*16 바이트가 64바이트 접근 단위의 배수여야 한다 -> J % 4 == 0 (E326).
# J=6이면 런이 96바이트라 매 런이 접근 단위 1.5개에 걸친다. 실측으로 (4,6)은
# 측정된 세 형상에서 전부 꼴찌 근처였고(+14.0/+79.7/+5.5%) 한 번도 이기지 않았다.
# 이 조항은 E296("런이 64바이트 미만이면 손해")의 자연스러운 연장이다.
# 두 한계는 **하드웨어 상수이지 법칙이 아니다** (E361). 각각 누산기·스크래치패드
# 용량의 절반이고(`loop_ws` 이중버퍼), 용량을 두 배로 한 판에서는 (8,8)이 최적이 된다.
# 다른 Gemmini로 옮길 때는 생성된 gemmini_params.h에서 다시 계산할 것:
#     ACC_TILES = ACC_ROWS/16/2          (기본 64/2 = 32)
#     SP_TILES  = BANK_ROWS*BANK_NUM/16/2 (기본 2048/2 = 512... 실측 512)
ACC_LIMIT = 32    # I*J <= ACC_LIMIT
SP_LIMIT  = 512   # Kc*(I+J) <= SP_LIMIT

def all_blocks():
    return [(I, J) for I in (2,4,5,6,8) for J in (2,4,6,8)
            if I*J <= ACC_LIMIT and I >= 4 and J >= 4 and (J*16) % 64 == 0]

def blocks_for(M, N):
    TI, Ntil = M//16, N//16
    return [b for b in all_blocks() if TI % b[0] == 0 and Ntil % b[1] == 0]

def intensity(b):            return 16*(1.0/b[0] + 1.0/b[1])
def legal_kcs(Ktil, I, J):   return [kc for kc in range(1, Ktil+1)
                                     if Ktil % kc == 0 and kc*(I+J) <= SP_LIMIT]
def min_chunks(K, I, J):
    Ktil = K//16; kcs = legal_kcs(Ktil, I, J)
    return min((Ktil//kc for kc in kcs), default=99)

# E302: 표준 한계·(8,4)에서 임계는 1.4 MB. E406: **mem2 + (8,8)에서는 2.6 MB다.**
# 12개 형상(E403·E404·E405·E406, 블록 (8,8), m=3, 5회 최소)을 단일 측정으로 채점하면
#   임계 1.4~2.24 MB : 평균 2.38% 최대 14.68%   <- 옛 값
#   임계 2.3~3.0 MB  : 평균 0.13% 최대  1.53%   <- 새 값 (구간 전체가 동점)
#   임계 4.0 MB      : 평균 2.41% 최대 12.54%
# E408이 구간을 좁혔고(2.25~2.50 MiB) E423이 **10회 재실행**으로 절반으로 더 좁혔다:
# [1024x2432](=2.375 MiB)에서 4청크가 **+5.3%**로 이기므로 교차는
# **(2359296, 2490368] 바이트 = (2.25, 2.375] MiB**다.
# E408에서 교차를 정한 칸([1024x2560], +1.6 %p)은 E422의 "조심" 밴드였는데, 이제
# "믿음" 밴드 칸(+5.3%)이 정한다. 2.42e6은 그 구간의 기하 중간이지 맞춘 상수가 아니다
# — 구간 안 어느 값이든 15개 형상 채점이 같다. (옛 2.49e6은 구간 상단 99.7% 지점이었다.)
# 표준 한계 판에는 옛 1.4e6을 그대로 둔다 — 새 값은 (8,8)+mem2에서만 측정됐다.
KN_THRESHOLD = 1.4e6

def chunk_target(K, N):      return 4 if K*N >= KN_THRESHOLD else 2

MIN_KC = 4   # E331: 청크 하나가 4타일보다 작으면 손해 (K=64에서 Kc=2가 8.5% 느리다)
             # 필터 자체는 살아 있지만(6084회), 아래 `big or kcs`의 **되돌림 쪽은**
             # Ktil이 4의 배수면 도달 불가다 — Kc=4가 늘 합법이라 big이 비지 않는다 (E424).

def pick_kc(K, I, J, target):
    """E300: 목표에 가장 가까운 합법 청크 수. 동률이면 큰 Kc.
    E331: 단, Kc는 4타일 이상 (가능할 때). K가 아주 작으면 분할하지 않는 쪽이 낫다."""
    Ktil = K//16; kcs = legal_kcs(Ktil, I, J)
    if not kcs: return None, None
    big = [kc for kc in kcs if kc >= MIN_KC]
    pool = big or kcs
    # E403/E404/E405: **E398/E400의 "목표 4면 합법 최대 Kc" 조항은 철회했다.**
    # 그 조항은 [3072x768] 한 형상에서 유도됐는데, 아홉 형상으로 넓히면 평균 5.24%·최대
    # 16.24% 손해다(항상 4청크는 3.00%/14.68%). 세로로 긴 형상만 상한 Kc를 원하고
    # 가로로 긴 형상과 정사각은 원하지 않는다. 대신 임계값을 옮겼다 — KN_THRESHOLD 참조.
    #
    # 동률 처리: 목표 2면 작은 Kc, 목표 4면 큰 Kc.
    # **주의 — 이 가지는 Ktil이 4의 배수이면 도달 불가다** (E424: 형상 x M x m 1521개
    # 조합에서 발동 0회, Ktil 1~400 중 4의 배수 100개에서도 0회). 이 프로젝트의 K는 전부
    # 64의 배수라 Ktil이 항상 4의 배수이므로 **실제로는 절대 안 걸린다.** 작은 K를 위해
    # 남겨 둔다.
    # 근거로 삼지 말 것: E405가 이 조항을 정한 비교([1536x1536] Kc=48 대 64, 2.3%)에서
    # Kc=64는 Ktil=96을 나누지 못해 legal_kcs가 애초에 후보로 주지 않는 값이다 —
    # 규칙이 고를 수 없는 두 값을 비교해 규칙을 정한 셈이었다.
    kc = min(pool, key=lambda kc: (abs(Ktil//kc - target),
                                   kc if target <= 2 else -kc))
    return kc, Ktil//kc

def pick_blocks(K, N, M=128, m=3):
    """E317/E318: 후보 리스트를 돌려준다 (첫 원소가 1순위, 둘 이상이면 둘 다 재라)."""
    cand = [b for b in blocks_for(M, N) if legal_kcs(K//16, *b)]
    if not cand: return []
    ok = [b for b in cand if min_chunks(K, *b) <= 4]
    if ok:
        # E412: 동점(강도가 같은 (I,J)와 (J,I)) 처리는 **N과 K의 대소**로 정해진다.
        #   N >= K -> I가 큰 쪽 (8,4)      N < K -> J가 큰 쪽 (4,8)
        # 33칸에서 **33/33, 평균 초과 0.00%**. 이 트랙의 모든 실측(measured.py 18칸 +
        # E409·E411·E412 15칸, m=2와 m=3 모두, N/K 0.25~4.0)을 모아 채점했다.
        # **이것이 E371의 "m<=2·K>=2560이면 J가 큰 쪽"을 대체한다** — 그 규칙은 같은
        # 33칸에서 평균 4.64%·최대 30.9%로 14칸을 빗나간다(m=2 칸 포함). E371의 원래
        # 39칸은 measured.py에 없어 재채점할 수 없지만, 그 조건(K>=2560)을 만족하는
        # 형상은 대개 N<K라 두 규칙이 같은 답을 냈을 것이다 — (m,K)는 N/K의 대리였다.
        # 경계는 정확히 N=K다: 0.75에서 (4,8), 1.00에서 (8,4)로 갈린다 (E412가
        # 비어 있던 (0.36, 1.00) 구간을 채워 확인).
        tb = (lambda b: (intensity(b), -b[0])) if N >= K else (lambda b: (intensity(b), -b[1]))
        first = min(ok, key=tb)
        pool = ok
    else:
        # E329: 아무 블록도 목표(<=4청크)에 못 갈 때는 강도가 아니라 **청크 수**를 먼저
        # 줄인다. h1536 FFN2에서 강도만 보면 (8,4) 12청크를 고르는데 (4,4) 6청크가
        # 단계 3.2% / 층 1.6% 빠르다. E282의 청크 항이 강도 차이를 압도한다.
        pool = [b for b in cand if min_chunks(K, *b) == min_chunks(K, *min(
            cand, key=lambda x: min_chunks(K, *x)))]
        tb2 = (lambda b: (min_chunks(K, *b), intensity(b), -b[0])) if N >= K \
              else (lambda b: (min_chunks(K, *b), intensity(b), -b[1]))
        first = min(cand, key=tb2)
    # E361: 대체 분기에서 pool = cand로 두면 alts가 **항상 빈 리스트**가 되어
    # E318의 조항이 발동하지 않았다. [8192x2048]에서 그 결함이 10.4%였다 —
    # (4,4) 8청크 강도 8.0을 고르는데 (4,8) 16청크 강도 6.0이 더 빠르다.
    # 큰 K·N에서는 강도가 청크를 이긴다.
    alts = [b for b in cand if b not in pool and intensity(b) < intensity(first)*0.8]
    alts.sort(key=(lambda b: (intensity(b), -b[0])) if N >= K
              else (lambda b: (intensity(b), -b[1])))
    # E426: 후보는 3개까지만 찍는다. 27개 단계 채점에서 4순위는 **10번 찍혀 0번 이겼고**,
    # 상위 3에서 자르면 손해 0.00%로 측정 횟수가 74 -> 64회(−14%)가 된다.
    # (상위 2는 +0.07%에 −28%지만, 3순위가 이긴 자리가 실제로 하나 있어 3을 남긴다.)
    out = [first] + alts
    # E380: 한계를 올린 판에서는 **표준 한계(32/512)에서 규칙이 고를 블록**도 후보에 넣는다.
    # 강도 순위가 (8,8)을 1순위로 올리지만 그 우위는 B 트래픽에서 오고 B는 싸다(E371).
    # Llama FFN에서 mem2 1순위가 표준 선택보다 2.6% 나빴다(E379).
    if ACC_LIMIT > 32 or SP_LIMIT > 512:
        g = globals(); a0, s0 = g['ACC_LIMIT'], g['SP_LIMIT']
        try:
            g['ACC_LIMIT'], g['SP_LIMIT'] = 32, 512
            std = pick_blocks(K, N, M, m)
        finally:
            g['ACC_LIMIT'], g['SP_LIMIT'] = a0, s0
        for b in std:
            if b not in out: out.append(b)
    # E426: 상한은 **E380 조항까지 붙인 뒤**에 건다 — 그 조항이 후보를 덧붙이므로
    # 조립 도중에 자르면 효과가 없다(처음에 그렇게 넣었다가 상한이 안 먹었다).
    return out[:3]

def allocate(k, nj, m):
    """E315: 독립 matmul k개를 m대에. 'each' 또는 'jsplit'."""
    e = m*math.ceil(k/m)/k
    j = m*math.ceil(nj/m)/nj
    if abs(e-j) < 1e-9: return "동률(아무거나)", e, j
    return ("each" if e < j else "jsplit"), e, j

def plan(K, N, m=3, k_independent=1, M=128):
    out = []
    t = chunk_target(K, N)
    for b in pick_blocks(K, N, M, m):
        kc, ch = pick_kc(K, *b, t)
        nj = math.ceil((N//16)/b[1])
        out.append(dict(block=b, Kc=kc, chunks=ch, nj=nj,
                        intensity=round(intensity(b),2),
                        tiles=kc*(b[0]+b[1])))
    alloc = allocate(k_independent, out[0]['nj'], m) if out else None
    return dict(target=t, candidates=out, alloc=alloc, M=M,
                third_helps=(K <= 1024))

def layer_plan(stages, m=2, M=128):
    """단계 목록 [(이름, K, N, 독립matmul수)] 을 받아 층 전체 계획을 만든다.
    루프라인 비중으로 '층의 1/3' 판정을 하므로 측정 없이도 E325 지침을 적용할 수 있다."""
    out = []
    for nm, K, N, k in stages:
        p = plan(K, N, m=m, k_independent=k, M=M)
        p['name'], p['K'], p['N'], p['k'] = nm, K, N, k
        p['roof'] = k * K * N            # M은 공통이므로 비중 계산에 불필요
        out.append(p)
    tot = sum(p['roof'] for p in out) or 1
    for p in out:
        p['share'] = p['roof'] / tot
    return out

if __name__ == "__main__":
    import sys
    args = [a for a in sys.argv[1:] if not a.startswith('-')]
    m = 2; M = 128
    for a in sys.argv[1:]:
        if a.startswith('-m'): m = int(a[2:])
        elif a.startswith('--acc='): ACC_LIMIT = int(a[6:])
        elif a.startswith('--sp='):  SP_LIMIT  = int(a[5:])
        elif a.startswith('-M'):     M = int(a[2:])
    globals()['ACC_LIMIT'] = ACC_LIMIT; globals()['SP_LIMIT'] = SP_LIMIT
    # E406: 온칩 메모리 2배 판에서는 K*N 임계가 다르다 (블록이 (8,8)로 커지기 때문).
    if ACC_LIMIT > 32 or SP_LIMIT > 512:
        globals()['KN_THRESHOLD'] = 2.42e6
    stages = []
    for arg in args:
        # KxN  또는  이름=KxN  또는  이름=KxN*k (독립 matmul k개)
        nm, _, rest = arg.rpartition('=')
        k = 1
        if '*' in rest: rest, _, ks = rest.partition('*'); k = int(ks)
        K, N = (int(x) for x in rest.split('x'))
        stages.append((nm or f"{K}x{N}", K, N, k))
    L = layer_plan(stages, m=m, M=M)
    print(f"가속기 {m}대 기준.  한계: I*J <= {ACC_LIMIT}, Kc*(I+J) <= {SP_LIMIT}  (mem2 판이면 --acc=64 --sp=1024).\n비중은 루프라인(M·K·N) 기준의 근사다.\n")
    for p in L:
        c = p['candidates'][0]
        # E380: 후보가 셋 이상일 수 있는데 둘째만 찍고 있었다. 전부 보여준다.
        # E393: 차점 블록은 **자기 Kc**를 쓴다(블록마다 합법 범위가 다르다). 같이 찍는다.
        alt = "" if len(p['candidates']) == 1 else \
              "  (+" + ", ".join(f"{c2['block']} Kc={c2['Kc']}" for c2 in p['candidates'][1:]) \
              + " 도 재라)"
        # 비중은 루프라인 기준(측정 전이므로) — 실측 비중은 효율이 낮은 단계에서 더 크다.
        # BERT-base에서 FFN1/FFN2가 루프라인으로는 둘 다 33.3%인데 실측은 31.1/37.3%다.
        # 그래서 임계를 30%로 낮춰 보수적으로 잡는다.
        big = "  ** 큰 단계 -> 차점 블록도 한 번 재라 (E325)" if p['share'] >= 0.30 else ""
        # E369/E371: 큰 단계인데 후보가 하나뿐이면 잴 것이 없다. 강도 순위는 바이트 순위인데
        # 바이트-시간 지수가 1이 아니고(E365), 특히 **I만 큰 블록의 우위는 B 트래픽에서 오는데
        # B는 K가 크면 싸다**(E371). mem2 한계에서 규칙이 늘 (8,8)을 단독으로 골랐고
        # 10형상 중 6에서 (4,8)에 졌다. 그래서 후보가 하나면 차순위 강도 블록을 붙여 준다.
        # E392: 표준 한계(32/512)에서 합법인 블록 수가 온칩 메모리 증설의 값어치를 예측한다.
        # 23단계에서 1개면 평균 +39.3%(30~56%), 3개면 +9.3%(0~34%). 개수는 Kc만으로 정해지고
        # (Kc<=42면 3, 42<Kc<=64면 1, 그 위는 0) **2인 경우는 없다**.
        nlegal = sum(1 for b in [(4,4),(8,4),(4,8)]
                     if (128//16) % b[0] == 0 and (p['N']//16) % b[1] == 0
                     and c['Kc']*(b[0]+b[1]) <= 512 and b[0]*b[1] <= 32)
        mem = ("  ** 표준 한계 합법 블록 %d개 -> 온칩 메모리 2배가 이 단계에 %s (E392)"
               % (nlegal, "**크게** 값을 한다 (30~56%)" if nlegal <= 1 else "0~34% 값을 한다")
               ) if nlegal <= 1 else ""
        atlim = ""
        if len(p['candidates']) == 1 and p['share'] >= 0.30:
            rest = [b for b in blocks_for(128, p['N'])
                    if legal_kcs(p['K']//16, *b) and b != c['block']]
            if rest:
                # E379: 동점 처리는 pick_blocks와 **같은 규칙**을 써야 한다.
                # J 우선을 하드코딩했더니 Llama FFN에서 (4,8)을 지목했는데 실측 최적은
                # (8,4)였다. E412 이후 규칙은 N 대 K다 (33/33) — 여기도 같이 바꾼다.
                tb = (lambda b: (intensity(b), -b[0])) if p['N'] >= p['K'] \
                     else (lambda b: (intensity(b), -b[1]))
                nb = min(rest, key=tb)
                atlim = f"  ** 후보가 하나뿐 -> {nb} 도 재라 (E369/E371)"
        # E413/E414/E415: M>=256이면 (16,4)이 합법이고 (TI/I=1이라) B를 한 번만 읽는다.
        # 이득은 N/K의 단조 함수이고 교차는 N/K 3과 4 사이다 (M=256, m=3, 8개 점):
        #   N/K  0.25  1.0   2.0   3.0   4.0   6.0   8.0
        #        -22  -14/-17 -3.2 -3.1  +6.6  +8.8 +10.7 %
        # **강도 순위에는 넣지 않는다** — 강도 5.0인데 N/K<=1에서 강도 6.0 블록보다 느리다.
        # 강도 공식은 A 재읽기와 B 재읽기를 같은 값으로 세는데 실제로는 B 바이트가 더 비싸다
        # (바이트 총량이 1.25배 많은데도 이긴다). 그 이유가 "A가 L2에 든다"는 설명은
        # E417에서 사전 등록 후 **기각**됐다 — A가 같은 384 KB인 두 칸의 이득이 다르고
        # A가 L2를 넘어도 여전히 이긴다. 기제는 미해결이고 이득은 M과 함께 준다.
        wide = ""
        # E416: 이득은 **공유할 때만** 나온다 — m=1에서 -0.8~-6.5%, m=2에서 -0.6%,
        # m=3에서 +5.2~+10.7%. B 재읽기를 A 재읽기로 바꾸는 거래라 가속기가 적으면
        # 공유 자원에 여유가 있어 살 것이 없다. 그래서 m>=3 조건을 건다.
        # M별 실측 이득 (m=3, N/K=4, E417/E418):
        #            M=256   M=512   M=1024
        #  [768x3072] +5.5%   +4.4%   +4.1%
        #  [1536x6144] +6.3%   +2.4%   +1.3%
        # 상한은 없다(여섯 칸 전부 양수, seq 256~1024). 감쇠는 **K 의존**이다 (E420):
        # K=1536에서 M 2배당 정확히 -3.7 %p, K=768에서 -1.1 %p. 그래서 큰 M + 큰 K에서는
        # 1%대가 되어 추가 측정이 값을 못 할 수 있다.
        #
        # E420: 교차는 N/K 2와 3 **사이**이고 M을 따라 움직이지 않는다(K=1536, M=256/512
        # 두 행이 같은 자리에서 뒤집힘). 다만 교차 위치가 K에 딸린다 — N/K=3은 K=1536에서
        # +7.9%(전체 최대)인데 K=1024에서 -3.1%다. E421이 N/K=3 선의 K 축을 훑어
        # 이것이 **K의 함수가 아님**을 확인했다: K=768/1024/1280/1536/2048에서
        # +0.4 / -5.5 / +5.3 / +8.0 / -1.2 % — 비단조이고 부호가 세 번 바뀐다.
        # 어떤 변수(Kc, 청크, nj, TI/I, A·B·C, K*N)도 이 모양을 안 만든다. 따라서
        # `N >= 4K`(위양성 0)를 유지하고 N/K=3은 **재보고 정하는 구역**으로 남긴다.
        # 잴 때 주의: 같은 칸이 세션 간 2.4% 흔들린 적이 있으므로 이득 3 %p 미만은 읽지 말 것.
        if m >= 3 and p.get('M', 128) >= 256 and p['N'] >= 4*p['K'] \
           and (p['M']//16) % 16 == 0 and (p['N']//16) % 4 == 0:
            wkc, wch = pick_kc(p['K'], 16, 4, chunk_target(p['K'], p['N']))
            if wkc:   # E424: 화면에 Kc까지 찍어야 검증기가 채점할 수 있다
                wide = (f"  ** m=3·M>=256·N>=4K -> **(16, 4) Kc={wkc} 청크{wch} 도 재라**"
                        f" (E416: 단계 5~11%, 층 1.9%)")
        print(f"{p['name']:12s} [{p['K']:5d}x{p['N']:5d}] 비중 {p['share']*100:4.1f}%  "
              f"{c['block']} Kc={c['Kc']} 청크{c['chunks']} nj={c['nj']}{alt}{big}{atlim}{mem}{wide}")
        if p['k'] > 1:
            how, e, j = p['alloc']
            print(f"{'':12s}   독립 matmul {p['k']}개 -> **{how}**  (불균형 하나씩 {e:.3f} / J분할 {j:.3f})")
    third = [p['name'] for p in L if p['third_helps']]
    print(f"\n세 번째 가속기가 값을 하는 단계: {third if third else '없음'}"
          f"  (K <= 1024 기준. 클럭 불변 확인됨 — E352에서 다섯 크기, 편차 <=2.1 %p)")
