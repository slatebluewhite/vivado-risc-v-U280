// steporder.c — 앞선 **스테퍼**가 뒤 측정을 바꾸는가, 그리고 깨끗한 c_issue (E224).
//
// E223: 크기 순서는 무해(0.4%)한데 E220의 값이 27%·14% 재현 안 됐다.
// E220은 각 크기에서 순진을 먼저 돌린 뒤 재사용을 쟀다. 두 스테퍼는 스크래치패드
// 배치가 다르다(순진 0/64, 재사용 0/64/128).
//
// 모드로 순서를 바꿔 직접 친다:
//   0: 재사용만        1: 순진만
//   2: 순진 -> 재사용 (E220 구조, 재사용을 보고)
//   3: 재사용 -> 순진 (반대, 순진을 보고)
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
static void naive(void){
  grt_work w0=W(0),w1=W(1),w2=W(2);
  if(NACC==2) grt_mm2_i8i8(&w0,&w1); else grt_mm3_i8(&w0,&w1,&w2); }
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
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/steporder.log","w");
  if(!g){perror("로그");exit(1);}
  int mode = argc>2 ? atoi(argv[2]) : 0;
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  const char *MN[]={"재사용만","순진만","순진->재사용","재사용->순진"};
  mark("=== 모드 %d: %s (best-of-5) ===", mode, MN[mode]);
  mark("%5s %5s | %9s | %11s | %s", "크기","개수","시간ms","역산 c_issue","정확성");
  int Ns[]={128,192,256};
  for(unsigned z=0;z<3;z++){
    SZ=Ns[z]; fill();
    double tops=(double)(SZ/16)*(SZ/16)*(SZ/16);
    for(NACC=2; NACC<=3; NACC++){
      double t; int w; double wc;
      if(mode==0){ reuse(); t=meas(reuse,&w); wc=3.0+16.0/SZ; }
      else if(mode==1){ naive(); t=meas(naive,&w); wc=4.0; }
      else if(mode==2){ naive(); meas(naive,&w); reuse(); t=meas(reuse,&w); wc=3.0+16.0/SZ; }
      else { reuse(); meas(reuse,&w); naive(); t=meas(naive,&w); wc=4.0; }
      mark("%5d %5d | %9.3f | %11.2f | %s", SZ, NACC, t*1e3,
           t*FREQ/(NACC*tops*wc), w?"FAIL":"PASS");
    } }
  mark("=== STEPORDER_DONE ===");
  return 0; }
