// het_c1.c — 코어1의 FP32 Gemmini가 왜 첫 RoCC 명령에서 멎는지 가른다 (E126).
//
// E125에서 확인된 사실: 코어0(INT8)은 flush~mvout 전 시퀀스를 통과하고,
// 코어1로 옮긴 직후 flush에서 멎는다. 다만 그때의 "코어1"은 내가 붙인 라벨이었을 뿐
// 실제 CPU 번호를 확인하지 않았다. 그래서 두 가능성이 아직 갈리지 않는다:
//   (A) 정말 코어1에 있고, 코어1의 FP32 Gemmini가 응답하지 않는다
//   (B) affinity가 안 먹어 코어0에 있고, 직전 코어0 작업이 남긴 상태가 문제다
//
// 이 프로그램은 **코어1을 먼저** 건드린다(코어0 작업의 잔재를 배제) 그리고
// 매 단계에서 sched_getcpu()를 실측해 기록한다.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <stdarg.h>
#include <sys/mman.h>
#include "include/gemmini_rt.h"

static FILE *g_log;
static void mark(const char *fmt, ...) {
  if (!g_log) return;
  va_list ap; va_start(ap, fmt);
  vfprintf(g_log, fmt, ap); va_end(ap);
  fprintf(g_log, "  (cpu=%d)\n", sched_getcpu());
  fflush(g_log); fsync(fileno(g_log));
}

static const grt_ctx C1 = { .dim = 8, .elem_bytes = 4, .acc_bytes = 4 };
static const grt_ctx C0 = { .dim = 16, .elem_bytes = 1, .acc_bytes = 4 };

static uint8_t Ab[16*16*4] __attribute__((aligned(64)));
static uint8_t Bb[16*16*4] __attribute__((aligned(64)));
static uint8_t Cb[16*16*4] __attribute__((aligned(64)));

static int pin(int cpu) {
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
  int r = sched_setaffinity(0, sizeof(s), &s);
  sched_yield();
  return r;
}

int main(void) {
  g_log = fopen("/mnt2/tmp/het_c1.log", "w");
  if (!g_log) { perror("로그 열기"); exit(1); }
  mark("시작");
  if (mlockall(MCL_CURRENT|MCL_FUTURE) != 0) { perror("mlockall"); exit(1); }
  mark("mlockall 후");

  // --- 코어1을 가장 먼저, 아무 잔재 없이 ---
  int r = pin(1);
  mark("코어1로 affinity 설정 (반환=%d)", r);

  mark("코어1: flush 직전");
  grt_flush();
  mark("코어1: flush 통과 ★");

  mark("코어1: config_ex 직전");
  grt_config_ex(&C1, GRT_WS);
  mark("코어1: config_ex 통과");

  mark("코어1: config_ld 직전");
  grt_config_ld(&C1, (uint64_t)C1.dim * C1.elem_bytes);
  mark("코어1: config_ld 통과");

  for (int i = 0; i < C1.dim*C1.dim; i++) ((float*)Ab)[i] = 1.0f;
  mark("코어1: mvin 직전");
  grt_mvin(&C1, Ab, 0, C1.dim, C1.dim);
  grt_fence();
  mark("코어1: mvin 통과");

  mark("코어1: config_st 직전");
  grt_config_st(&C1, (uint64_t)C1.dim * C1.elem_bytes);
  mark("코어1: config_st 통과");

  memset(Cb, 0, sizeof(Cb));
  mark("코어1: mvout(스크래치패드→DRAM) 직전");
  grt_mvout(&C1, Cb, 0, C1.dim, C1.dim);
  grt_fence();
  mark("코어1: mvout 통과, C[0]=%.3f (기대 1.000)", (double)((float*)Cb)[0]);

  // --- 그다음 코어0 (대조군) ---
  r = pin(0);
  mark("코어0으로 affinity 설정 (반환=%d)", r);
  mark("코어0: flush 직전");
  grt_flush();
  mark("코어0: flush 통과");

  mark("=== 전부 완료 ===");
  return 0;
}
