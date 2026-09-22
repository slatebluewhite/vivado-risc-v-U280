// order.c — 앞선 측정이 뒤 측정을 바꾸는가 (E223).
//
// E222에서 같은 192³ 재사용 스테퍼가 프로그램에 따라 16~24% 달랐다
// (단독 실행 2.662/4.060 ms 대 여러 크기 훑기 3.482/4.838 ms).
// 사실이면 85점 검증이 섞어 쓴 측정들 사이에 구조적 편향이 있다는 뜻이다.
//
// 한 프로세스 안에서 순서만 바꿔 확인한다:
//   A) 플러시 직후 192³ 먼저
//   B) 128³·256³을 돌린 **뒤** 192³
// 같은 코드, 같은 프로세스, 같은 데이터. 차이가 나면 순서 효과다.
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
static void reuse(void){
  grt_work w0=W(0),w1=W(1),w2=W(2);
  grt_cursor s0={0,0,0,0,0},s1={0,0,0,0,0},s2={0,0,0,0,0};
  for(int a=0;a<NACC;a++) grt_config_ex(&AC[a], GRT_WS);
  int left=NACC;
  while(left>0){ left=0;
    if(NACC>0){ grt_step_r_i8 (&w0,&s0); if(!s0.done) left++; }
    if(NACC>1){ grt_step_r_i8b(&w1,&s1); if(!s1.done) left++; }
    if(NACC>2){ grt_step_r_i8c(&w2,&s2); if(!s2.done) left++; } }
  grt_fence(); }
static void fill(void){
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++){
    int8_t a=(c==r)||(c==(r+1)%SZ), b=(int8_t)((r*3+c*5)%7-3);
    for(int k=0;k<3;k++){ A[k][r*SZ+c]=a; B[k][r*SZ+c]=b; } }
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++)
    REF[r*SZ+c]=(int8_t)(B[0][r*SZ+c]+B[0][((r+1)%SZ)*SZ+c]); }
static int chk(void){ int b=0;
  for(int a=0;a<NACC;a++) for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++)
    if(C[a][r*SZ+c]!=REF[r*SZ+c]) b++; return b; }
static double meas(int *w){
  double best=1e30; *w=0;
  for(int t=0;t<5;t++){ for(int a=0;a<3;a++) memset(C[a],0,(size_t)SZ*SZ);
    double t0=now(); reuse(); double dt=now()-t0; if(dt<best)best=dt;
    int b=chk(); if(b>*w)*w=b; }
  return best; }
static void one(int sz, const char *tag){
  SZ=sz; fill();
  for(NACC=2; NACC<=3; NACC++){
    reuse(); int w; double t=meas(&w);
    mark("  %s %4d³ m=%d : %8.3f ms  %s", tag, SZ, NACC, t*1e3, w?"FAIL":"PASS"); } }
int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/order.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  mark("=== 앞선 측정이 뒤 측정을 바꾸는가 (재사용 스테퍼, best-of-5) ===");
  mark("[A] 플러시 직후 192³");
  one(192,"A");
  mark("[B] 128³·256³을 돌린 뒤 192³");
  one(128,"B-사전");
  one(256,"B-사전");
  one(192,"B");
  mark("[C] 다시 192³ (B 직후)");
  one(192,"C");
  mark("=== ORDER_DONE ===");
  return 0; }
