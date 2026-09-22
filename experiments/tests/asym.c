// asym.c — 비대칭(서로 다른 가속기)에서 결합 방식을 가른다 (E216).
//
// E214: 모델은 동일 가속기 m개만 검증됐다. 비대칭 검증에 쓸 수 있는 점이 하나뿐이었는데
// (INT8 3.195 / FP32 2.453 / 교대 3.897), 거기서 p-노름 -7.1%, max() -18.0%였다.
// 나머지 비대칭 측정은 **순차 합계만** 기록돼 개별 시간으로 못 나눴다.
//
// 여기서는 각 크기마다 **세 값을 모두** 잰다: INT8 단독, FP32 단독, 타일 교대.
// blf50 = 코어0에 INT8 16x16(custom3) + FP32 8x8(custom2).
//
// 타일 수를 맞추려면 INT8 N에 대해 FP32는 N/2다 ((N/16)³ = (N/2/8)³).
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <sys/mman.h>
#include <math.h>
#include "include/gemmini_rt.h"
static const grt_ctx I8 = { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_INT8 };
static const grt_ctx FP = { .dim=8,  .elem_bytes=4, .acc_bytes=4, .opcode=GRT_OP_FP32 };
#define MAXI 256
#define MAXF 128
#define FREQ 50.0e6
static int8_t  A8[MAXI*MAXI], B8[MAXI*MAXI], C8[MAXI*MAXI] __attribute__((aligned(64)));
static float   Af[MAXF*MAXF], Bf[MAXF*MAXF], Cf[MAXF*MAXF] __attribute__((aligned(64)));
static int8_t  R8[MAXI*MAXI];
static float   Rf[MAXF*MAXF];
static int NI, NF;
static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }
static grt_work W8(void){ grt_work w={ .c=&I8,.A=A8,.B=B8,.C=C8,.M=NI,.N=NI,.K=NI }; return w; }

static grt_work Wf(void){ grt_work w={ .c=&FP,.A=Af,.B=Bf,.C=Cf,.M=NF,.N=NF,.K=NF }; return w; }
static void only8(void){ grt_matmul(&I8,A8,B8,C8,NI,NI,NI); }
static void onlyf(void){ grt_matmul(&FP,Af,Bf,Cf,NF,NF,NF); }
static void both(void){ grt_work a=W8(), b=Wf(); grt_mm2(&a,&b); }
static void fill(void){
  for(int r=0;r<NI;r++) for(int c=0;c<NI;c++){
    A8[r*NI+c]=(c==r)||(c==(r+1)%NI); B8[r*NI+c]=(int8_t)((r*3+c*5)%7-3); }
  for(int r=0;r<NI;r++) for(int c=0;c<NI;c++)
    R8[r*NI+c]=(int8_t)(B8[r*NI+c]+B8[((r+1)%NI)*NI+c]);
  for(int r=0;r<NF;r++) for(int c=0;c<NF;c++){
    Af[r*NF+c]=((c==r)||(c==(r+1)%NF))?1.0f:0.0f; Bf[r*NF+c]=(float)((r*3+c*5)%7-3); }
  for(int r=0;r<NF;r++) for(int c=0;c<NF;c++)
    Rf[r*NF+c]=Bf[r*NF+c]+Bf[((r+1)%NF)*NF+c]; }
static int chk8(void){ int b=0;
  for(int r=0;r<NI;r++) for(int c=0;c<NI;c++) if(C8[r*NI+c]!=R8[r*NI+c]) b++; return b; }
static int chkf(void){ int b=0;
  for(int r=0;r<NF;r++) for(int c=0;c<NF;c++)
    if(Cf[r*NF+c]<Rf[r*NF+c]-0.01f||Cf[r*NF+c]>Rf[r*NF+c]+0.01f) b++; return b; }
static double meas(void (*fn)(void), int both_chk, int *w){
  double best=1e30; *w=0;
  for(int t=0;t<3;t++){ memset(C8,0,(size_t)NI*NI); memset(Cf,0,(size_t)NF*NF*4);
    double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt;
    int b = both_chk==0 ? chk8() : (both_chk==1 ? chkf() : chk8()+chkf());
    if(b>*w)*w=b; }
  return best; }
int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/asym.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  grt_flush_ctx(&I8); grt_flush_ctx(&FP);
  mark("=== 비대칭 결합 검증 (blf50: INT8 16x16 + FP32 8x8, 코어0, best-of-3) ===");
  mark("타일 수를 맞춘다: INT8 N, FP32 N/2 -> 양쪽 (N/16)³ 타일연산");
  mark("%5s %5s %7s | %9s %9s %9s | %9s %9s | %s",
       "INT8","FP32","타일","INT8ms","FP32ms","교대ms","max예측","p3예측","정확성");
  int Ns[]={64,128,192,256};
  for(unsigned q=0;q<sizeof(Ns)/sizeof(Ns[0]);q++){
    NI=Ns[q]; NF=NI/2; fill();
    only8(); onlyf(); both();
    int w1,w2,w3;
    double t8=meas(only8,0,&w1), tf=meas(onlyf,1,&w2), tb=meas(both,2,&w3);
    double mx = t8>tf?t8:tf;
    double p3 = (t8*t8*t8 + tf*tf*tf);  p3 = pow(p3, 1.0/3.0);
    mark("%5d %5d %7d | %9.3f %9.3f %9.3f | %9.3f %9.3f | %s",
         NI, NF, (NI/16)*(NI/16)*(NI/16), t8*1e3, tf*1e3, tb*1e3, mx*1e3, p3*1e3,
         (w1||w2||w3)?"FAIL":"PASS");
  }
  mark("=== ASYM_DONE ===");
  return 0; }
