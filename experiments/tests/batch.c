// batch.c — 배치를 키우면 세 번째 가속기가 살아나는가 (E196).
//
// E195: BERT Q/K/V(M=128)에서 두 가속기는 1.92배인데 세 번째가 1.91배로 아무것도 못 한다.
// 원인은 DRAM — 가중치 576 KB가 L2를 혼자 넘어 스트리밍되고, 두 가속기에서 이미
// 3.56 B/cycle로 천장(4.75 읽기 / 2.91 혼합)에 닿는다.
//
// 처방은 명확하다: **가중치를 더 많은 입력 행에 재사용**하면 연산당 DRAM 트래픽이 준다.
// M=128 -> 고유 674 KB / 연산 294,912 cycle = 2.34 B/연산사이클
// M=512 -> 고유 960 KB / 연산 1,179,648 cycle = **0.83** B/연산사이클 (2.8배 개선)
//
// [측정 전 예측] M=128에서 세 번째가 0을 더하지만, M=512에서는 제값을 해야 한다.
// 블록은 (4,6) — 두 절반 규칙(I·J<=32, I·K+K·J<=512)을 모두 만족하는 최적.
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
#define MAXM 512
#define NN 768
#define KK 768
#define FREQ 50.0e6
#define BI 4
#define BJ 6
static int8_t X[MAXM*KK] __attribute__((aligned(64)));
static int8_t W[3][KK*NN] __attribute__((aligned(64)));
static int8_t O[3][MAXM*NN] __attribute__((aligned(64)));
static int8_t REF[MAXM*NN];
static int M, NACC, IINNER;   // IINNER=1이면 for j0 { for i0 } — W 블록 재사용
static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }
typedef struct { int i0,j0,done; } blk_cur;
static void blk_step(int a, blk_cur *s){
  if (s->done) return;
  int TI=M/16, TJ=NN/16, TK=KK/16;
  int I=(s->i0+BI<=TI)?BI:(TI-s->i0), J=(s->j0+BJ<=TJ)?BJ:(TJ-s->j0);
  grt_loop_ws(&AC[a], I,J,TK, X+(size_t)s->i0*16*KK, W[a]+(size_t)s->j0*16,
              O[a]+(size_t)s->i0*16*NN+(size_t)s->j0*16, KK, NN, NN);
  if (IINNER) { /* j0 바깥, i0 안쪽: 같은 W 블록을 모든 i0에 재사용 */
    s->i0+=BI; if(s->i0>=TI){ s->i0=0; s->j0+=BJ; if(s->j0>=TJ) s->done=1; }
  } else {
    s->j0+=BJ; if(s->j0>=TJ){ s->j0=0; s->i0+=BI; if(s->i0>=TI) s->done=1; }
  } }
static void run(void){
  for(int a=0;a<NACC;a++) grt_loop_ws_config(&AC[a], KK, NN, NN);
  blk_cur s[3]={{0,0,0},{0,0,0},{0,0,0}}; int left=NACC;
  while(left>0){ left=0; for(int a=0;a<NACC;a++){ blk_step(a,&s[a]); if(!s[a].done) left++; } }
  grt_fence(); }
static void fill(void){
  memset(X,0,sizeof(X));
  for(int r=0;r<M;r++){ X[(size_t)r*KK + (r%KK)]=1; X[(size_t)r*KK + ((r+1)%KK)]=1; }
  for(int r=0;r<KK;r++) for(int c=0;c<NN;c++){
    int8_t v=(int8_t)((r*3+c*5)%7-3);
    for(int w=0;w<3;w++) W[w][(size_t)r*NN+c]=v; }
  for(int r=0;r<M;r++) for(int c=0;c<NN;c++)
    REF[(size_t)r*NN+c]=(int8_t)(W[0][(size_t)(r%KK)*NN+c] + W[0][(size_t)((r+1)%KK)*NN+c]); }
static int chk(void){ int b=0;
  for(int a=0;a<NACC;a++) for(int r=0;r<M;r++) for(int c=0;c<NN;c++)
    if(O[a][(size_t)r*NN+c]!=REF[(size_t)r*NN+c]) b++; return b; }
int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/batch.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  mark("=== 배치를 키우면 세 번째 가속기가 살아나는가 (Q/K/V 형상, 블록 (%d,%d)) ===",BI,BJ);
  mark("배치 예측은 빗나갔다(E196). 여기서는 **순회 순서**를 비교한다:");
  mark("  j내곽 = for i0 { for j0 } — i0 한 행마다 W 전체(576KB)를 훑어 L2 초과, 재적재");
  mark("  i내곽 = for j0 { for i0 } — W 블록(72KB)이 모든 i0에 고정되어 재사용");
  mark("[예측] i내곽이 W 재적재를 없애 세 번째 가속기가 살아나야 한다.");
  mark("%5s %7s %9s %5s | %9s %10s | %7s | %8s | %s",
       "M","순회","고유KB","개수","시간ms","matmul당","처리량","DRAM B/cyc","정확성");
  int Ms[]={128,512};
  for(unsigned m=0;m<2;m++){
    M=Ms[m]; fill();
    double comp=(double)M*NN*KK/256.0;
    double uniq=(double)M*KK + (double)KK*NN;    // X + W (가속기당)
    for(IINNER=0; IINNER<2; IINNER++){
    double base=0;
    for(NACC=1; NACC<=3; NACC++){
      run();
      double best=1e30; int worst=0;
      for(int t=0;t<3;t++){ for(int a=0;a<3;a++) memset(O[a],0,(size_t)M*NN);
        double t0=now(); run(); double dt=now()-t0; if(dt<best)best=dt;
        int b=chk(); if(b>worst)worst=b; }
      double per=best/NACC; if(NACC==1) base=per;
      mark("%5d %6s %9.0f %5d | %9.3f %10.3f | %6.2fx | %8.2f | %s",
           M, IINNER?"i내곽":"j내곽", uniq/1024, NACC, best*1e3, per*1e3, base/per,
           (uniq*NACC)/(best*FREQ), worst?"FAIL":"PASS");
    } mark(""); }
    mark("  (연산당 고유 트래픽 %.2f B/연산사이클)", uniq/comp);
    mark(""); }
  mark("=== BATCH_DONE ===");
  return 0; }
