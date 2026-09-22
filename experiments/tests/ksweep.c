// ksweep.c — 공유 열화 항을 K의 함수로 (E234).
//
// E233: 모델의 `T_dram`은 메모리 항이 아니라 "N개 FSM 동시 실행의 실효 처리율"이다.
// 지금 값은 3점뿐이다 — 정방 23.6, BERT projection 14.6, FFN2 7.07 B/cycle.
// M·N을 고정하고 K만 훑어 그 항을 구조 파라미터의 함수로 만든다.
//
// M=128, N=768 고정, K = 384/768/1536/3072. 블록은 (4,6) 고정
// (I·J=24<=32, TI=8은 4로, TJ=48은 6으로 나누어떨어짐, K 분할 Kc=16).
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
#define M 128
#define NN 768
#define MAXK 3072
#define FREQ 50.0e6
#define BI 4
#define BJ 6
#define KC 16
static int8_t X[M*MAXK] __attribute__((aligned(64)));
static int8_t Wt[3][MAXK*NN] __attribute__((aligned(64)));
static int8_t O[3][M*NN] __attribute__((aligned(64)));
static int8_t REF[M*NN];
static int KK, NACC;
static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }
static double bytes_one(void){
  int TI=M/16, TJ=NN/16, TK=KK/16; double b=0;
  for(int i0=0;i0<TI;i0+=BI) for(int j0=0;j0<TJ;j0+=BJ){
    int I=(i0+BI<=TI)?BI:(TI-i0), J=(j0+BJ<=TJ)?BJ:(TJ-j0);
    b += (double)(I*TK + TK*J + I*J)*256.0; }
  return b; }
typedef struct { int i0,j0,done; } cur;
static void step(int a, cur *s){
  if (s->done) return;
  int TI=M/16, TJ=NN/16, TK=KK/16;
  int I=(s->i0+BI<=TI)?BI:(TI-s->i0), J=(s->j0+BJ<=TJ)?BJ:(TJ-s->j0);
  grt_block_ksplit(&AC[a], I,J,TK,KC, X+(size_t)s->i0*16*KK, Wt[a]+(size_t)s->j0*16,
                   O[a]+(size_t)s->i0*16*NN+(size_t)s->j0*16, KK, NN, NN, 1);
  s->i0+=BI; if(s->i0>=TI){ s->i0=0; s->j0+=BJ; if(s->j0>=TJ) s->done=1; } }
static void run(void){
  for(int a=0;a<NACC;a++) grt_loop_ws_config(&AC[a], KK, NN, NN);
  cur s[3]={{0,0,0},{0,0,0},{0,0,0}}; int left=NACC;
  while(left>0){ left=0; for(int a=0;a<NACC;a++){ step(a,&s[a]); if(!s[a].done) left++; } }
  grt_fence(); }
static void fill(void){
  memset(X,0,(size_t)M*KK);
  for(int r=0;r<M;r++){ X[(size_t)r*KK+r]=1; X[(size_t)r*KK+r+1]=1; }
  for(int r=0;r<KK;r++) for(int c=0;c<NN;c++){
    int8_t v=(int8_t)((r*3+c*5)%7-3);
    for(int w=0;w<3;w++) Wt[w][(size_t)r*NN+c]=v; }
  for(int r=0;r<M;r++) for(int c=0;c<NN;c++)
    REF[r*NN+c]=(int8_t)(Wt[0][(size_t)r*NN+c]+Wt[0][(size_t)(r+1)*NN+c]); }
static int chk(void){ int b=0;
  for(int a=0;a<NACC;a++) for(int r=0;r<M;r++) for(int c=0;c<NN;c++)
    if(O[a][r*NN+c]!=REF[r*NN+c]) b++; return b; }
static double meas(int *w){
  double best=1e30; *w=0;
  for(int t=0;t<3;t++){ for(int a=0;a<3;a++) memset(O[a],0,(size_t)M*NN);
    double t0=now(); run(); double dt=now()-t0; if(dt<best)best=dt;
    int b=chk(); if(b>*w)*w=b; }
  return best; }
int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/ksweep.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  mark("=== 공유 열화 항을 K의 함수로 (M=%d, N=%d, 블록 (%d,%d), Kc=%d) ===", M,NN,BI,BJ,KC);
  mark("E235: m=2도 재서 (작업셋 x 가속기 수) 2차원으로 완성한다.");
  mark("%6s %5s %8s | %9s %9s | %7s | %9s | %s",
       "K","개수","작업셋MB","1개ms","m개ms","열화","실효B/cyc","정확성");
  int Ks[]={384,768,1536,3072};
  for(unsigned q=0;q<4;q++){
    KK=Ks[q]; fill();
    double by=bytes_one();
    NACC=1; run(); int w1; double t1=meas(&w1);
    for(int mm=2; mm<=3; mm++){
      NACC=mm; run(); int wm; double tm=meas(&wm);
      double ws=(double)M*KK + (double)mm*((double)KK*NN + (double)M*NN);
      mark("%6d %5d %8.2f | %9.3f %9.3f | %6.2fx | %9.2f | %s",
           KK, mm, ws/1048576.0, t1*1e3, tm*1e3, tm/t1,
           mm*by/(tm*FREQ), (w1||wm)?"FAIL":"PASS");
    } }
  mark("=== KSWEEP_DONE ===");
  return 0; }
