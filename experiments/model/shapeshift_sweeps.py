#!/usr/bin/env python3
"""
Shapeshift 트랙의 모형 계산 전부를 재생성하는 스크립트 (E497~E505, 정정 포함).

E510 감사가 "계산 스크립트 미보존이라 재검증 불가" 로 분류한 표들을 재현 가능하게
만들기 위해 존재한다. 각 함수가 JOURNAL 의 해당 항목 표를 다시 만든다.
감사(E510)에서 확인된 정정이 반영돼 있다:
  - E503: "M>=32 에서 0.251x" 는 부정확 — 0.251 은 M=128 에서야 나온다 (M=32 는 0.256).
  - E504: "여섯 점에서 <=2%" 는 근거 불명 — 격자 전체 최대 편차는 4.6% (M=16,K=16 부근; 자체 출력과 일치).
  - E505: "h<4 는 무승부 자체가 불가능" 은 **min/max 혼동 오류** — 옳은 명제는
    "점근(K>>h) 곱이 h/max(h,4) 로 수렴" 이다. K<=h 에서는 h<4 도 H/max(h,4) 배까지 이긴다.
  - E498: decode GEMV 총 바이트는 16.0 MiB (문서의 16.4 는 KiB/1000 단위 혼용).
  - E501: 닫힌 형태의 maxSeg 는 **유효 사다리(h>=4)** 기준이다. h<4 shape 를 사다리에
    넣으면 공식이 깨진다 (예: 8x8 리터럴 maxSeg=4, B=64 -> 공식 4, 실제 2).

측정 대역폭 (이 보드, CLAUDE.md): DRAM 읽기 4.75 / 쓰기 3.54 B/cycle, L2 24.04, L2 = 512 KB.
"""
from math import ceil

B_R, B_W, B_L2, L2 = 4.75, 3.54, 24.04, 512 * 1024


def cycles(rows, cols, S, M, K, N, floor=4, chunk_major=True):
    """E467 의 사이클 모형 (xsim 오차 0). floor < 4 는 E528 의 확장:
    치환(max 에 floor) + 1(skid queue 관측 지연) + WAW stall — chunk-major 발행이고
    nt >= 2 면 stall 0, nt = 1 이면 (ch-1). tile-major 는 ch<=2 까지만 모형이 맞고
    ch=4·nt>=2 부터 유량 한계(E527)라 모형 밖.
    E544 정밀화: stall 은 같은 acc 주소 재방문 간격(nt=1 이면 tr)이 서비스 간격 3 보다
    짧을 때만 생긴다 — M=4 면 tr=4 >= 3 이라 nt=1·ch=2 여도 stall 0 (32x32 ms16 f2 실측
    49 = base+1, 등록 모형 50 은 1 빗나감)."""
    h = rows // S
    nt = max(1, ceil(N / (cols * S)))
    ch = max(1, ceil(K / h))
    p = nt * ch
    tr = max(M, h, floor)
    base = (p - 1) * tr + M + h + max(h, floor) + cols + 4
    if floor >= 4:
        return base
    revisit = (nt * tr) if (chunk_major and nt >= 2) else tr
    stall = (ch - 1) if revisit < 3 else 0
    return base + 1 + stall


