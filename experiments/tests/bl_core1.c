// bl_core1.c — big.LITTLE 구성에서 **가속기 없는 코어1**의 거동을 확인한다.
//
// 왜 중요한가:
//   타일1에는 RoCC가 아예 없으므로(cmdRouter 참조 0회) custom 명령은
//   **불법 명령(SIGILL)**로 깔끔히 죽어야 한다. 만약 여기서도 시스템이 멎는다면
//   big.LITTLE 구성의 이점(E127~E129의 버스 정지 우회)이 사라진다.
//
//   SIGILL은 프로세스를 죽이므로 자식을 fork해서 시험하고, 부모가 결과를 기록한다.
//   부모가 살아남아 기록을 남긴다는 사실 자체가 "시스템이 안 멎었다"는 증거다.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include "include/gemmini_rt.h"

static const grt_ctx INT8 = { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_INT8 };
static const grt_ctx FP32 = { .dim= 8, .elem_bytes=4, .acc_bytes=4, .opcode=GRT_OP_FP32 };

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static void pin(int c){ cpu_set_t s; CPU_ZERO(&s); CPU_SET(c,&s);
  sched_setaffinity(0,sizeof(s),&s); sched_yield(); }

// cpu에서 opcode를 한 번 찔러보고 결과를 부모가 판정한다
static void try_rocc(int cpu, const grt_ctx *c, const char *tag){
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) { pin(cpu); grt_flush_ctx(c); _exit(0); }   // 살아남으면 0
  int st = 0;
  for (int i = 0; i < 100; i++) {                            // 최대 10초 대기
    pid_t r = waitpid(pid, &st, WNOHANG);
    if (r == pid) {
      if (WIFSIGNALED(st))
        mark("%s → 시그널 %d (%s)%s", tag, WTERMSIG(st), strsignal(WTERMSIG(st)),
             WTERMSIG(st)==SIGILL ? "  ★ 기대대로: 가속기 없음, 깔끔히 죽음" : "");
      else
        mark("%s → 정상 종료 (코드 %d)  ★ 가속기가 응답함", tag, WEXITSTATUS(st));
      return;
    }
    usleep(100000);
  }
  mark("%s → **10초 무응답: 하트가 멎었다**", tag);
  kill(pid, SIGKILL);
}

int main(int argc, char **argv){
  g = fopen(argc>1?argv[1]:"/mnt2/tmp/bl_core1.log","w");
  if(!g){ perror("로그"); exit(1); }
  mark("=== 코어별·opcode별 거동 확인 ===");
  mark("설계 의도: 코어0은 두 opcode 모두 응답, 코어1은 둘 다 SIGILL");

  try_rocc(0, &INT8, "cpu0 custom3(INT8)");
  try_rocc(0, &FP32, "cpu0 custom2(FP32)");
  try_rocc(1, &INT8, "cpu1 custom3(INT8)");
  try_rocc(1, &FP32, "cpu1 custom2(FP32)");

  mark("=== 부모가 여기까지 왔다 = 시스템이 멎지 않았다 ===");
  return 0;
}
