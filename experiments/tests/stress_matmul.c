// stress_matmul.c — 간헐 오답의 사건률을 올리기 위한 고밀도 검사 (E81 계획 C)
//
// 문제: matmul-linux는 한 실행에 mesh 연산이 ~32회뿐이라 실패율이 0.11%에 그치고,
// 결정적 판정에 6,000회(5시간)가 필요하다.
//
// 해법: 한 번의 실행에서 같은 행렬곱을 수천 번 반복하고 매번 검증한다.
// N=128이면 타일 (128/16)^3 = 512개 → 1회 반복당 mesh 연산 512회.
// 같은 벽시계 시간에 검사하는 연산 수가 훨씬 많아진다.
//
// 정답은 CPU로 한 번만 계산해 두고(느리므로), 이후에는 그것과 비교만 한다.
// 입력이 고정이므로 하드웨어 출력도 매번 같아야 한다 — 달라지면 그 자체가 오류다.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif
#include "include/gemmini_testutils.h"

#define SZ 128                 // 행렬 한 변
#define DEFAULT_ITERS 2000

static elem_t A[SZ][SZ] row_align(1);
static elem_t B[SZ][SZ] row_align(1);
static elem_t C[SZ][SZ] row_align(1);
static elem_t GOLD[SZ][SZ];

int main(int argc, char **argv) {
  size_t iters = argc > 1 ? (size_t)atoi(argv[1]) : DEFAULT_ITERS;

#ifndef BAREMETAL
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) { perror("mlockall failed"); exit(1); }
#endif
  gemmini_flush(0);

  // 결정적 입력. 값이 작아 누적이 포화하지 않게 한다 —
  // 포화하면 서로 다른 오류가 같은 값으로 뭉개져 검출력이 떨어진다.
  for (size_t i = 0; i < SZ; i++)
    for (size_t j = 0; j < SZ; j++) {
      A[i][j] = (elem_t)((int)((i * 7 + j * 3) % 15) - 7);
      B[i][j] = (elem_t)((int)((i * 5 + j * 11) % 13) - 6);
    }

  printf("=== STRESS MATMUL SZ=%d iters=%lu ===\n", SZ, (unsigned long)iters);
  fflush(stdout);

  // 1회 실행해 그 결과를 기준으로 삼되, CPU로 검증해 기준 자체가 옳은지 확인한다.
  tiled_matmul_auto(SZ, SZ, SZ, (elem_t*)A, (elem_t*)B, NULL, (elem_t*)C,
      SZ, SZ, SZ, SZ, MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY,
      NO_ACTIVATION, ACC_SCALE_IDENTITY, 0, false, false, false, false, false, 0, WS);
  memcpy(GOLD, C, sizeof(GOLD));

  size_t cpu_bad = 0;
  for (size_t i = 0; i < SZ; i++)
    for (size_t j = 0; j < SZ; j++) {
      int64_t acc = 0;
      for (size_t k = 0; k < SZ; k++) acc += (int64_t)A[i][k] * (int64_t)B[k][j];
      if (acc > 127) acc = 127; else if (acc < -128) acc = -128;
      if ((int64_t)GOLD[i][j] != acc) cpu_bad++;
    }
  printf("baseline vs CPU: %lu / %d mismatched\n",
         (unsigned long)cpu_bad, SZ * SZ);
  fflush(stdout);
  if (cpu_bad != 0) {
    // 기준이 이미 틀렸다면 반복 비교는 의미가 없다.
    printf("STRESS_INVALID (기준 자체가 CPU와 불일치)\n");
    return 2;
  }

  size_t bad_runs = 0;
  for (size_t it = 0; it < iters; it++) {
    memset(C, 0, sizeof(C));
    tiled_matmul_auto(SZ, SZ, SZ, (elem_t*)A, (elem_t*)B, NULL, (elem_t*)C,
        SZ, SZ, SZ, SZ, MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY,
        NO_ACTIVATION, ACC_SCALE_IDENTITY, 0, false, false, false, false, false, 0, WS);

    size_t diff = 0;
    for (size_t i = 0; i < SZ; i++)
      for (size_t j = 0; j < SZ; j++)
        if (C[i][j] != GOLD[i][j]) diff++;

    if (diff != 0) {
      bad_runs++;
      printf("STRESS_FAIL it=%lu diff=%lu\n", (unsigned long)it, (unsigned long)diff);
      fflush(stdout);
    }
  }

  // 한 반복이 검사한 mesh 타일 연산 수 = (SZ/DIM)^3
  unsigned long tiles = (SZ / DIM) * (SZ / DIM) * (SZ / DIM);
  printf("STRESS_DONE iters=%lu bad=%lu tiles_per_iter=%lu total_tiles=%lu\n",
         (unsigned long)iters, (unsigned long)bad_runs, tiles,
         (unsigned long)(tiles * iters));
  fflush(stdout);
  return bad_runs == 0 ? 0 : 1;
}
