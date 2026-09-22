// hw_requant.c — E150의 결론을 실측으로 확정한다 (E151).
//
// 주장: 레이어 사이 재양자화는 호스트가 할 일이 아니다. Gemmini가 mvout 단계에서
//       int32 누산값에 ACC_SCALE을 적용해 elem_t로 좁혀 준다(`full_C=false`).
//
// 2단 파이프라인 H = X*W1, Y = H*W2 를 두 방식으로 비교한다:
//   (a) 호스트 재양자화: full_C=true로 int32를 받아 호스트가 곱·시프트로 int8로 좁힌다
//   (b) 하드웨어 재양자화: full_C=false로 이미 int8인 결과를 받아 바로 다음 단에 넣는다
//
// 예측: (b)가 단일 matmul 두 번에 가깝고, (a)보다 훨씬 빠르다.
// 두 경로 모두 공식 라이브러리(tiled_matmul_auto)를 쓴다 — 내 구현의 비용을 배제한다.
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

#define N 128
static elem_t X[N*N] row_align(1), W1[N*N] row_align(1), W2[N*N] row_align(1);
static elem_t H8[N*N] row_align(1), Y8[N*N] row_align(1);
static acc_t  H32[N*N] row_align_acc(1);
static volatile int64_t sink;

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }

#define MM(A,B,C,full) tiled_matmul_auto(N,N,N,(elem_t*)(A),(elem_t*)(B),NULL,(C), \
    N,N,N,N, MVIN_SCALE_IDENTITY,MVIN_SCALE_IDENTITY,MVIN_SCALE_IDENTITY, \
    NO_ACTIVATION,ACC_SCALE_IDENTITY,0,false,false,false,(full),false,0,WS)

// (a) 호스트가 int32→int8 재양자화
static void path_host(void){
  MM(X,W1,H32,true);
  const int32_t mult=1518500250; const int shift=31;
  int64_t chk=0;
  for (int i=0;i<N*N;i++){
    int64_t v=((int64_t)H32[i]*mult)>>shift;
    if(v>127)v=127; if(v<-128)v=-128; H8[i]=(elem_t)v; chk+=v;
  }
  sink=chk;
  MM(H8,W2,Y8,false);
}
// (b) 하드웨어가 재양자화 (full_C=false — mvout에서 ACC_SCALE 적용 후 elem_t로 좁힘)
static void path_hw(void){
  MM(X,W1,H8,false);
  MM(H8,W2,Y8,false);
}
static void single(void){ MM(X,W1,H8,false); }

static double timeit(void (*fn)(void), int it){
  double best=1e30;
  for(int s=0;s<3;s++){ double t0=now(); for(int i=0;i<it;i++) fn();
    double dt=(now()-t0)/it; if(dt<best)best=dt; }
  return best;
}

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/hwrq.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  gemmini_flush(0);

  for (int i=0;i<N*N;i++){ X[i]=(elem_t)(i%15-7); W1[i]=(elem_t)(i%11-5); W2[i]=(elem_t)(i%13-6); }

  mark("=== 레이어 사이 재양자화: 호스트 vs 하드웨어 (N=%d, 공식 라이브러리) ===", N);
  double t1 = timeit(single, 5);
  double ta = timeit(path_host, 5);
  double tb = timeit(path_hw, 5);
  mark("단일 matmul 1회                      %8.3f ms", t1*1e3);
  mark("(a) 2단 + 호스트 재양자화            %8.3f ms   단일의 %.2f배", ta*1e3, ta/t1);
  mark("(b) 2단 + 하드웨어 재양자화          %8.3f ms   단일의 %.2f배", tb*1e3, tb/t1);
  mark("→ 하드웨어 재양자화가 %.2f배 빠르다", ta/tb);
  mark("(체크섬 %lld)", (long long)sink);
  mark("=== HWRQ_DONE ===");
  return 0;
}
