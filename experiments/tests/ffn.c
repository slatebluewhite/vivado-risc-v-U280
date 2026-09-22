// ffn.c — K가 크면 스크래치패드 규칙이 블로킹을 죽인다 (E198).
//
// BERT-base FFN 두 번째 층은 [128x3072] x [3072x768]이다. K=3072 = 192 타일.
// E195의 스크래치패드 절반 규칙 I*K + K*J <= 512 를 넣으면
//     192(I+J) <= 512  =>  I+J <= 2  =>  (I,J) = (1,1) 뿐
// 강도는 16(1/1 + 1/1 + 1/192) = 32.1 B/연산사이클로 최적치(6.5)의 5배다.
//
// 라이브러리는 이 경우 K를 쪼갠다(tiled_matmul_outer의 k0 루프, 중간 블록은 C=NULL로
// 누산기에 남긴다). grt_loop_ws에는 그 기능이 없다.
//
// [측정 전 예측] 내 경로가 스톡보다 여러 배 느리다. 이것이 grt의 적용 한계다.
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
static int BI, BJ;
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
static void run_grt(void){
  int TI=M/16, TJ=NN/16, TK=KK/16;
  grt_loop_ws_config(&AC1, KK, NN, NN);
  for(int j0=0;j0<TJ;j0+=BJ) for(int i0=0;i0<TI;i0+=BI){
    int I=(i0+BI<=TI)?BI:(TI-i0), J=(j0+BJ<=TJ)?BJ:(TJ-j0);
    grt_loop_ws(&AC1, I,J,TK, X+(size_t)i0*16*KK, W+(size_t)j0*16,
                O+(size_t)i0*16*NN+(size_t)j0*16, KK, NN, NN); }
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
  mark("%-30s %9.3f ms  활용률 %5.1f%%  %s", name, best*1e3,
       100.0*comp/(best*FREQ), worst?"FAIL":"PASS");
  return best; }
int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/ffn.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  gemmini_flush(0); grt_flush_ctx(&AC1); fill();
  double comp=(double)M*NN/256.0*KK;
  mark("=== BERT FFN2 [%dx%d]x[%dx%d], K=%d타일, 가속기 1개 ===", M,KK,KK,NN, KK/16);
  mark("이론 연산 %.0f cycle = %.3f ms", comp, comp/FREQ*1e3);
  mark("스크래치패드 규칙 I*K+K*J<=512 => %d(I+J)<=512 => I+J<=%d", KK/16, 512/(KK/16));
  double st = trial("스톡 tiled_matmul_auto", run_stock);
  BI=1; BJ=1; double g1 = trial("grt_loop_ws (1,1)  [유일한 합법]", run_grt);
  BI=2; BJ=1; trial("grt_loop_ws (2,1)  [규칙 위반]", run_grt);
  BI=1; BJ=2; trial("grt_loop_ws (1,2)  [규칙 위반]", run_grt);
  mark("");
  mark("=> grt/스톡 = %.2f배 느림", g1/st);
  mark("=== FFN_DONE ===");
  return 0; }
