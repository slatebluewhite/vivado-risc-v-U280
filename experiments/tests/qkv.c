// qkv.c — BERT-base의 Q/K/V 프로젝션을 세 가속기에 (E195).
//
// 지금까지는 전부 정방 합성 행렬이었다. 실제 워크로드에서 확인한다.
// BERT-base 인코더의 Q/K/V는 [128x768] x [768x768] **독립 matmul 세 개**로,
// 세 가속기에 정확히 하나씩 대응한다 — 분할도 통신도 필요 없다.
//
// M=128(8타일), N=K=768(48타일). 블록은 I*J<=32 규칙에 맞춰 (8,4).
//   스크래치패드 I*K + K*J = 8*48 + 48*4 = 576 타일 <= 1024 OK
//   누산기 I*J = 32 <= 64/2 OK (E191에서 32 안전 확인)
//
// 비교: 한 가속기로 셋을 순차 처리 vs 세 가속기로 동시 처리.
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
#define KK 768
#define FREQ 50.0e6
static int8_t X[M*KK] __attribute__((aligned(64)));          // 입력은 셋이 공유
static int8_t W[3][KK*NN] __attribute__((aligned(64)));      // Wq, Wk, Wv
static int8_t O[3][M*NN] __attribute__((aligned(64)));       // Q, K, V
static int8_t REF[M*NN];
static int BI=4, BJ=6, NACC;   /* 스크래치패드 절반 규칙: I*K+K*J = 192+288 = 480 <= 512 */

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }
static double bytes_one(void){
  int TI=M/16, TJ=NN/16, TK=KK/16; double b=0;
  for(int i0=0;i0<TI;i0+=BI) for(int j0=0;j0<TJ;j0+=BJ)
    b += (double)(BI*TK + TK*BJ + BI*BJ)*256.0;
  return b; }

typedef struct { int i0,j0,done; } blk_cur;
static void blk_step(int a, blk_cur *s){
  if (s->done) return;
  int TI=M/16, TJ=NN/16, TK=KK/16;
  grt_loop_ws(&AC[a], BI,BJ,TK, X + (size_t)s->i0*16*KK, W[a] + (size_t)s->j0*16,
              O[a] + (size_t)s->i0*16*NN + (size_t)s->j0*16, KK, NN, NN);
  s->j0+=BJ; if(s->j0>=TJ){ s->j0=0; s->i0+=BI; if(s->i0>=TI) s->done=1; } }

static void run_seq(void){          // 가속기 하나로 Q,K,V를 차례로
  grt_loop_ws_config(&AC[0], KK, NN, NN);
  for(int w=0; w<3; w++){
    blk_cur s={0,0,0};
    int TI=M/16, TJ=NN/16, TK=KK/16;
    while(!s.done){
      grt_loop_ws(&AC[0], BI,BJ,TK, X + (size_t)s.i0*16*KK, W[w] + (size_t)s.j0*16,
                  O[w] + (size_t)s.i0*16*NN + (size_t)s.j0*16, KK, NN, NN);
      s.j0+=BJ; if(s.j0>=TJ){ s.j0=0; s.i0+=BI; if(s.i0>=TI) s.done=1; } }
  }
  grt_fence(); }
static void run_con(void){          // 세 가속기가 하나씩 (NACC개만)
  for(int a=0;a<NACC;a++) grt_loop_ws_config(&AC[a], KK, NN, NN);
  blk_cur s[3]={{0,0,0},{0,0,0},{0,0,0}}; int left=NACC;
  while(left>0){ left=0; for(int a=0;a<NACC;a++){ blk_step(a,&s[a]); if(!s[a].done) left++; } }
  grt_fence(); }

static void fill(void){
  for(int r=0;r<M;r++) for(int c=0;c<KK;c++) X[r*KK+c] = (c==r)||(c==(r+1)%KK);
  for(int r=0;r<KK;r++) for(int c=0;c<NN;c++){
    int8_t v=(int8_t)((r*3+c*5)%7-3);
    for(int w=0;w<3;w++) W[w][r*NN+c]=v; }
  for(int r=0;r<M;r++) for(int c=0;c<NN;c++)
    REF[r*NN+c]=(int8_t)(W[0][r*NN+c] + W[0][((r+1)%KK)*NN+c]); }
static int chk(int n){ int b=0;
  for(int w=0;w<n;w++) for(int r=0;r<M;r++) for(int c=0;c<NN;c++)
    if(O[w][r*NN+c]!=REF[r*NN+c]) b++; return b; }

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/qkv.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  fill();
  double comp = (double)M*NN*KK/256.0;      // matmul 하나의 이론 연산 cycle
  double b1 = bytes_one();

  mark("=== BERT-base Q/K/V 프로젝션 [%dx%d]x[%dx%d] 세 개 (블록 (%d,%d)) ===",
       M,KK,KK,NN,BI,BJ);
  mark("matmul 하나: 이론 연산 %.0f cycle = %.3f ms, 이동 %.0f KB (강도 %.2f B/cycle)",
       comp, comp/FREQ*1e3, b1/1024, b1/comp);
  mark("");
  { memset(O,0,sizeof(O));
    run_seq(); double best=1e30; int worst=0;
    for(int t=0;t<3;t++){ memset(O,0,sizeof(O));
      double t0=now(); run_seq(); double dt=now()-t0; if(dt<best)best=dt;
      int b=chk(3); if(b>worst)worst=b; }
    mark("한 가속기로 셋 순차 : %8.3f ms  (%s)", best*1e3, worst?"FAIL":"PASS");
    mark("");
    double seq=best;
    for(NACC=1; NACC<=3; NACC++){
      run_con();
      double bb=1e30; int ww=0;
      for(int t=0;t<3;t++){ memset(O,0,sizeof(O));
        double t0=now(); run_con(); double dt=now()-t0; if(dt<bb)bb=dt;
        int b=chk(NACC); if(b>ww)ww=b; }
      mark("가속기 %d개 동시    : %8.3f ms  (%d개 처리) 활용률 %5.1f%%  %s",
           NACC, bb*1e3, NACC, 100.0*NACC*comp/(bb*FREQ), ww?"FAIL":"PASS");
      if(NACC==3) mark("  => Q/K/V 전체 대비 순차: **%.2f배**", seq/bb);
    }
  }
  mark("=== QKV_DONE ===");
  return 0; }
