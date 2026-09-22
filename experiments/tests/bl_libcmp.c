// bl_libcmp.c — 내 런타임 라이브러리 경로와 Gemmini 공식 라이브러리 경로를
//               **같은 바이너리·같은 실행·같은 형상**으로 비교한다 (E137).
//
// 왜 필요한가:
//   E136에서 INT8 0.260 ms(활용률 12.6%), FP32 1.770 ms(7.4%)가 나왔다.
//   비교(6.80배)는 통제가 잘 됐지만 **절대값이 낮다**. 내 `grt_matmul`이
//   타일마다 A·B를 새로 mvin하고 config_ld를 두 번씩 내는 단순 구현이기 때문이다.
//   그 손해가 정확히 얼마인지 재지 않으면 E136의 숫자를 어떻게 읽어야 할지 알 수 없다.
//
//   공식 `tiled_matmul_auto`는 B를 재사용하고 루프 FSM으로 명령 발행을 줄인다.
//   두 경로를 같은 실행 안에서 재면 "내 구현의 비용"만 분리된다.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <sys/mman.h>
#include "include/gemmini_testutils.h"   // 공식 경로 (INT8, DIM=16, custom3)
#include "include/gemmini_rt.h"          // 내 런타임 경로

static const grt_ctx INT8 = { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_INT8 };

#define SZ 64
static elem_t Am[SZ][SZ] row_align(1);
static elem_t Bm[SZ][SZ] row_align(1);
static elem_t Cm[SZ][SZ] row_align(1);

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec + t.tv_nsec*1e-9; }

static void fill(void){
  for (int i=0;i<SZ;i++) for(int j=0;j<SZ;j++){
    Am[i][j] = (i==j)?1:0;                       // 단위행렬
    Bm[i][j] = (elem_t)((i+j)%5) - 2;
  }
  memset(Cm,0,sizeof(Cm));
}
static int verify(void){
  int bad=0;
  for (int i=0;i<SZ;i++) for(int j=0;j<SZ;j++) if (Cm[i][j]!=Bm[i][j]) bad++;
  return bad;
}

static void run_lib(void){
  tiled_matmul_auto(SZ, SZ, SZ, (elem_t*)Am, (elem_t*)Bm, NULL, (elem_t*)Cm,
      SZ, SZ, SZ, SZ, MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY,
      NO_ACTIVATION, ACC_SCALE_IDENTITY, 0, false, false, false, false, false, 0, WS);
}
static void run_rt(void){ grt_matmul(&INT8, Am, Bm, Cm, SZ, SZ, SZ); }

static double bench(void (*fn)(void), const char *tag){
  fill(); gemmini_flush(0); fn();
  int bad = verify();
  mark("%-8s 정확성 %s (%d/%d)", tag, bad?"FAIL":"PASS", bad, SZ*SZ);
  if (bad) return -1;
  double best=1e30;
  for (int s=0;s<3;s++){
    double t0=now();
    for (int i=0;i<10;i++) fn();
    double dt=(now()-t0)/10.0;
    if (dt<best) best=dt;
  }
  double macs=(double)SZ*SZ*SZ;
  mark("%-8s %.3f ms   %.1f MMAC/s   활용률 %.1f%%",
       tag, best*1e3, macs/best/1e6, macs/best/(256.0*31.25e6)*100.0);
  return best;
}

int main(int argc, char **argv){
  g = fopen(argc>1?argv[1]:"/mnt2/tmp/bl_libcmp.log","w");
  if(!g){ perror("로그"); exit(1); }
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();

  mark("=== 같은 바이너리·같은 실행: 공식 라이브러리 vs 내 런타임 구현 (INT8, %d^3) ===", SZ);
  mark("(cpu=%d, DIM=%d, 이론 최대 8000 MMAC/s @31.25MHz)", sched_getcpu(), DIM);
  double lib = bench(run_lib, "라이브러리");
  double rt  = bench(run_rt,  "런타임판");
  if (lib>0 && rt>0) mark("→ 내 구현이 %.2f배 느리다", rt/lib);
  mark("=== LIBCMP_DONE ===");
  return 0;
}
