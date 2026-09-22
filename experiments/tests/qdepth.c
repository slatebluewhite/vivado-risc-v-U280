// qdepth.c — c_issue가 타일당 명령 수에 의존하는가 (E222).
//
// E220: m=3에서 순진(작업명령 4)이 13.7~14.7, 재사용(3)이 15.6~16.7로 갈렸다.
// 가설: **타일당 명령이 많을수록 큐가 깊어져 명령당 비용이 상각된다.**
//
// 깨끗이 재려면 명령만 늘리고 바이트는 그대로여야 한다. preload는 스크래치패드에서만
// 읽으므로 DMA가 없다 — 같은 주소를 REP번 반복하면 결과는 그대로면서 발행만 늘어난다.
// 작업명령 = 2 + REP (mvin B, preload xREP, compute) + A 적재(j==0에서만).
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
static const grt_ctx AC[3] = {
  { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_INT8 },
  { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_FP32 },
  { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_C1   },
};
#define SZ 192
#define FREQ 50.0e6
static int8_t A[3][SZ*SZ], B[3][SZ*SZ], C[3][SZ*SZ] __attribute__((aligned(64)));
static int8_t REF[SZ*SZ];
static int NACC;
static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }
static grt_work W(int a){ grt_work w={ .c=&AC[a],.A=A[a],.B=B[a],.C=C[a],
                                       .M=SZ,.N=SZ,.K=SZ }; return w; }
#define RUNNER(NAME, SA, SB, SC)                                        \
static void NAME(void){                                                 \
  grt_work w0=W(0),w1=W(1),w2=W(2);                                     \
  grt_cursor s0={0,0,0,0,0}, s1={0,0,0,0,0}, s2={0,0,0,0,0};            \
  for(int a=0;a<NACC;a++) grt_config_ex(&AC[a], GRT_WS);                \
  int left=NACC;                                                        \
  while(left>0){ left=0;                                                \
    if(NACC>0){ SA(&w0,&s0); if(!s0.done) left++; }                     \
    if(NACC>1){ SB(&w1,&s1); if(!s1.done) left++; }                     \
    if(NACC>2){ SC(&w2,&s2); if(!s2.done) left++; } }                   \
  grt_fence(); }
RUNNER(run_r0, grt_step_r_i8, grt_step_r_i8b, grt_step_r_i8c)
RUNNER(run_r1, grt_s_r1a, grt_s_r1b, grt_s_r1c)
RUNNER(run_r3, grt_s_r3a, grt_s_r3b, grt_s_r3c)
RUNNER(run_r6, grt_s_r6a, grt_s_r6b, grt_s_r6c)
static void fill(void){
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++){
    int8_t a=(c==r)||(c==(r+1)%SZ), b=(int8_t)((r*3+c*5)%7-3);
    for(int k=0;k<3;k++){ A[k][r*SZ+c]=a; B[k][r*SZ+c]=b; } }
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++)
    REF[r*SZ+c]=(int8_t)(B[0][r*SZ+c]+B[0][((r+1)%SZ)*SZ+c]); }
static int chk(void){ int b=0;
  for(int a=0;a<NACC;a++) for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++)
    if(C[a][r*SZ+c]!=REF[r*SZ+c]) b++; return b; }
static double meas(void (*fn)(void), int *w){
  double best=1e30; *w=0;
  for(int t=0;t<5;t++){ for(int a=0;a<3;a++) memset(C[a],0,(size_t)SZ*SZ);
    double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt;
    int b=chk(); if(b>*w)*w=b; }
  return best; }
int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/qdepth.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  fill();
  double tops=(double)(SZ/16)*(SZ/16)*(SZ/16);
  mark("=== c_issue의 명령 수 의존성 (%d³, 타일연산 %.0f, preload 반복으로 명령만 증가) ===",
       SZ, tops);
  mark("바이트는 REP과 무관하다 — preload는 스크래치패드만 읽는다.");
  mark("%6s %5s %5s | %9s | %11s", "REP","작업명령","개수","시간ms","역산 c_issue");
  struct { int rep; void (*f)(void); } V[]={{1,run_r0},{1,run_r1},{3,run_r3},{6,run_r6}};
  const char *nm[]={"기준(1)","1","3","6"};
  for(unsigned q=0;q<4;q++){
    double wc = 2.0 + V[q].rep;                  /* mvin B + preload xREP + compute */
    for(NACC=2; NACC<=3; NACC++){
      V[q].f();
      int w; double t=meas(V[q].f,&w);
      mark("%6s %5.0f %5d | %9.3f | %11.2f %s", nm[q], wc, NACC, t*1e3,
           t*FREQ/(NACC*tops*wc), w?"FAIL":"PASS");
    } }
  mark("=== QDEPTH_DONE ===");
  return 0; }
