// bf16_diag.c — BF16에서 "연산 결과가 누산기에 도달하지 않는" 원인을 가른다 (E63 후속)
//
// 확인된 사실:
//   - mvin → 누산기 → 전폭 mvout : 정상 (E62)
//   - matmul → 누산기 → mvout    : 전부 0 (E63)
//
// 즉 누산기 자체와 읽기 경로는 살아 있고, 연산 결과만 도달하지 않는다.
// 가능성이 둘이다:
//   (A) mesh가 0을 낸다 (입력이 안 들어가거나 MAC이 0을 냄)
//   (B) mesh는 계산하는데 누산기에 써지지 않는다
//
// D(바이어스)를 이용해 가른다. D는 누산기로 직접 들어가고 mesh를 거치지 않는다.
//   케이스 1: A=0, B=0, D≠0  → C가 D면 "누산기 쓰기 경로는 살아 있다"
//   케이스 2: A≠0, B≠0, D=0  → C가 0이면 "mesh 결과가 안 온다"
//   케이스 3: A≠0, B≠0, D≠0  → C가 D면 mesh 성분만 유실, C가 0이면 전체 유실
//
// 세 결과의 조합이 (A)와 (B)를 구분한다.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif
#include "include/gemmini_testutils.h"

#define SZ 16

static elem_t A[SZ][SZ] row_align(1);
static elem_t B[SZ][SZ] row_align(1);
static acc_t  D[SZ][SZ] row_align_acc(1);
static elem_t C[SZ][SZ] row_align(1);

static inline float bf16_to_f32(uint16_t b) {
  union { uint32_t u; float f; } c; c.u = ((uint32_t)b) << 16; return c.f;
}
static inline uint16_t f32_to_bf16(float f) {
  union { uint32_t u; float f; } c; c.f = f;
  uint32_t lsb = (c.u >> 16) & 1u, bias = 0x7fffu + lsb;
  return (uint16_t)((c.u + bias) >> 16);
}

static void run(int use_D) {
  memset(C, 0, sizeof(C));
  tiled_matmul_auto(SZ, SZ, SZ, (elem_t*)A, (elem_t*)B,
      use_D ? (void*)D : NULL, (void*)C,
      SZ, SZ, SZ, SZ,
      MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY,
      NO_ACTIVATION, ACC_SCALE_IDENTITY, 0, false,
      false, false, /*full_C=*/false, /*low_D=*/false, 0, WS);
}

static void show(const char *tag) {
  printf("%-28s C[0][0..3] = %.3f %.3f %.3f %.3f\n", tag,
         bf16_to_f32(C[0][0]), bf16_to_f32(C[0][1]),
         bf16_to_f32(C[0][2]), bf16_to_f32(C[0][3]));
  size_t nz = 0;
  for (size_t i = 0; i < SZ; i++)
    for (size_t j = 0; j < SZ; j++) if (C[i][j] != 0) nz++;
  printf("%-28s nonzero = %lu / %d\n", "", (unsigned long)nz, SZ * SZ);
  fflush(stdout);
}

static void fill(int a_nonzero, int d_nonzero) {
  for (size_t i = 0; i < SZ; i++)
    for (size_t j = 0; j < SZ; j++) {
      A[i][j] = a_nonzero ? f32_to_bf16((i == j) ? 1.0f : 0.0f) : 0;   // 단위행렬
      B[i][j] = a_nonzero ? f32_to_bf16(2.0f) : 0;                     // 전부 2.0
      D[i][j] = d_nonzero ? 7.0f : 0.0f;
    }
}

int main() {
#ifndef BAREMETAL
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) { perror("mlockall failed"); exit(1); }
#endif
  gemmini_flush(0);
  printf("=== BF16 DIAG (SZ=%d, DIM=%d) ===\n", SZ, DIM);
  printf("A=단위행렬, B=2.0, D=7.0 → 기대: A*B=2.0, A*B+D=9.0\n");
  fflush(stdout);

  fill(0, 1); run(1); show("1) A=0,B=0,D=7  기대 7.0");
  fill(1, 0); run(0); show("2) A=I,B=2,D=없음 기대 2.0");
  fill(1, 1); run(1); show("3) A=I,B=2,D=7  기대 9.0");

  printf("=== DIAG_DONE ===\n");
  fflush(stdout);
  return 0;
}
