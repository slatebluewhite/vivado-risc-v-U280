// bf16_scale.c — Float 곱셈 자체가 동작하는지 확인한다 (E97 후속)
//
// E97: mesh 곱셈 결과가 0이고, 누산기·DMA는 정상.
// 남은 후보는 (a) Float MAC 자체가 0을 냄 (b) mesh 데이터패스(가중치 적재 등).
//
// mvin 스케일은 **같은 Float 곱셈기**를 쓰지만 mesh를 거치지 않는다.
// 스케일이 먹으면 Float 곱셈은 정상 → 문제는 mesh 쪽으로 좁혀진다.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif
#include "include/gemmini_testutils.h"

static elem_t In[DIM][DIM] row_align(1);
static elem_t Out[DIM][DIM] row_align(1);

static inline float bf16_to_f32(uint16_t b){union{uint32_t u;float f;}c;c.u=((uint32_t)b)<<16;return c.f;}
static inline uint16_t f32_to_bf16(float f){union{uint32_t u;float f;}c;c.f=f;uint32_t l=(c.u>>16)&1u,b=0x7fffu+l;return (uint16_t)((c.u+b)>>16);}

static void roundtrip(float scale, const char *tag) {
  memset(Out, 0, sizeof(Out));
  gemmini_extended_config_ld(DIM * sizeof(elem_t), scale);
  gemmini_config_st(DIM * sizeof(elem_t));
  gemmini_mvin(In, 0);
  gemmini_fence();
  gemmini_mvout(Out, 0);
  gemmini_fence();
  printf("%-22s in=%.2f  out=%.2f %.2f %.2f\n", tag, bf16_to_f32(In[0][0]),
         bf16_to_f32(Out[0][0]), bf16_to_f32(Out[0][1]), bf16_to_f32(Out[1][0]));
  fflush(stdout);
}

int main() {
#ifndef BAREMETAL
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) { perror("mlockall failed"); exit(1); }
#endif
  gemmini_flush(0);
  for (size_t i = 0; i < DIM; i++)
    for (size_t j = 0; j < DIM; j++) In[i][j] = f32_to_bf16(3.0f);

  printf("=== BF16 SCALE TEST (DIM=%d) ===\n", DIM);
  printf("입력 3.0. 스케일이 먹으면 out = 3.0 * scale 이어야 한다.\n");
  fflush(stdout);
  roundtrip(1.0f, "scale=1.0 기대 3.0");
  roundtrip(2.0f, "scale=2.0 기대 6.0");
  roundtrip(0.5f, "scale=0.5 기대 1.5");
  printf("=== SCALE_DONE ===\n");
  fflush(stdout);
  return 0;
}
