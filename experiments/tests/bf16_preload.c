// bf16_preload.c — 라이브러리 방식(B를 preload로 적재)이 BF16에서 되는지 확인한다 (E104 후속)
//
// 내 원시 테스트:   preload_zeros(C) + compute_preloaded(A, B)      → 98% 정확
// 라이브러리 방식:  extended_preload(B, C) + compute_preloaded(A, GARBAGE) → ?
//
// 후자가 0이면 "가중치 preload 경로"가 BF16에서 깨진 것으로 확정된다.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif
#include "include/gemmini_testutils.h"
static elem_t A[DIM][DIM] row_align(1), B[DIM][DIM] row_align(1), C[DIM][DIM] row_align(1);
static inline float b2f(uint16_t b){union{uint32_t u;float f;}c;c.u=((uint32_t)b)<<16;return c.f;}
static inline uint16_t f2b(float f){union{uint32_t u;float f;}c;c.f=f;uint32_t l=(c.u>>16)&1u,x=0x7fffu+l;return (uint16_t)((c.u+x)>>16);}

int main(){
#ifndef BAREMETAL
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0){perror("mlockall");exit(1);}
#endif
  gemmini_flush(0);
  for (size_t i=0;i<DIM;i++) for (size_t j=0;j<DIM;j++){ A[i][j]=f2b(i==j?1.0f:0.0f); B[i][j]=f2b(5.0f); }
  const uint32_t Aa=0, Ba=DIM;
  const uint32_t acc = (uint32_t)1 << (ADDR_LEN-1);        // 누산기, 덮어쓰기
  gemmini_config_ld(DIM*sizeof(elem_t)); gemmini_config_st(DIM*sizeof(elem_t));
  gemmini_mvin(A, Aa); gemmini_mvin(B, Ba); gemmini_fence();
  gemmini_extended_config_ex(WEIGHT_STATIONARY, NO_ACTIVATION, 0, 1, false, false);
  printf("=== BF16 PRELOAD 방식 비교 (DIM=%d) 기대 5.0 ===\n", DIM); fflush(stdout);

  // 방식 A: 내 기존 방식
  for (size_t i=0;i<DIM;i++) for (size_t j=0;j<DIM;j++) C[i][j]=f2b(9.0f);
  gemmini_preload_zeros(acc);
  gemmini_compute_preloaded(Aa, Ba);
  gemmini_fence(); gemmini_mvout(C, acc); gemmini_fence();
  printf("방식A (compute에 B전달)  C[0][0..2] = %.1f %.1f %.1f\n", b2f(C[0][0]), b2f(C[0][1]), b2f(C[0][2]));
  fflush(stdout);

  // 방식 B: 라이브러리 방식 — B를 preload로 적재, compute 2번째 인자는 GARBAGE
  for (size_t i=0;i<DIM;i++) for (size_t j=0;j<DIM;j++) C[i][j]=f2b(9.0f);
  gemmini_extended_preload(Ba, acc, DIM, DIM, DIM, DIM);
  gemmini_extended_compute_preloaded(Aa, GARBAGE_ADDR, DIM, DIM, DIM, DIM);
  gemmini_fence(); gemmini_mvout(C, acc); gemmini_fence();
  printf("방식B (preload로 B적재)  C[0][0..2] = %.1f %.1f %.1f\n", b2f(C[0][0]), b2f(C[0][1]), b2f(C[0][2]));
  fflush(stdout);

  // 방식 C: 라이브러리 방식 + preload와 compute 사이에 fence
  for (size_t i=0;i<DIM;i++) for (size_t j=0;j<DIM;j++) C[i][j]=f2b(9.0f);
  gemmini_extended_preload(Ba, acc, DIM, DIM, DIM, DIM);
  gemmini_fence();                                    // ★ 사이에 fence
  gemmini_extended_compute_preloaded(Aa, GARBAGE_ADDR, DIM, DIM, DIM, DIM);
  gemmini_fence(); gemmini_mvout(C, acc); gemmini_fence();
  printf("방식C (preload+fence)    C[0][0..2] = %.1f %.1f %.1f\n", b2f(C[0][0]), b2f(C[0][1]), b2f(C[0][2]));
  fflush(stdout);

  // 방식 D: preload를 두 번 발행 (가중치를 확실히 적재)
  for (size_t i=0;i<DIM;i++) for (size_t j=0;j<DIM;j++) C[i][j]=f2b(9.0f);
  gemmini_extended_preload(Ba, acc, DIM, DIM, DIM, DIM);
  gemmini_extended_compute_preloaded(Aa, GARBAGE_ADDR, DIM, DIM, DIM, DIM);
  gemmini_extended_preload(Ba, acc, DIM, DIM, DIM, DIM);
  gemmini_extended_compute_preloaded(Aa, GARBAGE_ADDR, DIM, DIM, DIM, DIM);
  gemmini_fence(); gemmini_mvout(C, acc); gemmini_fence();
  printf("방식D (preload 2회)      C[0][0..2] = %.1f %.1f %.1f\n", b2f(C[0][0]), b2f(C[0][1]), b2f(C[0][2]));
  fflush(stdout);

  printf("=== PRELOAD_DONE ===\n"); fflush(stdout);
  return 0;
}
