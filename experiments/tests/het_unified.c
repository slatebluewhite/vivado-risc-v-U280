// het_unified.c — **하나의 바이너리**로 두 코어의 서로 다른 Gemmini를 모두 구동한다.
//
// Gemmini 기본 헤더를 쓰지 않고 gemmini_rt.h(런타임 파라미터)만 쓴다.
// 따라서 DIM·elem_t가 컴파일 타임에 고정되지 않는다.
//
// 검증: 각 코어에서 한 타일 크기의 A·B를 mvin → preload/compute → mvout 하고
//       CPU 계산과 비교한다. A는 단위행렬이므로 결과는 B와 같아야 한다.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sched.h>
#include <pthread.h>
#include "include/gemmini_rt.h"

// 두 코어의 가속기 사양 (하드웨어 구성에서 알고 있는 값)
static const grt_ctx CORE0 = { .dim = 16, .elem_bytes = 1, .acc_bytes = 4 };  // INT8 16x16
static const grt_ctx CORE1 = { .dim =  8, .elem_bytes = 4, .acc_bytes = 4 };  // FP32 8x8

// 정렬된 버퍼 (최대 16x16 x 4바이트)
static uint8_t Abuf[16*16*4] __attribute__((aligned(64)));
static uint8_t Bbuf[16*16*4] __attribute__((aligned(64)));
static uint8_t Cbuf[16*16*4] __attribute__((aligned(64)));

static void fill(const grt_ctx *c, int is_float) {
  int d = c->dim;
  for (int i = 0; i < d; i++)
    for (int j = 0; j < d; j++) {
      if (is_float) {
        ((float*)Abuf)[i*d+j] = (i==j) ? 1.0f : 0.0f;
        ((float*)Bbuf)[i*d+j] = 3.0f;
        ((float*)Cbuf)[i*d+j] = 9.0f;      // 표식
      } else {
        ((int8_t*)Abuf)[i*d+j] = (i==j) ? 1 : 0;
        ((int8_t*)Bbuf)[i*d+j] = 3;
        ((int8_t*)Cbuf)[i*d+j] = 9;
      }
    }
}

static int one_tile(const grt_ctx *c, int is_float, const char *tag) {
  int d = c->dim;
  uint32_t A_sp = 0, B_sp = d;
  uint32_t C_acc = GRT_ACC(0);            // 누산기 0번, 덮어쓰기 모드
  fill(c, is_float);

  grt_flush();
  grt_config_ld(c, (uint64_t)d * c->elem_bytes);
  grt_config_st(c, (uint64_t)d * c->elem_bytes);
  grt_config_ex(c, GRT_WS);

  grt_mvin(c, Abuf, A_sp, d, d);
  grt_mvin(c, Bbuf, B_sp, d, d);
  grt_fence();

  // WS 표준 흐름: B를 preload로 배열에 적재하고, compute에는 A만 준다.
  grt_preload(c, B_sp, C_acc, d, d, d, d);
  grt_compute(c, A_sp, GRT_GARBAGE, d, d, d, d);
  grt_fence();

  grt_mvout(c, Cbuf, C_acc, d, d);
  grt_fence();

  int bad = 0;
  for (int i = 0; i < d; i++)
    for (int j = 0; j < d; j++) {
      double v = is_float ? ((float*)Cbuf)[i*d+j] : (double)((int8_t*)Cbuf)[i*d+j];
      if (v != 3.0) bad++;
    }
  double first = is_float ? ((float*)Cbuf)[0] : (double)((int8_t*)Cbuf)[0];
  printf("%-8s cpu=%d dim=%d elem=%dB  C[0][0]=%.1f  오류 %d/%d  →  %s\n",
         tag, sched_getcpu(), d, c->elem_bytes, first, bad, d*d, bad?"FAIL":"PASS");
  fflush(stdout);
  return bad;
}

static void pin(int cpu) {
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
  sched_setaffinity(0, sizeof(s), &s);
  sched_yield();
}

int main(){
  if (mlockall(MCL_CURRENT|MCL_FUTURE) != 0) { perror("mlockall"); exit(1); }
  printf("=== 통합 바이너리: 한 프로그램이 두 가속기를 구동 ===\n"); fflush(stdout);

  pin(0);  int b0 = one_tile(&CORE0, 0, "코어0");   // INT8 16x16
  pin(1);  int b1 = one_tile(&CORE1, 1, "코어1");   // FP32 8x8
  pin(0);  int b2 = one_tile(&CORE0, 0, "코어0재");  // 되돌아와서 다시

  printf("UNIFIED %s\n", (b0||b1||b2) ? "FAIL" : "PASS");
  fflush(stdout);
  return (b0||b1||b2) ? 1 : 0;
}
