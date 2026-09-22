// rocc_core.c — "코어1에서 RoCC가 동작하는가"만 본다. 구성 무관, 어떤 비트스트림에서도 실행 가능.
//
// 왜: E126에서 이종 구성의 코어1이 첫 RoCC 명령(flush)에서 멎었다. 그런데 이게
//     (A) 이종 구성 고유의 문제인지, (B) 이 프로젝트에서 타일1의 RoCC가 원래 안 되는지
//     구분되지 않는다. (B)라면 지금까지의 모든 측정이 코어0 한정이라는 뜻이 된다.
//     이 프로그램을 알려진-정상 INT8 비트스트림(d9-FINAL)에서 돌리면 바로 갈린다.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mman.h>
#include "include/gemmini_rt.h"

static FILE *g;
static void mark(const char *fmt, ...) {
  if (!g) return;
  va_list ap; va_start(ap, fmt); vfprintf(g, fmt, ap); va_end(ap);
  fprintf(g, "  (cpu=%d)\n", sched_getcpu());
  fflush(g); fsync(fileno(g));
}
static int pin(int c){ cpu_set_t s; CPU_ZERO(&s); CPU_SET(c,&s);
  int r = sched_setaffinity(0,sizeof(s),&s); sched_yield(); return r; }

// opcode를 인자로 받는다: 두 번째 인자가 2면 custom2, 아니면 custom3.
// big.LITTLE 구성처럼 한 코어에 가속기가 둘 있을 때 각각을 따로 찔러볼 수 있다.
int main(int argc, char **argv){
  const char *path = argc > 1 ? argv[1] : "/mnt2/tmp/rocc_core.log";
  grt_ctx ctx = { .dim = 16, .elem_bytes = 1, .acc_bytes = 4,
                  .opcode = (argc > 2 && argv[2][0] == '2') ? GRT_OP_FP32 : GRT_OP_INT8 };
  g = fopen(path, "w"); if(!g){ perror("로그"); exit(1); }
  mark("시작 — opcode=custom%d", ctx.opcode);
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0){ perror("mlockall"); exit(1); }

  long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
  mark("온라인 CPU 수 = %ld", ncpu);

  // 코어1을 먼저 — 코어0 잔재 배제
  for (int cpu = (int)ncpu - 1; cpu >= 0; cpu--) {
    int r = pin(cpu);
    mark("cpu%d로 affinity (반환=%d)", cpu, r);
    mark("cpu%d: flush 직전", cpu);
    grt_flush_ctx(&ctx);
    mark("cpu%d: flush 통과 ★", cpu);
  }
  mark("=== 모든 코어에서 RoCC 응답 확인 ===");
  return 0;
}
