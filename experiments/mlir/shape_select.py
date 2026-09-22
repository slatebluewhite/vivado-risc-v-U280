#!/usr/bin/env python3
"""
Shape selection for a shapeshift Gemmini, as an MLIR pass would do it.

The pass walks each matmul-like op (linalg.matmul / linalg.batch_matmul /
tosa.matmul), reads M, K, N off the operand types, and picks the shape index that
config_ex rs1[12:10] should carry.

GEOMETRY. A shapeshift Gemmini of `rows x cols` PEs can run as `h x (cols*S)`
where `h = rows/S` and S is a supported segment count. One pass of that array
consumes K = h of the reduction and produces cols*S output columns. So:

  - `rows/S >= K` WAS a correctness constraint and is now GONE (E466): with K
    splitting (chunk the reduction into `K/h` passes that accumulate into the same
    C region) any shape is correct for any K. Verified against an absolute golden
    at K=16 with h=4, i.e. four chunks.
  - `h >= 4` WAS a hardware constraint and is GONE (E474): E456 moved the D
    preload's reverse index onto the feed window `total_rows`, which places the
    weight rows at the END of the window whatever h is. h = 2 verified against the
    absolute golden, and h = 1 as well (E506/E508 — after the per-bank adder fix).
  - `cols*S <= N` was in an earlier version of this rule and is REMOVED (E461).
    It was written as if idle columns cost time. Measured, they do not: at
    (M,K,N) = (4,4,16) the S=4 shape still wins, 32 cycles against S=1's 56,
    even though 48 of its 64 columns are wasted. Idle columns cost accumulator
    banks and write bandwidth, not cycles.

  cycles(S) = (passes - 1) * total_rows + M + h + max(h,4) + cols + 4
      passes     = ceil(N / (cols*S)) * ceil(K / h)
      total_rows = max(M, h, 4)
      h          = rows / S
  S* = the S minimising cycles(S), ties broken toward the LARGER S,
       subject to h >= 4.

That formula is EXACT on all 59 measured cells across FOUR array geometries
(16x16, 8x8/maxSeg2, 16x8, 8x8/maxSeg4 — E467-E474) — not fitted, every term is a mechanism: each extra pass costs a full `total_rows`
window, the last pass costs only the M real rows, the drain is `2*(h+cols)`, and
the `cols + 4` tail is the column pipeline.

Both clauses of the earlier rule are gone, replaced by counting passes directly:
  - the N clause ("cols*S <= N") was deleted because idle columns cost no time
    (E461), but the width still matters through `ceil` — when N < cols*S the tile
    count clamps at 1 and folding buys nothing there while still multiplying the
    chunk count.
  - the K clause ("rows/S >= K") was deleted because K splitting makes every shape
    correct (E466).

When neither ceil clamps, passes(S) = N*K/(rows*cols) is INDEPENDENT of S, so
folding costs nothing and still wins on pass length (`max(M,h,4)`) and drain
(`h + cols`) — measured 2.36x at equal pass count (E466). The tie-break toward
larger S is exactly that effect.
"""

def supported_segments(rows, max_segments):
    return [s for s in range(1, max_segments + 1) if rows % s == 0]


def passes(S, K, N, rows=16, cols=16):
    h = rows // S
    return max(1, -(-N // (cols * S))) * max(1, -(-K // h))


def cycles(S, M, K, N, rows=16, cols=16, floor=4, chunk_major=True):
    """Exact on every measured point (E460~E467 at floor 4; E528 at floor 2).
    floor < 4 hardware (shapeshift_row_floor): +1 is the accumulator skid queue's
    observation delay; the WAW stall is zero under CHUNK-MAJOR issue with nt >= 2
    and (ch-1) when nt == 1. Tile-major issue at floor < 4 with ch >= 4 hits the
    sustained-flow limit (E527) and must not be emitted."""
    h = rows // S
    nt = max(1, -(-N // (cols * S)))
    ch = max(1, -(-K // h))
    tr = max(M, h, floor)
    base = (nt * ch - 1) * tr + M + h + max(h, floor) + cols + 4
    if floor >= 4:
        return base
    stall = 0 if (chunk_major and nt >= 2) else (ch - 1)
    return base + 1 + stall


def select_shape(M, K, N, rows=16, cols=16, max_segments=4, floor=4):
    """Return (shape_index, S, h, why). `floor` = the hardware's
    shapeshift_row_floor; the useful ladder is h >= floor (E493/E529), and when
    floor < 4 the pass MUST also emit the matmul in chunk-major order (E528)."""
    segs = supported_segments(rows, max_segments)
    legal = [s for s in segs if rows // s >= floor] or [1]
    best = min(legal, key=lambda s: (cycles(s, M, K, N, rows, cols, floor), -s))
    h = rows // best
    waste = max(1, (cols * best) // max(N, 1)) if cols * best > N else 1
    why = "cycles " + "/".join(str(cycles(s, M, K, N, rows, cols, floor)) for s in legal)
    if cols * best > N:
        why += f", 다만 width={cols*best} > N={N} 이라 accumulator 를 {waste}배 낭비"
    return segs.index(best), best, h, why


def utilization(M, K, N, s, rows=16, cols=16):
    """Fraction of PEs doing useful work in one pass, ignoring pipeline overhead."""
    h = rows // s
    used_rows = min(K, h)
    used_cols = min(N, cols * s)
    return used_rows * used_cols / (rows * cols)


if __name__ == "__main__":
    print(f"{'M,K,N':>16}{'pick S':>8}{'h x width':>12}{'util S*':>9}{'util S=1':>10}  이유")
    cases = [
        (16, 16, 16), (16, 8, 32), (16, 4, 64),      # E456 에서 실측한 세 모양
        (128, 768, 768),                              # BERT projection
        (128, 3072, 768),                             # BERT FFN2
        (1, 4096, 4096),                              # GEMV (decode)
        (16, 2, 64),                                  # K 가 하한(4)보다 작다
        (4, 4, 16),                                   # E461 [A]: N 이 좁아도 S=4 가 이긴다
        (16, 16, 16),                                 # E467 예측: 접기가 **지는** 첫 지점
    ]
    for M, K, N in cases:
        idx, s, h, why = select_shape(M, K, N)
        u1 = utilization(M, K, N, 1)
        us = utilization(M, K, N, s)
        print(f"{f'{M},{K},{N}':>16}{s:>8}{f'{h}x{16*s}':>12}{us:>8.1%}{u1:>10.1%}  {why}")
