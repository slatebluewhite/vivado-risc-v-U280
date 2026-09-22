// het_split.c — 이종 빌드에서 코어1이 멎는 지점을 명령 종류별로 쪼갠다 (E127).
//
// 지금까지 확정된 것(E126):
//   - 같은 바이너리가 d9-FINAL(둘 다 INT8)과 gem8fp32(둘 다 FP32)에서는 cpu1 flush 통과
//   - 이종 빌드에서만 cpu1 flush 정지
//   - 가속기 RTL·타일 RTL은 정상 빌드와 완전히 동일, 타이밍도 깨끗
//
// 그래서 flush의 무엇이 문제인지를 쪼갠다. 순서를 일부러 이렇게 잡았다:
//   1) config_ex  — TLB도 DMA도 건드리지 않는 순수 설정
//   2) flush(skip=1) — TLB 플러시를 건너뛰는 flush
//   3) flush(skip=0) — TLB까지 비우는 flush  ← 지금 멎는 것
//   4) mvin        — TLB/DMA를 실제로 쓴다
// 어디서 멎느냐가 원인을 가른다.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <sched.h>
#include <string.h>
#include <sys/mman.h>
#include "include/gemmini_rt.h"

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"  (cpu=%d)\n", sched_getcpu()); fflush(g); fsync(fileno(g));
}
static int pin(int c){ cpu_set_t s; CPU_ZERO(&s); CPU_SET(c,&s);
  int r=sched_setaffinity(0,sizeof(s),&s); sched_yield(); return r; }

static const grt_ctx C1 = { .dim=8, .elem_bytes=4, .acc_bytes=4 };
static uint8_t Ab[8*8*4] __attribute__((aligned(64)));

int main(int argc, char **argv){
  g = fopen(argc>1?argv[1]:"/mnt2/tmp/het_split.log","w");
  if(!g){ perror("로그"); exit(1); }
  mark("시작");
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0){ perror("mlockall"); exit(1); }
  pin(1); mark("cpu1 고정");

  mark("1) config_ex 직전 (TLB·DMA 무관)");
  grt_config_ex(&C1, GRT_WS);
  mark("1) config_ex 통과 ★");

  mark("2) flush(skip=1) 직전 (TLB 플러시 건너뜀)");
  grt_flush_skip(1);
  mark("2) flush(skip=1) 통과 ★");

  mark("3) flush(skip=0) 직전 (TLB까지 비움)");
  grt_flush_skip(0);
  mark("3) flush(skip=0) 통과 ★");

  mark("4) config_ld 직전");
  grt_config_ld(&C1, (uint64_t)C1.dim*C1.elem_bytes);
  mark("4) config_ld 통과 ★");

  for (int i=0;i<C1.dim*C1.dim;i++) ((float*)Ab)[i] = 1.0f;
  mark("5) mvin 직전 (TLB/DMA 실사용)");
  grt_mvin(&C1, Ab, 0, C1.dim, C1.dim);
  grt_fence();
  mark("5) mvin 통과 ★");

  mark("=== 코어1 전부 통과 ===");
  return 0;
}
