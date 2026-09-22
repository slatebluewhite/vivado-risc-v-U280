// ksplit.c — K 분할이 큰 K 문제를 살리는가 (E199).
// BERT FFN2 [128x3072]x[3072x768], K=192타일. E198: grt (1,1)이 스톡보다 5.26배 느렸다.
// [예측] K를 16타일씩 끊고 (8,4)를 쓰면 강도가 32.1 -> 6.08로 5.3배 줄어 스톡에 근접한다.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <sys/mman.h>
#include "include/gemmini.h"
#include "include/gemmini_rt.h"
static const grt_ctx AC1 = { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_INT8 };
#define M 128
#define NN 768
#define KK 3072
#define FREQ 50.0e6
static elem_t X[M*KK], W[KK*NN], O[M*NN] __attribute__((aligned(64)));
static elem_t REF[M*NN];
static int BI, BJ, KC;
static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }
static void run_stock(void){
  tiled_matmul_auto(M, NN, KK, X, W, NULL, O, KK, NN, NN, NN,
                    MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY,
                    NO_ACTIVATION, ACC_SCALE_IDENTITY, 0, false,
                    false, false, false, false, 3, WS); }
static void run_ks(void){
  int TI=M/16, TJ=NN/16, TK=KK/16;
  grt_loop_ws_config(&AC1, KK, NN, NN);
  for(int j0=0;j0<TJ;j0+=BJ) for(int i0=0;i0<TI;i0+=BI){
    int I=(i0+BI<=TI)?BI:(TI-i0), J=(j0+BJ<=TJ)?BJ:(TJ-j0);
    grt_block_ksplit(&AC1, I,J,TK,KC, X+(size_t)i0*16*KK, W+(size_t)j0*16,
                     O+(size_t)i0*16*NN+(size_t)j0*16, KK, NN, NN, 1); }
  grt_fence(); }
static void fill(void){
  memset(X,0,sizeof(X));
  for(int r=0;r<M;r++){ X[(size_t)r*KK+r]=1; X[(size_t)r*KK+r+1]=1; }
  for(int r=0;r<KK;r++) for(int c=0;c<NN;c++) W[(size_t)r*NN+c]=(elem_t)((r*3+c*5)%7-3);
  for(int r=0;r<M;r++) for(int c=0;c<NN;c++)
    REF[r*NN+c]=(elem_t)(W[(size_t)r*NN+c]+W[(size_t)(r+1)*NN+c]); }
static int chk(void){ int b=0;
  for(int r=0;r<M;r++) for(int c=0;c<NN;c++) if(O[r*NN+c]!=REF[r*NN+c]) b++; return b; }
static double trial(const char *name, void (*fn)(void)){
  double best=1e30; int worst=0;
  for(int t=0;t<3;t++){ memset(O,0,sizeof(O));
    double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt;
    int b=chk(); if(b>worst)worst=b; }
  double comp=(double)M*NN/256.0*KK;
  mark("%-34s %9.3f ms  활용률 %5.1f%%  %s", name, best*1e3,
       100.0*comp/(best*FREQ), worst?"FAIL":"PASS");
  return best; }
int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/ksplit.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  gemmini_flush(0); grt_flush_ctx(&AC1); fill();
  double comp=(double)M*NN/256.0*KK;
  mark("=== K 분할 (BERT FFN2 [%dx%d]x[%dx%d], K=%d타일, 가속기 1개) ===", M,KK,KK,NN,KK/16);
  mark("이론 연산 %.0f cycle = %.3f ms", comp, comp/FREQ*1e3);
  double st = trial("스톡 tiled_matmul_auto", run_stock);
  struct { int i,j,k; } cfg[] = {{8,4,16},{8,4,8},{4,4,16},{4,6,16},{8,4,32},{2,2,64}};
  double best=1e30; const char *bn="";
  for(unsigned q=0;q<sizeof(cfg)/sizeof(cfg[0]);q++){
    BI=cfg[q].i; BJ=cfg[q].j; KC=cfg[q].k;
    char nm[64]; snprintf(nm,sizeof(nm),"grt K분할 (%d,%d) Kc=%d", BI,BJ,KC);
    double t=trial(nm, run_ks);
    if(t<best){ best=t; }
  }
  mark("");
  mark("=> 최선 grt/스톡 = %.2f배  (E198의 K 분할 없이는 5.26배 느렸다)", best/st);
  mark("=== KSPLIT_DONE ===");
  return 0; }
