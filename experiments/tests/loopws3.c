// loopws3.c — 루프 FSM에서 가속기 1/2/3개는 어떻게 스케일하는가 (E177).
//
// E168은 손수 발행(타일당 명령 6개)에서 세 번째 가속기의 한계 이득이 급락한다고 쟀다:
// 두 개 1.93배, 세 개 2.21배 — 두 번째가 +0.93, 세 번째가 +0.28.
// 원인은 코어의 발행 대역폭으로 지목됐고, E173이 그것을 정량화했다(타일 연산당 여유 2.6 cycle).
//
// E176에서 loop_ws는 발행 비용을 matmul당으로 바꿨다(명령 6개로 타일 512개).
// 그렇다면 세 번째 가속기도 살아나야 한다. 그것을 확인한다.
//
// 바깥 타일링: 누산기가 I*J <= ACC_ROWS/dim = 64 타일이므로 I,J를 8타일씩 끊는다.
// 블록마다 loop_ws 하나(명령 6개)이고, 가속기들 사이에서 **블록 단위로 번갈아** 발행해
// 양쪽이 일찍 시작하게 한다.
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

// 3i8f50: 한 코어에 INT8 16x16 세 개 (custom3, custom2, custom1)
static const grt_ctx AC[3] = {
  { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_INT8 },
  { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_FP32 },
  { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_C1   },
};

#define MAXN 192
#define FREQ 50.0e6
#define PEAK (16.0*16.0*FREQ)
#define D 16
#define BLK 8                      // I,J를 8타일씩 → I*J = 64 = 누산기 한계

// 192³ x 3세트 = 324 KB (+REF 36 KB) — 512 KB L2 아래로 유지한다.
// E175에서 448 KB가 문턱에 걸터앉아 회차마다 값이 뒤집힌 전례가 있다.
static int8_t A[3][MAXN*MAXN], B[3][MAXN*MAXN], C[3][MAXN*MAXN] __attribute__((aligned(64)));
static int8_t REF[MAXN*MAXN];
static int SZ, NACC;

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }

// 한 가속기의 블록 순회 상태
typedef struct { int i0, j0, done; } blk_cur;

static void blk_step(int a, blk_cur *s){
  if (s->done) return;
  int T = SZ / D;
  int I = (s->i0 + BLK <= T) ? BLK : (T - s->i0);
  int J = (s->j0 + BLK <= T) ? BLK : (T - s->j0);
  grt_loop_ws(&AC[a], I, J, T,
              A[a] + (size_t)s->i0 * D * SZ,
              B[a] + (size_t)s->j0 * D,
              C[a] + (size_t)s->i0 * D * SZ + (size_t)s->j0 * D,
              SZ, SZ, SZ);
  s->j0 += BLK;
  if (s->j0 >= T) { s->j0 = 0; s->i0 += BLK; if (s->i0 >= T) s->done = 1; }
}

static void cfg_all(void){
  for (int a=0; a<NACC; a++)
    grt_loop_ws_config(&AC[a], (uint64_t)SZ, (uint64_t)SZ, (uint64_t)SZ);
}
static void run_seq(void){          // 가속기들을 차례로 (겹침 없음)
  cfg_all();
  for (int a=0; a<NACC; a++){
    blk_cur s = {0,0,0};
    while (!s.done) blk_step(a,&s);
    grt_fence();
  }
}
static void run_con(void){          // 블록 단위로 번갈아 발행하고 마지막에 한 번만 fence
  cfg_all();
  blk_cur s[3] = {{0,0,0},{0,0,0},{0,0,0}};
  int left = NACC;
  while (left > 0){
    left = 0;
    for (int a=0; a<NACC; a++){ blk_step(a,&s[a]); if(!s[a].done) left++; }
  }
  grt_fence();
}

static void fill(void){
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++){
    int8_t a = (c==r) || (c==(r+1)%SZ);
    int8_t b = (int8_t)((r*3+c*5)%7-3);
    for(int k=0;k<3;k++){ A[k][r*SZ+c]=a; B[k][r*SZ+c]=b; }
  }
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++)
    REF[r*SZ+c] = (int8_t)(B[0][r*SZ+c] + B[0][((r+1)%SZ)*SZ+c]);
}
static int chk(void){ int b=0;
  for(int a=0;a<NACC;a++) for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++)
    if(C[a][r*SZ+c]!=REF[r*SZ+c]) b++;
  return b; }
static double timeit(void (*fn)(void), int *bad){
  double best=1e30; *bad=0;
  for(int s=0;s<5;s++){ for(int a=0;a<3;a++) memset(C[a],0,(size_t)SZ*SZ);
    double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt; *bad+=chk(); }
  return best;
}

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/loopws3.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);

  mark("=== 루프 FSM으로 가속기 1/2/3개 (3i8f50, 50MHz, best-of-5) ===");
  mark("대조: 손수 발행에서는 두 개 1.93배, 세 개 2.21배로 포화했다(E168)");
  mark("%5s %4s | %9s %9s | %7s | %7s | %s",
       "크기","개수","순차ms","동시ms","이득","활용%","정확성");

  int Ns[]={64,96,128,192};
  for(unsigned i=0;i<sizeof(Ns)/sizeof(Ns[0]);i++){
    SZ=Ns[i]; fill();
    for(NACC=1; NACC<=3; NACC++){
      run_seq(); run_con(); run_seq(); run_con(); run_seq(); run_con();  /* 웜업 3회 */
      int b1,b2;
      double ts=timeit(run_seq,&b1);
      double tc=timeit(run_con,&b2);
      // 활용률은 동시 실행 기준: NACC개 matmul의 총 MAC / (시간 x NACC개 피크)
      double util = 100.0*((double)NACC*SZ*SZ*SZ/tc)/(PEAK*NACC);
      mark("%4d³ %4d | %9.3f %9.3f | %6.2fx | %6.1f%% | %s",
           SZ, NACC, ts*1e3, tc*1e3, ts/tc, util, (b1||b2)?"FAIL":"PASS");
      if(b1||b2) mark("     불일치: 순차=%d 동시=%d", b1,b2);
    }
  }
  mark("=== LOOPWS3_DONE ===");
  return 0;
}
