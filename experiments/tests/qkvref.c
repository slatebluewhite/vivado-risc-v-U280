// qkvref.c — Q/K/V 형상에서 내 grt 경로가 스톡 라이브러리와 대등한가 (E197).
//
// E195~E196에서 Q/K/V의 다중 가속기 배수를 보고했지만, 기준선이 **내 구현**이었다.
// 스톡 tiled_matmul_auto보다 느린 기준선 위에서 잰 배수는 의미가 약하다.
// E178에서 정방 행렬은 대등함을 확인했으나(0.626 대 0.636 ms) 비정방은 확인 안 했다.
//
// 가속기 하나에서 [128x768]x[768x768]을 세 방식으로 잰다:
//   스톡 tiled_matmul_auto (custom3 고정)
//   내 grt_loop_ws, j 내곽 (E195의 순회)
//   내 grt_loop_ws, i 내곽 (E196의 개선)
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
#define KK 768
#define FREQ 50.0e6
#define BI 4
#define BJ 6
static elem_t X[M*KK], W[KK*NN], O[M*NN] __attribute__((aligned(64)));
static elem_t REF[M*NN];
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
static void emit(int i0,int j0){
  int TI=M/16, TJ=NN/16, TK=KK/16;
  int I=(i0+BI<=TI)?BI:(TI-i0), J=(j0+BJ<=TJ)?BJ:(TJ-j0);
  grt_loop_ws(&AC1, I,J,TK, X+(size_t)i0*16*KK, W+(size_t)j0*16,
              O+(size_t)i0*16*NN+(size_t)j0*16, KK, NN, NN); }
static void run_jin(void){
  int TI=M/16, TJ=NN/16;
  grt_loop_ws_config(&AC1, KK, NN, NN);
  for(int i0=0;i0<TI;i0+=BI) for(int j0=0;j0<TJ;j0+=BJ) emit(i0,j0);
  grt_fence(); }
static void run_iin(void){
  int TI=M/16, TJ=NN/16;
  grt_loop_ws_config(&AC1, KK, NN, NN);
  for(int j0=0;j0<TJ;j0+=BJ) for(int i0=0;i0<TI;i0+=BI) emit(i0,j0);
  grt_fence(); }
static void fill(void){
  memset(X,0,sizeof(X));
  for(int r=0;r<M;r++){ X[r*KK+r]=1; X[r*KK+r+1]=1; }
  for(int r=0;r<KK;r++) for(int c=0;c<NN;c++) W[r*NN+c]=(elem_t)((r*3+c*5)%7-3);
  for(int r=0;r<M;r++) for(int c=0;c<NN;c++)
    REF[r*NN+c]=(elem_t)(W[r*NN+c]+W[(r+1)*NN+c]); }
static int chk(void){ int b=0;
  for(int r=0;r<M;r++) for(int c=0;c<NN;c++) if(O[r*NN+c]!=REF[r*NN+c]) b++; return b; }
static void trial(const char *name, void (*fn)(void)){
  double best=1e30; int worst=0;
  for(int t=0;t<5;t++){ memset(O,0,sizeof(O));
    double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt;
    int b=chk(); if(b>worst)worst=b; }
  double comp=(double)M*NN*KK/256.0;
  mark("%-28s %9.3f ms  활용률 %5.1f%%  %s", name, best*1e3,
       100.0*comp/(best*FREQ), worst?"FAIL":"PASS"); }
int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/qkvref.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  gemmini_flush(0); grt_flush_ctx(&AC1); fill();
  mark("=== Q/K/V 형상 [%dx%d]x[%dx%d], 가속기 1개, best-of-5 ===", M,KK,KK,NN);
  mark("이론 연산 %.0f cycle = %.3f ms", (double)M*NN*KK/256.0, (double)M*NN*KK/256.0/FREQ*1e3);
  trial("스톡 tiled_matmul_auto", run_stock);
  trial("grt_loop_ws (4,6) j내곽", run_jin);
  trial("grt_loop_ws (4,6) i내곽", run_iin);
  mark("=== QKVREF_DONE ===");
  return 0; }
