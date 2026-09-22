// loopws.c — 루프 FSM이 발행 병목을 없애면 단일 코어 다중 가속기가 되살아나는가 (E176).
//
// E175의 결론은 "이 기법의 가치는 인터페이스가 비효율일수록 크다"였다:
// 타일당 명령 6개면 1.88배, 3개면 1.35배. 가속기를 잘 쓸수록 줄 것이 없어진다.
//
// 그 논리에는 빠져나갈 구멍이 있다. 병목은 "타일당 명령 수"가 아니라
// **발행 총량**이고, loop_ws는 명령 6개로 I×J×K 타일 전체를 돌린다.
// 128³이면 512 타일 연산이 명령 6개다 — 발행 비용이 matmul당이 된다.
// 그러면 한 코어가 두 가속기를 거의 공짜로 먹일 수 있어야 한다.
//
// 재는 것:
//   단독      loop_ws 하나 (활용률 — 라이브러리 수준인지 확인)
//   순차      가속기1 완주 후 가속기2
//   동시      둘 다 발행하고 마지막에 한 번만 fence
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <sys/mman.h>
#include "include/gemmini_rt.h"

static const grt_ctx AC1 = { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_INT8 };
static const grt_ctx AC2 = { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_FP32 };

#define MAXN 128
#define FREQ 50.0e6
#define PEAK (16.0*16.0*FREQ)
#define D 16
// ACC_ROWS=1024, dim=16 → 누산기 타일 64개 → I*J <= 64.
// 128³은 I=J=K=8이라 I*J=64로 딱 맞는다. 그래서 여기서는 128³까지만 잰다.
static int8_t A1[MAXN*MAXN], B1[MAXN*MAXN], C1[MAXN*MAXN] __attribute__((aligned(64)));
static int8_t A2[MAXN*MAXN], B2[MAXN*MAXN], C2[MAXN*MAXN] __attribute__((aligned(64)));
static int8_t REF[MAXN*MAXN];
static int SZ;

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }

static void cfg(const grt_ctx *c){
  grt_loop_ws_config(c, (uint64_t)SZ, (uint64_t)SZ, (uint64_t)SZ);  // int8이라 바이트=원소
}
static void issue(const grt_ctx *c, const int8_t *A, const int8_t *B, int8_t *C){
  int T = SZ / D;
  grt_loop_ws(c, T, T, T, A, B, C, SZ, SZ, SZ);
}

static void one(void){ cfg(&AC1); issue(&AC1,A1,B1,C1); grt_fence(); }
static void seq(void){
  cfg(&AC1); issue(&AC1,A1,B1,C1); grt_fence();
  cfg(&AC2); issue(&AC2,A2,B2,C2); grt_fence();
}
static void con(void){                    // 둘 다 발행하고 마지막에 한 번만 fence
  cfg(&AC1); cfg(&AC2);
  issue(&AC1,A1,B1,C1);
  issue(&AC2,A2,B2,C2);
  grt_fence();
}

static void fill(void){
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++){
    int8_t a = (c==r) || (c==(r+1)%SZ);
    int8_t b = (int8_t)((r*3+c*5)%7-3);
    A1[r*SZ+c]=a; B1[r*SZ+c]=b; A2[r*SZ+c]=a; B2[r*SZ+c]=b;
  }
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++)
    REF[r*SZ+c] = (int8_t)(B1[r*SZ+c] + B1[((r+1)%SZ)*SZ+c]);
}
static int chk(int both){ int b=0;
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++){
    if(C1[r*SZ+c]!=REF[r*SZ+c]) b++;
    if(both && C2[r*SZ+c]!=REF[r*SZ+c]) b++;
  } return b; }
static double timeit(void (*fn)(void), int both, int *bad){
  double best=1e30; *bad=0;
  for(int s=0;s<5;s++){ memset(C1,0,(size_t)SZ*SZ); memset(C2,0,(size_t)SZ*SZ);
    double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt; *bad+=chk(both); }
  return best;
}

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/loopws.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  grt_flush_ctx(&AC1); grt_flush_ctx(&AC2);

  mark("=== 루프 FSM(loop_ws)으로 단일 코어 두 가속기 (2i8f50, 50MHz, best-of-5) ===");
  mark("대조: 손수 발행 시 타일당 명령 6개→1.88배, 3개→1.35배 (128³)");
  mark("loop_ws는 명령 6개로 타일 %d개를 돌린다", (128/D)*(128/D)*(128/D));
  mark("%5s | %8s %7s | %8s %8s | %7s | %s",
       "크기","단독ms","활용%","순차ms","동시ms","속도","정확성");

  int Ns[]={64,96,128};
  for(unsigned i=0;i<sizeof(Ns)/sizeof(Ns[0]);i++){
    SZ=Ns[i]; fill();
    one(); seq(); con(); one(); seq(); con(); one(); seq(); con();   /* 웜업 3회 */
    int b0,b1,b2;
    double t1=timeit(one,0,&b0);
    double ts_=timeit(seq,1,&b1);
    double tc=timeit(con,1,&b2);
    mark("%4d³ | %8.3f %6.1f%% | %8.3f %8.3f | %6.2fx | %s",
         SZ, t1*1e3, 100.0*((double)SZ*SZ*SZ/t1)/PEAK, ts_*1e3, tc*1e3, ts_/tc,
         (b0||b1||b2)?"FAIL":"PASS");
    if(b0||b1||b2) mark("     불일치: 단독=%d 순차=%d 동시=%d", b0,b1,b2);
  }
  mark("=== LOOPWS_DONE ===");
  return 0;
}