def segs(rows, maxSeg=8, floor=4):
    """유효 사다리: h >= floor (E493/E494; floor=2 확장은 E516~E528).
    E501 닫힌 형태의 maxSeg 는 이 사다리 기준이다."""
    return [S for S in (1, 2, 4, 8, 16) if rows % S == 0 and rows // S >= floor and S <= maxSeg]


def t_real(rows, cols, S, M, K, N, B=None):
    """roofline max(compute, memory). B 를 주면 단일 대역폭, 안 주면 E498 방식
    (L2 안이면 tot/B_L2, 밖이면 rd/B_R + wr/B_W)."""
    rd = M * K + K * N
    wr = M * N
    tot = rd + wr
    mem = tot / B if B else (tot / B_L2 if tot <= L2 else rd / B_R + wr / B_W)
    return max(cycles(rows, cols, S, M, K, N), mem)


def gain_folded(rows, cols, M, K, N, B=None, maxSeg=8):
    ss = segs(rows, maxSeg)
    return t_real(rows, cols, 1, M, K, N, B) / min(t_real(rows, cols, S, M, K, N, B) for S in ss)


def e497_table():
    CASES = [("prefill proj", 128, 768, 768), ("decode GEMV", 1, 4096, 4096),
             ("ResNet 1x1 C=3", 3136, 3, 64), ("small batch K=8", 8, 8, 512)]
    print("[E497] compute 스케줄만 (메모리 없음):")
    for nm, M, K, N in CASES:
        row = []
        for r in (8, 16, 32):
            ss = segs(r)
            c1 = cycles(r, r, 1, M, K, N)
            cb = min(cycles(r, r, S, M, K, N) for S in ss)
            row.append(f"{c1/cb:.2f}x")
        print(f"  {nm:<18}" + "  ".join(f"{x:>7}" for x in row))


def e498_table():
    print("[E498] roofline 을 넣은 실제 이득 (16x16):")
    for nm, M, K, N in [("LLM decode GEMV", 1, 4096, 4096), ("BERT decode proj", 1, 768, 768),
                        ("ResNet 1x1 C=3", 3136, 3, 64), ("small batch K=8", 8, 8, 512)]:
        rd, wr = M * K + K * N, M * N
        print(f"  {nm:<18} compute {gain_folded(16,16,M,K,N,B=10**12):>5.2f}x  실제 {gain_folded(16,16,M,K,N):>5.2f}x"
              f"   ({(rd+wr)/2**20:.1f} MiB)")


def e501_closed_form():
    DECODE = [(1, 4096, 4096, 4), (1, 4096, 16384, 1), (1, 16384, 4096, 1)]
    def g(r, ms, B):
        ss = segs(r, ms)
        b = sum(t_real(r, r, 1, M, K, N, B) * rep for M, K, N, rep in DECODE)
        d = sum(min(t_real(r, r, S, M, K, N, B) for S in ss) * rep for M, K, N, rep in DECODE)
        return b / d
    print("[E501] 이득 = min(유효 maxSeg, B/cols)  — 유효 사다리(h>=4) 에서만:")
    worst = 0
    for r, ms in [(16, 4), (16, 2), (32, 8), (32, 4), (8, 2)]:
        for B in (16, 24.04, 32, 48, 64, 96, 128, 256):
            if B < r:
                continue
            eff = max(segs(r, ms))
            pred = min(eff, B / r)
            got = g(r, ms, B)
            worst = max(worst, abs(got - pred) / pred)
    print(f"  유효 조합 전수 최대 상대오차: {100*worst:.3f}%")
    print("  주의: 사다리에 h<4 를 넣으면 공식이 깨진다 (8x8 maxSeg=4 리터럴, B=64: 공식 4, 실제",
          f"{g(8,4,64):.1f})")


def e504_grid():
    print("[E504] 접은 8x8(h=4) vs 16x16(H=16), N=1024 — 곱 공식 대비 편차:")
    def prod(M, K):  # E505 의 4-바닥 수정판
        return (ceil(K / 16) / ceil(K / 4)) * (max(M, 16, 4) / max(M, 4, 4))
    dev = []
    for M in (1, 4, 8, 16, 3136):
        for K in (2, 4, 8, 16, 32, 128, 576):
            got = cycles(16, 16, 1, M, K, 1024) / cycles(8, 8, 2, M, K, 1024)
            dev.append(abs(got - prod(M, K)) / prod(M, K))
    print(f"  격자 최대 편차 {100*max(dev):.1f}%  평균 {100*sum(dev)/len(dev):.1f}%"
          "   (E504 의 '<=2%' 는 격자 전체에서는 성립하지 않는다 — E510 정정)")


def e505_corrected():
    print("[E505 정정] 곱의 점근과 상한 — 원문의 '무승부 불가' 는 min/max 혼동:")
    for h, H in [(8, 32), (4, 16), (2, 32), (2, 16), (1, 16)]:
        asym = h / max(h, 4)          # K >> h 에서의 곱 (M 인자 최대일 때)
        peak = H / max(h, 4)          # K <= h, M 작을 때의 곱 (전역 최대)
        print(f"  h={h:>2} H={H:>2}: 점근 {asym:.2f} ({'무승부' if asym>=1 else '영구 열세'}),"
              f" 전역 최대 {peak:.1f}배 (K<={h} 한정)")
    print("  검산 (모형): h=2,H=32, M=1, K=2 ->",
          f"{cycles(32,32,1,1,2,1024)/cycles(8,8,4,1,2,1024):.2f}배 (이긴다);",
          f"K=1024 -> {cycles(32,32,1,1,1024,1024)/cycles(8,8,4,1,1024,1024):.3f} (점근 0.5)")


if __name__ == "__main__":
    e497_table(); print()
    e498_table(); print()
    e501_closed_form(); print()
    e504_grid(); print()
    e505_corrected()
