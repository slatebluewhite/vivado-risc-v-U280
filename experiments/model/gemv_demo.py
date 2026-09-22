#!/usr/bin/env python3
"""
실증 트랙 (E547~): 현재 보드에서 접기 이득이 실재하는 GEMV/small-M 워크로드 후보의
예측표. E501 의 닫힌 형태(이득 = min(유효 maxSeg, B/cols))와 E467 사이클 모형 위에
roofline 을 씌운다 — 전부 검증된 도구의 조합이고 새 가정은 없다.

L2 상주 판정: 한 '반복 단위'(decode 라면 토큰당 도는 가중치 전체)가 512 KB 이하일 때.
넘으면 토큰마다 재스트리밍이라 DRAM 대역폭이 적용된다 (E498).
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from shapeshift_sweeps import cycles, segs, B_R, B_L2, L2

# (이름, [(M,K,N) 반복 리스트], 반복당 가중치 바이트) — int8
CASES = [
    ("GRU H=128 (1 timestep)", [(1, 128, 128)] * 6, 6 * 128 * 128),
    ("GRU H=256 (1 timestep)", [(1, 256, 256)] * 6, 6 * 256 * 256),
    ("MLP head 256-256-10",    [(1, 256, 256), (1, 256, 10)], 256 * 256 + 256 * 10),
    ("tf decode H=192 layer",  [(1, 192, 192)] * 4 + [(1, 192, 768), (1, 768, 192)],
                               4 * 192 * 192 + 2 * 192 * 768),
    ("tf decode H=512 layer",  [(1, 512, 512)] * 4 + [(1, 512, 2048), (1, 2048, 512)],
                               4 * 512 * 512 + 2 * 512 * 2048),
    ("ResNet 1x1 C=3",         [(3136, 3, 64)], 3 * 64),
    ("small batch K=8",        [(8, 8, 512)], 8 * 512),
    ("LLM decode H=4096 proj", [(1, 4096, 4096)], 4096 * 4096),
]


def t_case(rows, cols, S, mats, wbytes):
    resident = wbytes <= L2
    B = B_L2 if resident else B_R
    t = 0
    for M, K, N in mats:
        comp = cycles(rows, cols, S, M, K, N)
        mem = (M * K + K * N + M * N) / B
        t += max(comp, mem)
    return t, resident


def table(rows, cols):
    print(f"\n[{rows}x{cols}, floor 4, 유효 사다리 {segs(rows)}]")
    print(f"{'workload':<26}{'상주':<5}{'S=1 cycles':>12}{'best':>10}{'이득':>7}")
    for name, mats, wb in CASES:
        t1, res = t_case(rows, cols, 1, mats, wb)
        best = min(t_case(rows, cols, S, mats, wb)[0] for S in segs(rows))
        print(f"{name:<26}{'L2' if res else 'DRAM':<5}{t1:>12.0f}{best:>10.0f}{t1/best:>6.2f}x")


if __name__ == "__main__":
    print("이득 = t(S=1)/t(best S), roofline max(compute, bytes/B) — B는 상주 여부로 결정")
    print(f"B_L2={B_L2}, B_DRAM(read)={B_R}, L2={L2//1024} KB")
    table(16, 16)
    table(8, 8)
    print("\n닫힌 형태 검산: min(유효 maxSeg, B/cols) — 16x16: min(4, 1.50)=1.50 /"
          " 8x8: min(2, 3.01)=2.00 (decode 극한)")
