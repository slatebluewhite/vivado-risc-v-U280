// mb192.c — 192³에서 결과가 틀리는 것이 내 사용법인가 하드웨어인가 (E178).
//
// E177: 가속기당 loop_ws를 **여러 번** 발행하는 크기(192³, 2x2 블록)만 FAIL이고,
// 한 번만 발행하는 크기(64/96/128³)는 전부 PASS. 불일치 개수가 회차마다 달라
// 결정론적 주소 버그가 아니라 경합으로 보인다.
//
// 가르는 실험 네 개를 **같은 바이너리·같은 데이터**로 돌린다:
//   A. 스톡 tiled_matmul_auto (라이브러리 자신의 타일링) — 라이브러리가 맞는가?
//   B. 내 grt_loop_ws 다중 블록, fence 없음        — E177 재현
//   C. 내 grt_loop_ws 다중 블록, 블록마다 fence     — WAR 가설 검증
//   D. 내 grt_loop_ws 단일 블록(128³)              — 대조군(맞아야 함)
//
// A가 맞고 C가 맞으면 내 사용법 문제(동기화 누락). A가 틀리면 이 고정 커밋의 하드웨어 문제.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <sys/mman.h>
#include "include/gemmini.h"          // 스톡 (custom3 고정)
#include "include/gemmini_rt.h"       // 런타임 판

static const grt_ctx AC1 = { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_INT8 };

#define MAXN 192
#define BLK 8
static elem_t A[MAXN*MAXN], B[MAXN*MAXN], C[MAXN*MAXN] __attribute__((aligned(64)));
static elem_t REF[MAXN*MAXN];
static int SZ;

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }

// --- A: 스톡 라이브러리 -----------------------------------------------------
static void run_stock(void){
  tiled_matmul_auto(SZ, SZ, SZ, A, B, NULL, C,
                    SZ, SZ, SZ, SZ,
                    MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY,
                    NO_ACTIVATION, ACC_SCALE_IDENTITY, 0, false,
                    false, false, false, false, 3, WS);
}

// --- B/C: 내 loop_ws 블록 순회 ----------------------------------------------
static void run_mine(int fence_each){
  int T = SZ / 16;
  grt_loop_ws_config(&AC1, (uint64_t)SZ, (uint64_t)SZ, (uint64_t)SZ);
  for (int i0 = 0; i0 < T; i0 += BLK)
    for (int j0 = 0; j0 < T; j0 += BLK) {
      int I = (i0 + BLK <= T) ? BLK : (T - i0);
      int J = (j0 + BLK <= T) ? BLK : (T - j0);
      grt_loop_ws(&AC1, I, J, T,
                  A + (size_t)i0 * 16 * SZ,
                  B + (size_t)j0 * 16,
                  C + (size_t)i0 * 16 * SZ + (size_t)j0 * 16,
                  SZ, SZ, SZ);
      if (fence_each) grt_fence();
    }
  grt_fence();
}
static void run_nofence(void){ run_mine(0); }
static void run_fence(void){ run_mine(1); }

static void fill(void){
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++){
    A[r*SZ+c] = (c==r) || (c==(r+1)%SZ);
    B[r*SZ+c] = (elem_t)((r*3+c*5)%7-3);
  }
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++)
    REF[r*SZ+c] = (elem_t)(B[r*SZ+c] + B[((r+1)%SZ)*SZ+c]);
}
static int chk(void){ int b=0;
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++) if(C[r*SZ+c]!=REF[r*SZ+c]) b++; return b; }

// 회차마다 불일치 수가 달라지는지 보려면 **회차별로** 세야 한다 (합산은 정보를 지운다).
static void trial(const char *name, void (*fn)(void)){
  double best=1e30; int bad[5];
  for(int s=0;s<5;s++){ memset(C,0,(size_t)SZ*SZ);
    double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt; bad[s]=chk(); }
  int worst=0; for(int s=0;s<5;s++) if(bad[s]>worst) worst=bad[s];
  mark("%4d³ %-22s %8.3f ms  불일치/회차 = %d %d %d %d %d  %s",
       SZ, name, best*1e3, bad[0],bad[1],bad[2],bad[3],bad[4],
       worst? "FAIL":"PASS");
}

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/mb192.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  gemmini_flush(0);
  grt_flush_ctx(&AC1);

  mark("=== 192³ 다중 블록 오류: 내 사용법인가 하드웨어인가 (3i8f50, best-of-5) ===");
  mark("블록 수: 128³ = 1개(8x8 타일), 192³ = 4개(12타일을 8+4로 끊음)");

  SZ = 128; fill();
  trial("내 loop_ws(단일블록)", run_nofence);
  trial("스톡 tiled_matmul_auto", run_stock);

  SZ = 192; fill();
  trial("스톡 tiled_matmul_auto", run_stock);
  trial("내 loop_ws(fence 없음)", run_nofence);
  trial("내 loop_ws(블록마다 fence)", run_fence);

  mark("=== MB192_DONE ===");
  return 0;
}
