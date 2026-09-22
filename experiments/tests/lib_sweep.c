// lib_sweep.c — 행렬 크기를 훑으며 활용률을 잰다 (E139).
//
// 질문: "PE가 1/4(64 vs 256)인데 실제 성능은 얼마나 떨어지는가?"
// 지금까지 세 개의 답이 나왔다: 6.80배(대역폭 병목), 2.93배(64³ 라이브러리),
// ~4.0배(BERT). 차이가 **행렬 크기**로 설명되는지 곡선으로 확정한다.
//
// 예측: 크기가 커지면 16×16 메시가 차면서 INT8:FP32 비가 2.93배에서 4배 쪽으로 오른다.
//       (8×8 메시는 작은 크기에서도 잘 차므로 덜 변한다)
//
// 헤더를 바꿔 INT8판/FP32판 두 바이너리로 빌드한다. 코드는 완전히 동일하다.
// 측정 규칙: 최선값 of 3세트 × 반복(큰 크기는 반복 수를 줄인다).
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

#define MAXSZ 256
static elem_t Am[MAXSZ*MAXSZ] row_align(1);
static elem_t Bm[MAXSZ*MAXSZ] row_align(1);
static elem_t Cm[MAXSZ*MAXSZ] row_align(1);

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec + t.tv_nsec*1e-9; }

static void run(int n){
  tiled_matmul_auto(n,n,n, Am,Bm,NULL,Cm, n,n,n,n,
      MVIN_SCALE_IDENTITY,MVIN_SCALE_IDENTITY,MVIN_SCALE_IDENTITY,
      NO_ACTIVATION,ACC_SCALE_IDENTITY,0,false,false,false,false,false,0,WS);
}

int main(int argc, char **argv){
  g = fopen(argc>1?argv[1]:"/mnt2/tmp/lib_sweep.log","w");
  if(!g){ perror("로그"); exit(1); }
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  gemmini_flush(0);

  double peak = (double)DIM*DIM*31.25e6;
  mark("=== 크기 훑기: DIM=%d elem=%zuB opcode=custom%d, 이론최대 %.0f MMAC/s ===",
       DIM, sizeof(elem_t), XCUSTOM_ACC, peak/1e6);
  mark("%6s %10s %12s %9s %8s", "N", "ms", "MMAC/s", "활용률", "정확성");

  int sizes[] = {32, 64, 128, 192, 256};
  for (unsigned si=0; si<sizeof(sizes)/sizeof(sizes[0]); si++){
    int n = sizes[si];
    // A는 단위행렬 → C는 B와 같아야 한다 (정수·부동소수 모두 정확한 검증)
    for (int i=0;i<n;i++) for(int j=0;j<n;j++){
      Am[i*n+j] = (i==j)?(elem_t)1:(elem_t)0;
      Bm[i*n+j] = (elem_t)((i+j)%5 - 2);
    }
    memset(Cm,0,(size_t)n*n*sizeof(elem_t));
    run(n);
    int bad=0;
    for (int i=0;i<n;i++) for(int j=0;j<n;j++) if (Cm[i*n+j]!=Bm[i*n+j]) bad++;

    int iters = n<=64 ? 10 : (n<=128 ? 5 : 3);
    double best=1e30;
    for (int set=0; set<3; set++){
      double t0=now(); for(int it=0; it<iters; it++) run(n);
      double dt=(now()-t0)/iters; if (dt<best) best=dt;
    }
    double macs=(double)n*n*n;
    mark("%6d %10.3f %12.1f %8.1f%% %8s",
         n, best*1e3, macs/best/1e6, macs/best/peak*100.0, bad?"FAIL":"PASS");
  }
  mark("=== SWEEP_DONE ===");
  return 0;
}
