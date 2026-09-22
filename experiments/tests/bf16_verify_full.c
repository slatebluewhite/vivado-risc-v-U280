// bf16_verify_full.c — BF16 연산 정확성 자체 검증 (E52 후속)
//
// 왜 필요한가: 번들 테스트(matmul.c)는 BF16을 검증할 수 없다.
// BF16 헤더는 `typedef uint16_t elem_t`로 원시 비트 컨테이너를 쓰는데,
// CPU 참조는 `C += A[r][k]*B[k][c]`로 그 비트패턴을 정수 곱셈한다.
// 게다가 is_equal은 float 경로에서도 정확 일치를 요구한다.
// 따라서 참조값 자체가 무의미하고, 실패해도 하드웨어에 대해 아무것도 말해주지 않는다.
//
// 이 프로그램은 (1) BF16 <-> float 변환을 명시적으로 수행하고
// (2) double로 오라클을 계산한 뒤 (3) BF16 정밀도에 맞는 상대 오차로 비교한다.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif
#include "include/gemmini_testutils.h"

// BF16은 FP32의 상위 16비트다.
static inline float bf16_to_f32(uint16_t b) {
  union { uint32_t u; float f; } c;
  c.u = ((uint32_t)b) << 16;
  return c.f;
}

// round-to-nearest-even으로 FP32를 BF16으로 줄인다.
static inline uint16_t f32_to_bf16(float f) {
  union { uint32_t u; float f; } c;
  c.f = f;
  uint32_t lsb = (c.u >> 16) & 1u;
  uint32_t bias = 0x7fffu + lsb;
  return (uint16_t)((c.u + bias) >> 16);
}

#define N 64          // 8의 배수 (DIM=8)
#define TOL 0.04f     // BF16 가수 8비트 → 상대오차 2^-8 ≈ 0.0039.
                      // N=64 누적(FP32 누산)까지 감안해 여유 있게 잡되,
                      // 하드웨어가 근본적으로 틀리면 통과하지 못할 만큼은 좁게.

static elem_t A[N][N] row_align(1);
static elem_t B[N][N] row_align(1);
static acc_t  C[N][N] row_align_acc(1);   // ★ 전폭 출력: acc_t(FP32)로 받는다

int main() {
#ifndef BAREMETAL
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) { perror("mlockall failed"); exit(1); }
#endif
  gemmini_flush(0);

  printf("=== BF16 VERIFY FULL-WIDTH (DIM=%d, N=%d) ===\n", DIM, N);
  fflush(stdout);

  // BF16으로 정확히 표현되는 값만 쓴다(변환 왕복 오차를 입력에서 배제).
  for (size_t i = 0; i < N; i++)
    for (size_t j = 0; j < N; j++) {
      float a = (float)((int)((i * 7 + j * 3) % 17) - 8) * 0.25f;
      float b = (float)((int)((i * 5 + j * 11) % 13) - 6) * 0.5f;
      A[i][j] = f32_to_bf16(a);
      B[i][j] = f32_to_bf16(b);
      C[i][j] = 0.0f;
    }

  // 입력이 실제로 왕복 무손실인지 먼저 확인한다.
  // 여기서 깨지면 아래 비교는 하드웨어가 아니라 내 변환을 재는 셈이 된다.
  int convfail = 0;
  for (size_t i = 0; i < N && !convfail; i++)
    for (size_t j = 0; j < N; j++) {
      float a = (float)((int)((i * 7 + j * 3) % 17) - 8) * 0.25f;
      if (bf16_to_f32(A[i][j]) != a) { convfail = 1; break; }
    }
  printf("input roundtrip: %s\n", convfail ? "LOSSY (테스트 무효)" : "exact");
  fflush(stdout);

  tiled_matmul_auto(N, N, N,
      (elem_t*)A, (elem_t*)B, NULL, (elem_t*)C,
      N, N, N, N,
      MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY,
      NO_ACTIVATION, ACC_SCALE_IDENTITY, 0, false,
      false, false, /*full_C=*/true, false, 0, WS);

  // 오라클: double로 계산한다. 하드웨어는 FP32로 누산하므로
  // double 기준과의 차이는 BF16 반올림 + FP32 누산 오차뿐이어야 한다.
  size_t bad = 0;
  double worst = 0.0;
  size_t wi = 0, wj = 0;
  for (size_t i = 0; i < N; i++)
    for (size_t j = 0; j < N; j++) {
      double acc = 0.0;
      for (size_t k = 0; k < N; k++)
        acc += (double)bf16_to_f32(A[i][k]) * (double)bf16_to_f32(B[k][j]);
      double got = (double)C[i][j];
      double denom = fabs(acc) > 1.0 ? fabs(acc) : 1.0;
      double rel = fabs(got - acc) / denom;
      if (rel > worst) { worst = rel; wi = i; wj = j; }
      if (rel > TOL) bad++;
    }

  printf("mismatches: %lu / %lu\n", (unsigned long)bad, (unsigned long)(N * N));
  printf("worst rel err: %d.%03d%% at [%lu][%lu]\n",
         (int)(worst * 100), (int)(worst * 100000) % 1000,
         (unsigned long)wi, (unsigned long)wj);
  fflush(stdout);

  if (convfail) { printf("BF16_VERIFY_FULL INVALID\n"); return 2; }
  if (bad == 0) { printf("BF16_VERIFY_FULL PASS\n"); return 0; }
  printf("BF16_VERIFY_FULL FAIL\n");
  return 1;
}
