// lib_mm.c — 공식 라이브러리 경로(tiled_matmul_auto)로 64^3 matmul을 잰다 (E138).
//
// 헤더를 바꿔 넣어 INT8판/FP32판 두 바이너리로 빌드한다.
//   INT8: 기본 include 경로 (gemmini_params.h, DIM=16, custom3)
//   FP32: gemmini_params_fp32.h를 gemmini_params.h 자리에 놓고 빌드 (DIM=8, custom2)
// 코드는 완전히 동일하므로 E120의 교훈("같은 코드로 재지 않으면 비교가 성립하지 않는다")을 지킨다.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <sys/mman.h>
#include "include/gemmini_testutils.h"

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

int main(int argc, char **argv){
  g = fopen(argc>1?argv[1]:"/mnt2/tmp/lib_mm.log","w");
  if(!g){ perror("로그"); exit(1); }
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  gemmini_flush(0);

  // A는 단위행렬, B는 작은 값 → C는 B와 같아야 한다 (정수·부동소수 모두 정확)
  for (int i=0;i<SZ;i++) for(int j=0;j<SZ;j++){
    Am[i][j] = (i==j) ? (elem_t)1 : (elem_t)0;
    Bm[i][j] = (elem_t)((i+j)%5 - 2);
  }
  memset(Cm,0,sizeof(Cm));

  #define RUN() tiled_matmul_auto(SZ,SZ,SZ,(elem_t*)Am,(elem_t*)Bm,NULL,(elem_t*)Cm, \
      SZ,SZ,SZ,SZ, MVIN_SCALE_IDENTITY,MVIN_SCALE_IDENTITY,MVIN_SCALE_IDENTITY, \
      NO_ACTIVATION,ACC_SCALE_IDENTITY,0,false,false,false,false,false,0,WS)

  RUN();
  int bad=0; for (int i=0;i<SZ;i++) for(int j=0;j<SZ;j++) if (Cm[i][j]!=Bm[i][j]) bad++;
  mark("DIM=%d elem=%zuB opcode=custom%d  정확성 %s (%d/%d)",
       DIM, sizeof(elem_t), XCUSTOM_ACC, bad?"FAIL":"PASS", bad, SZ*SZ);
  if (bad) { mark("=== LIB_MM_DONE ==="); return 1; }

  double best=1e30;
  for (int set=0;set<3;set++){
    double t0=now(); for(int it=0;it<10;it++) RUN();
    double dt=(now()-t0)/10.0; if (dt<best) best=dt;
  }
  double macs=(double)SZ*SZ*SZ, peak=(double)DIM*DIM*31.25e6;
  mark("%.3f ms   %.1f MMAC/s   PE=%d   이론최대 %.0f MMAC/s   활용률 %.1f%%",
       best*1e3, macs/best/1e6, DIM*DIM, peak/1e6, macs/best/peak*100.0);
  mark("=== LIB_MM_DONE ===");
  return 0;
}
