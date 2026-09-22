// bf16_raw_mm.c — 원시 preload/compute로도 곱이 0인지 확인한다 (E99)
// E97은 tiled_matmul_auto(라이브러리 경로)만 시험했다. 원시 ISA에서도 0이면
// 결함은 명령 발행 방식과 무관하고 mesh 자체에 있다.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif
#include "include/gemmini_testutils.h"

static elem_t A[DIM][DIM] row_align(1);
static elem_t B[DIM][DIM] row_align(1);
static elem_t C[DIM][DIM] row_align(1);

static inline float b2f(uint16_t b){union{uint32_t u;float f;}c;c.u=((uint32_t)b)<<16;return c.f;}
static inline uint16_t f2b(float f){union{uint32_t u;float f;}c;c.f=f;uint32_t l=(c.u>>16)&1u,x=0x7fffu+l;return (uint16_t)((c.u+x)>>16);}

int main() {
#ifndef BAREMETAL
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) { perror("mlockall failed"); exit(1); }
#endif
  gemmini_flush(0);

  // A는 단위행렬, B는 5.0 → A*B의 각 원소는 5.0이어야 한다
  for (size_t i = 0; i < DIM; i++)
    for (size_t j = 0; j < DIM; j++) {
      A[i][j] = f2b(i == j ? 1.0f : 0.0f);
      B[i][j] = f2b(5.0f);
      C[i][j] = 0;
    }

  const int A_addr = 0, B_addr = DIM, C_addr = 2 * DIM;
  printf("=== BF16 RAW MATMUL (DIM=%d) ===\n", DIM);
  printf("A=단위행렬, B=5.0 → 기대 C=5.0\n"); fflush(stdout);

  gemmini_config_ld(DIM * sizeof(elem_t));
  gemmini_config_st(DIM * sizeof(elem_t));
  gemmini_mvin(A, A_addr);
  gemmini_mvin(B, B_addr);
  gemmini_fence();

  // WS: preload_zeros로 바이어스 없이, compute_preloaded(A, B)
  gemmini_extended_config_ex(WEIGHT_STATIONARY, NO_ACTIVATION, 0, 1, false, false);
  gemmini_preload_zeros(C_addr);
  gemmini_compute_preloaded(A_addr, B_addr);
  gemmini_fence();

  gemmini_mvout(C, C_addr);
  gemmini_fence();

  for (size_t i = 0; i < DIM; i++) {
    printf("ROW %lu:", (unsigned long)i);
    for (size_t j = 0; j < DIM; j++) printf(" %.1f", b2f(C[i][j]));
    printf("\n");
  }
  fflush(stdout);
  size_t nz = 0;
  for (size_t i = 0; i < DIM; i++) for (size_t j = 0; j < DIM; j++) if (C[i][j] != 0) nz++;
  printf("nonzero = %lu / %d\n", (unsigned long)nz, DIM * DIM);

  // 대조: 스크래치패드에 넣은 A, B가 실제로 살아 있는지 되읽어 확인
  memset(C, 0, sizeof(C));
  gemmini_mvout(C, B_addr);
  gemmini_fence();
  printf("스크래치패드 B 되읽기 = %.3f %.3f (5.0이어야 함)\n", b2f(C[0][0]), b2f(C[0][1]));
  // ── 케이스 2: C를 표식(9.0)으로 채운 뒤 mvout ──
  // DMA가 그 자리를 안 쓰면 9.0이 남고, 0을 쓰면 0.0이 된다.
  printf("--- 표식 9.0 선충전 후 mvout ---\n"); fflush(stdout);
  for (size_t i = 0; i < DIM; i++) for (size_t j = 0; j < DIM; j++) C[i][j] = f2b(9.0f);
  gemmini_preload_zeros(C_addr);
  gemmini_compute_preloaded(A_addr, B_addr);
  gemmini_fence();
  gemmini_mvout(C, C_addr);
  gemmini_fence();
  size_t nz2 = 0;
  for (size_t i = 0; i < DIM; i++) {
    printf("F%lu:", (unsigned long)i);
    for (size_t j = 0; j < DIM; j++) { printf(" %.1f", b2f(C[i][j])); if (C[i][j]) nz2++; }
    printf("\n");
  }
  printf("표식 테스트: 9.0이 남으면 미기록, 0.0이면 0을 기록한 것\n");
  printf("=== RAWMM_DONE ===\n"); fflush(stdout);
  return 0;
}
