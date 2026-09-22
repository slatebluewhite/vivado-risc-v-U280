// stress_cfg.c — 설정 전환이 간헐 오답의 방아쇠인지 검증한다 (E82 후속)
//
// E82에서 확인: 같은 설정으로 768,000 타일 연산을 돌려도 실패가 0이다.
// 즉 실패는 "연산을 많이 해서" 생기는 것이 아니다.
//
// matmul-linux(실패가 나는 쪽)와의 차이 중 유력한 것은 **설정을 계속 바꾼다**는 점이다.
// 이 테스트는 연산 내용을 고정한 채 **매 반복마다 활성화 설정만 번갈아** 바꾼다.
// 연산량 대비 설정 전환 횟수가 극대화된다.
//
// 정답: 활성화는 출력단에 적용되므로 RELU 결과는 기본 결과에 max(0,·)를 취한 것과 같다.
// 따라서 기준값 하나에서 두 설정의 정답을 모두 유도할 수 있다.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif
#include "include/gemmini_testutils.h"

#define SZ 32                  // 작게 잡아 단위 시간당 전환 횟수를 늘린다
#define DEFAULT_ITERS 4000

static elem_t A[SZ][SZ] row_align(1);
static elem_t B[SZ][SZ] row_align(1);
static elem_t C[SZ][SZ] row_align(1);
static elem_t GOLD_PLAIN[SZ][SZ];
static elem_t GOLD_RELU[SZ][SZ];

static void run(int act) {
  memset(C, 0, sizeof(C));
  tiled_matmul_auto(SZ, SZ, SZ, (elem_t*)A, (elem_t*)B, NULL, (elem_t*)C,
      SZ, SZ, SZ, SZ, MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY,
      act, ACC_SCALE_IDENTITY, 0, false, false, false, false, false, 0, WS);
}

int main(int argc, char **argv) {
  size_t iters = argc > 1 ? (size_t)atoi(argv[1]) : DEFAULT_ITERS;

#ifndef BAREMETAL
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) { perror("mlockall failed"); exit(1); }
#endif
  gemmini_flush(0);

  for (size_t i = 0; i < SZ; i++)
    for (size_t j = 0; j < SZ; j++) {
      A[i][j] = (elem_t)((int)((i * 7 + j * 3) % 15) - 7);
      B[i][j] = (elem_t)((int)((i * 5 + j * 11) % 13) - 6);
    }

  printf("=== STRESS CFG SZ=%d iters=%lu ===\n", SZ, (unsigned long)iters);
  fflush(stdout);

  // 기준값을 만들고 CPU로 검증한다 (기준이 틀리면 이후 비교가 무의미 — E63)
  run(NO_ACTIVATION);
  memcpy(GOLD_PLAIN, C, sizeof(GOLD_PLAIN));

  size_t cpu_bad = 0;
  for (size_t i = 0; i < SZ; i++)
    for (size_t j = 0; j < SZ; j++) {
      int64_t acc = 0;
      for (size_t k = 0; k < SZ; k++) acc += (int64_t)A[i][k] * (int64_t)B[k][j];
      if (acc > 127) acc = 127; else if (acc < -128) acc = -128;
      if ((int64_t)GOLD_PLAIN[i][j] != acc) cpu_bad++;
      GOLD_RELU[i][j] = acc > 0 ? (elem_t)acc : 0;
    }
  printf("baseline vs CPU: %lu / %d mismatched\n", (unsigned long)cpu_bad, SZ * SZ);
  fflush(stdout);
  if (cpu_bad != 0) { printf("CFG_INVALID\n"); return 2; }

  // RELU 기준값도 하드웨어로 한 번 확인해 둔다 (유도가 맞는지)
  run(RELU);
  size_t relu_bad = 0;
  for (size_t i = 0; i < SZ; i++)
    for (size_t j = 0; j < SZ; j++)
      if (C[i][j] != GOLD_RELU[i][j]) relu_bad++;
  printf("relu baseline: %lu / %d mismatched\n", (unsigned long)relu_bad, SZ * SZ);
  fflush(stdout);
  if (relu_bad != 0) { printf("CFG_INVALID (relu 기준 불일치)\n"); return 2; }

  size_t bad = 0, switches = 0;
  for (size_t it = 0; it < iters; it++) {
    int act = (it & 1) ? RELU : NO_ACTIVATION;   // ★ 매 반복 설정 전환
    run(act);
    switches++;

    elem_t (*g)[SZ] = (act == RELU) ? GOLD_RELU : GOLD_PLAIN;
    size_t diff = 0;
    for (size_t i = 0; i < SZ; i++)
      for (size_t j = 0; j < SZ; j++)
        if (C[i][j] != g[i][j]) diff++;

    if (diff != 0) {
      bad++;
      printf("CFG_FAIL it=%lu act=%d diff=%lu\n",
             (unsigned long)it, act, (unsigned long)diff);
      fflush(stdout);
    }
  }

  unsigned long tiles = (SZ / DIM) * (SZ / DIM) * (SZ / DIM);
  printf("CFG_DONE iters=%lu switches=%lu bad=%lu tiles_per_iter=%lu total_tiles=%lu\n",
         (unsigned long)iters, (unsigned long)switches, (unsigned long)bad,
         tiles, (unsigned long)(tiles * iters));
  fflush(stdout);
  return bad == 0 ? 0 : 1;
}
