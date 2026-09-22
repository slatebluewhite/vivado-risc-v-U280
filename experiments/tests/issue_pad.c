// issue_pad.c — 분할 경로가 정말 **코어의 명령 발행**에 닿아 있는가 (E173).
//
// E172의 주장: 단일 가속기 경로는 가속기를 기다리고(24 cycle/명령), 분할 경로는
// 코어의 발행 경로에 닿아 있다(13.5 cycle/명령). 명령 수 세기만으로 내린 결론이라
// 직접 친다.
//
// 방법: 발행 루프에 **쓸모없는 CPU 작업**을 n만큼 끼워 넣고 n을 늘린다.
//   - 가속기 바운드라면 코어는 어차피 놀고 있으므로 작은 n은 **공짜로 흡수**된다.
//   - 발행 바운드라면 n에 **즉시 비례해** 느려진다.
// 흡수 한계(시간이 오르기 시작하는 n)가 그 경로의 여유 발행 대역폭이다.
//
// 이 구분은 "온칩 버스 경합"과 "코어 발행"을 가른다 — 버스 경합이라면 CPU 작업을
// 넣어도 (한계 전까지는) 아무 변화가 없어야 한다.
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

#define SZ 256
#define FREQ 50.0e6
static int8_t A[SZ*SZ], B[SZ*SZ], C[SZ*SZ] __attribute__((aligned(64)));
static int8_t REF[SZ*SZ];

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }

// 의존 사슬이라 슈퍼스칼라로도 못 접힌다. volatile 싱크로 죽은 코드 제거를 막는다
// (E145에서 결과를 안 읽어 3us로 잘못 잰 전례가 있다).
// 1차 시도는 64비트 곱셈 사슬이라 반복당 약 13 cycle이었다 — 최소 눈금이
// 타일 연산(72 cycle)의 60%라 미세 구조가 안 보였다. 시프트/xor 사슬로 바꾼다
// (의존 사슬이라 접히지 않고, 반복당 약 3 cycle).
static long sink;
static inline void pad(int n){
  long x = sink;
  for (int i = 0; i < n; i++) x = (x << 1) ^ (x >> 63) ^ 1L;
  sink = x;
}

static int PAD;
static void single_pad(void){
  grt_work w = { .c=&AC1, .A=A, .B=B, .C=C, .M=SZ,.N=SZ,.K=SZ };
  grt_cursor s = {0,0,0,0,0};
  grt_config_ex(w.c, GRT_WS);
  while (!s.done) { grt_step_r_i8(&w, &s); pad(PAD); }
  grt_fence();
}
static void split_pad(void){
  const int half = SZ/2;
  grt_work w1 = { .c=&AC1, .A=A,                 .B=B, .C=C,                 .M=half,.N=SZ,.K=SZ };
  grt_work w2 = { .c=&AC2, .A=A+(size_t)half*SZ, .B=B, .C=C+(size_t)half*SZ, .M=half,.N=SZ,.K=SZ };
  grt_cursor s1 = {0,0,0,0,0}, s2 = {0,0,0,0,0};
  grt_config_ex(w1.c, GRT_WS);
  grt_config_ex(w2.c, GRT_WS);
  while (!s1.done || !s2.done) {
    grt_step_r_i8(&w1,&s1);  pad(PAD);
    grt_step_r_i8b(&w2,&s2); pad(PAD);
  }
  grt_fence();
}

static void fill(void){
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++){
    A[r*SZ+c] = (c==r) || (c==(r+1)%SZ);
    B[r*SZ+c] = (int8_t)((r*3+c*5)%7-3);
  }
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++)
    REF[r*SZ+c] = (int8_t)(B[r*SZ+c] + B[((r+1)%SZ)*SZ+c]);
}
static int chk(void){ int b=0;
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++) if(C[r*SZ+c]!=REF[r*SZ+c]) b++; return b; }
static double timeit(void (*fn)(void), int *bad){
  double best=1e30; *bad=0;
  for(int s=0;s<5;s++){ memset(C,0,(size_t)SZ*SZ);
    double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt; *bad+=chk(); }
  return best;
}

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/issue_pad.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  grt_flush_ctx(&AC1); grt_flush_ctx(&AC2);
  fill();

  // pad 자체의 비용을 먼저 잰다 (가속기 없이)
  mark("=== 발행 루프에 CPU 작업을 끼워 흡수 한계를 잰다 (%d³, cpu=%d, best-of-5) ===",
       SZ, sched_getcpu());
  {
    const int reps = 4096;   // 256³의 타일 연산 수와 같다
    for (int n = 1; n <= 64; n *= 2) {
      double t0=now(); for(int r=0;r<reps;r++) pad(n); double dt=now()-t0;
      mark("pad(%2d) 단독 %4d회: %8.3f ms  → %5.1f cycle/회", n, reps, dt*1e3,
           dt*FREQ/reps);
    }
  }
  mark("");
  mark("%5s | %9s %7s | %9s %7s | %6s | %s",
       "pad","단일ms","증가%","분할ms","증가%","이득","정확성");

  double s_base=0, p_base=0;
  int pads[]={0,1,2,3,4,6,8,12,16,24,32,48,64};
  for(unsigned i=0;i<sizeof(pads)/sizeof(pads[0]);i++){
    PAD=pads[i];
    single_pad(); split_pad();            /* 웜업 */
    int b1,b2;
    double ts=timeit(single_pad,&b1);
    double tp=timeit(split_pad,&b2);
    if(i==0){ s_base=ts; p_base=tp; }
    mark("%5d | %9.3f %6.1f%% | %9.3f %6.1f%% | %5.2fx | %s",
         PAD, ts*1e3, 100.0*(ts/s_base-1.0), tp*1e3, 100.0*(tp/p_base-1.0),
         ts/tp, (b1||b2)?"FAIL":"PASS");
  }
  mark("=== ISSUE_PAD_DONE ===");
  return 0;
}
