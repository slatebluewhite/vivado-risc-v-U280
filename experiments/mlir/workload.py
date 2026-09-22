#!/usr/bin/env python3
"""
What the validated cycle model says about real workloads, as a function of ARRAY SIZE.

The model (E467, and out-of-sample confirmed at h=2 in E494 and at rows=32 in E496 —
exact on every cell measured, rows 8..32, h 32..2):

    cycles(S) = (passes - 1) * total_rows + M + h + max(h,4) + cols + 4
        passes     = ceil(N / (cols*S)) * ceil(K / h)
        total_rows = max(M, h, 4)
        h          = rows / S

The useful ladder depth is rows/4 (E493/E494): once h <= 4 the `max(h,4)` floor stops
total_rows from shrinking, so folding further only adds chunks.

CAVEAT — and E498 QUANTIFIED IT, with a result that changes the conclusion:
this model counts COMPUTE-SCHEDULE cycles only. Feeding this board's measured
bandwidths (DRAM 4.75 B/cycle read, L2 24.04, L2 = 512 KB) into a
`max(compute, memory)` roofline collapses the decode rows to **1.00x** — a decode
GEMV streams its whole weight matrix per token, so the array already idles ~98 %
and shortening the compute schedule changes nothing. What survives is the
pass-count source (N tiles) on work that fits in L2: ResNet 1x1 1.44x,
small batch 1.62x. Read every number below as an UPPER BOUND.
"""

CASES = [
    ("BERT projection (prefill)",      128, 768, 768),
    ("BERT FFN1 (prefill)",            128, 768, 3072),
    ("attention scores Q.K^T",         128,  64,  128),
    ("BERT projection (decode, M=1)",    1, 768, 768),
    ("LLM decode GEMV",                  1, 4096, 4096),
    ("ResNet 1x1 conv, C_in=3",       3136,   3,   64),
    ("depthwise 3x3 (per group)",     3136,   9,    1),
    ("small batch, K=8",                 8,   8,  512),
]

GEOMETRIES = [(8, 8), (16, 16), (32, 32)]


def cycles(rows, cols, S, M, K, N):
    h = rows // S
    p = max(1, -(-N // (cols * S))) * max(1, -(-K // h))
    return (p - 1) * max(M, h, 4) + M + h + max(h, 4) + cols + 4


def segs(rows):
    """Shapes worth building: the ladder saturates at h = 4 (E493/E494)."""
    return [S for S in (1, 2, 4, 8, 16) if rows % S == 0 and rows // S >= 4]


def best(rows, cols, M, K, N):
    ss = segs(rows)
    c = {S: cycles(rows, cols, S, M, K, N) for S in ss}
    pick = min(ss, key=lambda S: (c[S], -S))
    return pick, c


if __name__ == "__main__":
    print("접기 이득 = cycles(S=1) / cycles(고른 S).  사다리는 h>=4 까지만 (E493/E494).")
    print()
    hdr = "".join(f"{f'{r}x{c}':>16}" for r, c in GEOMETRIES)
    print(f"{'workload':<30}{'M,K,N':>16}{hdr}")
    print("-" * (46 + 16 * len(GEOMETRIES)))
    for name, M, K, N in CASES:
        row = ""
        for r, c in GEOMETRIES:
            pick, cy = best(r, c, M, K, N)
            row += f"{f'{cy[1]/cy[pick]:.2f}x (S={pick})':>16}"
        print(f"{name:<30}{f'{M},{K},{N}':>16}{row}")
    print()
    print("읽는 법 — 이득의 원천이 둘이고, 배열 크기에 **반대로** 반응한다 (E497):")
    print("  (a) pass 길이  total_rows = max(M,h,4):  M 이 작을 때만 h 를 따라 준다.")
    print("      배열이 크면 h 도 커서 줄일 여지가 많다  ->  이득이 **커진다** (상한 rows/4)")
    print("  (b) pass 수    ceil(N/(cols*S)):  N 이 넓을 때만 S 배로 준다.")
    print("      배열이 넓으면 접기 전에 이미 N 이 몇 타일 안 된다  ->  이득이 **줄어든다**")
    print()
    print("그래서 권장은 배열 크기가 아니라 **M 과 N** 으로 갈린다:")
    print("  M 이 작으면 (decode)      -> 큰 배열일수록 접기가 이득이고, 면적도 큰 배열이 싸다")
    print("  M 이 크고 N 이 좁으면     -> 작은 배열에서만 이득이 있다")
    print("  M >= rows 이고 K 가 크면  -> 구조적으로 이득 0 (prefill). total_rows=M 로 고정")
