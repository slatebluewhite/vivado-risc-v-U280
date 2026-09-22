// stress_raw.c — 원시 preload/compute 경로를 고밀도로 반복한다 (E83 후속)
//
// E83에서 확인: 실패는 tiled_matmul_auto(루프 FSM) 경로에서는 안 나오고
// matmul.c의 원시 preload/compute 경로에서만 나온다(3,600회 중 4건).
//
// matmul.c는 한 실행에 이 시퀀스를 ~32회만 수행하고 프로세스를 끝낸다.
// 이 테스트는 같은 시퀀스를 한 프로세스 안에서 수만 번 반복해 사건률을 끌어올린다.
// 목표: 수정 전/후 판정을 몇 시간이 아니라 몇 분에 끝낼 수 있게 하는 것.
//
// matmul.c와 같은 구성을 유지한다:
//   - OUTPUT_STATIONARY, 활성화/시프트 설정
//   - preload(D 사용) + compute_preloaded 조합  ← D 피연산자가 관여하는 경로
//   - preload_zeros + compute_accumulated 조합

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif
#include "include/gemmini_testutils.h"

#define DEFAULT_ITERS 20000

static elem_t A[DIM][DIM] row_align(1);
static elem_t B[DIM][DIM] row_align(1);
static elem_t D[DIM][DIM] row_align(1);
static elem_t C[DIM][DIM] row_align(1);
static elem_t GOLD[DIM][DIM];

int main(int argc, char **argv) {
  size_t iters = argc > 1 ? (size_t)atoi(argv[1]) : DEFAULT_ITERS;

#ifndef BAREMETAL
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) { perror("mlockall failed"); exit(1); }
#endif
  gemmini_flush(0);

  // 값을 작게 잡아 포화를 피한다 — 포화하면 서로 다른 오류가 같은 값으로 뭉개진다.
  for (size_t i = 0; i < DIM; i++)
    for (size_t j = 0; j < DIM; j++) {
      A[i][j] = (elem_t)((int)((i * 3 + j * 5) % 7) - 3);
      B[i][j] = (elem_t)((int)((i * 5 + j * 2) % 5) - 2);
      D[i][j] = (elem_t)((int)((i + j) % 3) - 1);
    }

  printf("=== STRESS RAW DIM=%d iters=%lu ===\n", DIM, (unsigned long)iters);
  fflush(stdout);

  // 주의: OUTPUT_STATIONARY + preload(D) 의 정확한 의미론을 CPU로 재현하려던
  // 첫 시도는 20,000회 전부 diff=239로 실패했다 — 하드웨어가 아니라 내 참조가 틀린 것이다
  // (일정한 diff는 간헐 오류가 아니라 결정론적 불일치를 뜻한다).
  //
  // 이 테스트의 목적은 절대 정확성이 아니라 **간헐적 이탈 검출**이므로,
  // 첫 실행 결과를 기준으로 삼고 이후 반복이 그것과 달라지는지만 본다.
  // 입력이 고정이므로 하드웨어는 매번 같은 값을 내야 한다.

  const int A_addr = 0, B_addr = DIM, D_addr = 2 * DIM, C_addr = 3 * DIM;

  int have_ref = 0;
  size_t bad = 0;
  for (size_t it = 0; it < iters; it++) {
    memset(C, 0, sizeof(C));

    gemmini_mvin(A, A_addr);
    gemmini_mvin(B, B_addr);
    gemmini_mvin(D, D_addr);

    gemmini_extended_config_ex(OUTPUT_STATIONARY, NO_ACTIVATION, 0, 1, false, false);

    // ★ D를 preload 하는 조합 — A/B/D 세 피연산자가 모두 관여한다
    gemmini_preload(D_addr, C_addr);
    gemmini_compute_preloaded(A_addr, B_addr);

    gemmini_mvout(C, C_addr);
    gemmini_fence();

    if (!have_ref) {                 // 첫 결과를 기준으로 채택
      memcpy(GOLD, C, sizeof(GOLD));
      have_ref = 1;
      printf("ref: %d %d %d %d\n", C[0][0], C[0][1], C[0][2], C[0][3]);
      fflush(stdout);
      continue;
    }
    size_t diff = 0;
    for (size_t i = 0; i < DIM; i++)
      for (size_t j = 0; j < DIM; j++)
        if (C[i][j] != GOLD[i][j]) diff++;

    if (diff != 0) {
      bad++;
      printf("RAW_FAIL it=%lu diff=%lu\n", (unsigned long)it, (unsigned long)diff);
      fflush(stdout);
      if (bad == 1) {
        // 첫 실패의 값을 남긴다 — 오류 형태를 알아야 원인을 좁힐 수 있다
        printf("  got : %d %d %d %d\n", C[0][0], C[0][1], C[0][2], C[0][3]);
        printf("  gold: %d %d %d %d\n", GOLD[0][0], GOLD[0][1], GOLD[0][2], GOLD[0][3]);
        fflush(stdout);
      }
    }
  }

  printf("RAW_DONE iters=%lu bad=%lu\n", (unsigned long)iters, (unsigned long)bad);
  fflush(stdout);
  return bad == 0 ? 0 : 1;
}
