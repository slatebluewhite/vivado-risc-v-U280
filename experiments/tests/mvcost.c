// mvcost.c — c_issue를 명령 **종류별**로 분해 (E225).
//
// E224: 재사용(mvin 1) 13.1, 순진(mvin 2) 17.1로 스테퍼마다 다르다. 두 방정식으로 푼
// mvin/기타 분해가 m=2와 m=3에서 크게 달라(30/4.2 대 17.1/12.1) 불안정했다.
//
// mvin 개수를 1~4로 바꾸며 점을 늘린다. 여분 mvin은 쓰이지 않는 스크래치패드 주소로
// 가므로 결과가 바뀌지 않는다. 회귀식:
//     시간 = tops · ( n_mvin·c_mvin + 2·c_other ) / f      (가속기당, x m)
// 즉 시간을 n_mvin에 대해 직선으로 맞추면 기울기가 c_mvin, 절편이 2·c_other다.
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
static const grt_ctx AC[2] = {
  { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_INT8 },
  { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_FP32 },
};
#define SZ 192
#define FREQ 50.0e6
static int8_t A[2][SZ*SZ], B[2][SZ*SZ], C[2][SZ*SZ] __attribute__((aligned(64)));
static int8_t REF[SZ*SZ];
static int NACC=2;
static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }
static grt_work W(int a){ grt_work w={ .c=&AC[a],.A=A[a],.B=B[a],.C=C[a],
                                       .M=SZ,.N=SZ,.K=SZ }; return w; }
#define RUN2(NAME, SA, SB)                                              \
static void NAME(void){                                                 \
  grt_work w0=W(0),w1=W(1);                                             \
  grt_cursor s0={0,0,0,0,0}, s1={0,0,0,0,0};                            \
  grt_config_ex(&AC[0], GRT_WS); grt_config_ex(&AC[1], GRT_WS);         \
  while(!s0.done || !s1.done){ SA(&w0,&s0); SB(&w1,&s1); }              \
  grt_fence(); }
RUN2(run_x0, grt_step_r_i8, grt_step_r_i8b)
RUN2(run_x1, grt_m1a, grt_m1b)
RUN2(run_x2, grt_m2a, grt_m2b)
RUN2(run_x3, grt_m3a, grt_m3b)
static void fill(void){
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++){
    int8_t a=(c==r)||(c==(r+1)%SZ), b=(int8_t)((r*3+c*5)%7-3);
    for(int k=0;k<2;k++){ A[k][r*SZ+c]=a; B[k][r*SZ+c]=b; } }
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++)
    REF[r*SZ+c]=(int8_t)(B[0][r*SZ+c]+B[0][((r+1)%SZ)*SZ+c]); }
static int chk(void){ int b=0;
  for(int a=0;a<NACC;a++) for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++)
    if(C[a][r*SZ+c]!=REF[r*SZ+c]) b++; return b; }
static double meas(void (*fn)(void), int *w){
  double best=1e30; *w=0;
  for(int t=0;t<5;t++){ for(int a=0;a<2;a++) memset(C[a],0,(size_t)SZ*SZ);
    double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt;
    int b=chk(); if(b>*w)*w=b; }
  return best; }
int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/mvcost.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  for(int a=0;a<2;a++) grt_flush_ctx(&AC[a]);
  fill();
  double tops=(double)(SZ/16)*(SZ/16)*(SZ/16);
  mark("=== mvin 개수별 발행 비용 (%d³, 타일연산 %.0f, 가속기 2개, best-of-5) ===", SZ, tops);
  mark("여분 mvin은 쓰이지 않는 주소로 — 결과 불변. 기울기=c_mvin, 절편=2·c_other");
  mark("%7s %9s | %11s | %s", "mvin수","시간ms","cycle/타일연산","정확성");
  void (*V[4])(void)={run_x0,run_x1,run_x2,run_x3};
  double cy[4];
  for(int q=0;q<4;q++){
    V[q](); int w; double t=meas(V[q],&w);
    cy[q] = t*FREQ/(NACC*tops);
    mark("%7d %9.3f | %11.2f | %s", q+1, t*1e3, cy[q], w?"FAIL":"PASS");
  }
  /* 선형 회귀: cy = c_mvin·n + 2·c_other  (n = 1..4, A 적재는 무시할 수준) */
  double sx=0,sy=0,sxx=0,sxy=0;
  for(int q=0;q<4;q++){ double x=q+1; sx+=x; sy+=cy[q]; sxx+=x*x; sxy+=x*cy[q]; }
  double slope=(4*sxy-sx*sy)/(4*sxx-sx*sx), icpt=(sy-slope*sx)/4;
  mark("");
  mark("회귀:  c_mvin = %.2f cycle,  c_other = %.2f cycle (절편 %.2f / 2)",
       slope, icpt/2, icpt);
  mark("=== MVCOST_DONE ===");
  return 0; }
