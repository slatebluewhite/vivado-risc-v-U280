// wide.c — B를 4타일씩 실으면 연산 루프에서도 빨라지는가 (E227).
//
// E226: 순수 DMA에서 mvin 비용이 폭과 무관하게 명령당 76 cycle이라, 1타일씩 싣는 것은
// 4배 손해다(m=3에서 3.21 대 13.33 B/cycle). 연산 루프에서는 mvin이 부분적으로 가려지므로
// 이득이 그대로 나오지는 않을 것이다 — 얼마나 나오는지 잰다.
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
#define MAXN 256
#define FREQ 50.0e6
static int8_t A[3][MAXN*MAXN], B[3][MAXN*MAXN], C[3][MAXN*MAXN] __attribute__((aligned(64)));
static int8_t REF[MAXN*MAXN];
static int SZ, NACC;
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
RUNNER(run_narrow, grt_step_r_i8, grt_step_r_i8b, grt_step_r_i8c)
RUNNER(run_wide,   grt_step_w_i8, grt_step_w_i8b, grt_step_w_i8c)
RUNNER(run_wide4,  grt_step_w4_i8, grt_step_w4_i8b, grt_step_w4_i8c)
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
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/wide.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  mark("=== B를 1타일 대 4타일씩 싣기 (재사용 스테퍼, best-of-5) ===");
  mark("[가설] 4타일 mvin이 공유에서 손해인 이유가 이중 버퍼 입도라면,");
  mark("       버퍼를 4개로 늘리면(4버퍼) 회복돼야 한다.");
  mark("%5s %5s | %9s %9s %9s | %7s %7s | %s",
       "크기","개수","1타일ms","4타일ms","4버퍼ms","2버퍼","4버퍼","정확성");
  int Ns[]={128,192,256};
  for(unsigned z=0;z<3;z++){
    SZ=Ns[z]; fill();

    for(NACC=1; NACC<=3; NACC++){
      run_narrow(); run_wide(); run_wide4();
      int w1,w2,w3;
      double tn=meas(run_narrow,&w1), tw=meas(run_wide,&w2), t4=meas(run_wide4,&w3);
      mark("%5d %5d | %9.3f %9.3f %9.3f | %6.2fx %6.2fx | %s",
           SZ, NACC, tn*1e3, tw*1e3, t4*1e3, tn/tw, tn/t4,
           (w1||w2||w3)?"FAIL":"PASS");
    } }
  mark("=== WIDE_DONE ===");
  return 0; }
