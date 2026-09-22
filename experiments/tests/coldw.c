// coldw.c — projection의 높은 DRAM 대역폭은 L2 잔류 산물인가 (E201).
//
// E199/E200: 연산당 고유 트래픽이 2.33 B/cycle로 같은데 projection은 다중 가속기 2.0~2.25배,
// FFN2는 1.22배다. 차이는 달성 DRAM 대역폭(4.0 대 2.45)인데 접근 폭 가설은 기각됐다(E200).
//
// 남은 후보: **반복 측정의 L2 잔류.** projection의 W는 576 KB로 L2(512 KB)에 근접하므로
// best-of-N의 다음 회차에서 일부가 남아 있을 수 있다. FFN2의 2.36 MB는 전혀 안 남는다.
// 그렇다면 projection의 4.0은 성능이 아니라 **측정 방식의 산물**이다.
//
// [측정 전 예측] 회차마다 **다른 가중치 사본**을 쓰면(L2 잔류 불가) projection의
// 다중 가속기 이득이 2.25배에서 크게 떨어져야 한다. 안 떨어지면 잔류 가설도 기각이다.
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
#define BI 4
#define BJ 6
#define ROT 3                       // 가중치 사본 수 (3 x 3 x 576KB = 5.2MB)
static int8_t X[M*KK] __attribute__((aligned(64)));
static int8_t W[ROT][3][KK*NN] __attribute__((aligned(64)));
static int8_t O[3][M*NN] __attribute__((aligned(64)));
static int8_t REF[M*NN];
static int NACC, ROTI, COLD;
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
  grt_loop_ws(&AC[a], I,J,TK, X+(size_t)s->i0*16*KK, W[ROTI][a]+(size_t)s->j0*16,
              O[a]+(size_t)s->i0*16*NN+(size_t)s->j0*16, KK, NN, NN);
  s->i0+=BI; if(s->i0>=TI){ s->i0=0; s->j0+=BJ; if(s->j0>=TJ) s->done=1; } }  // i 내곽
static void run(void){
  for(int a=0;a<NACC;a++) grt_loop_ws_config(&AC[a], KK, NN, NN);
  blk_cur s[3]={{0,0,0},{0,0,0},{0,0,0}}; int left=NACC;
  while(left>0){ left=0; for(int a=0;a<NACC;a++){ blk_step(a,&s[a]); if(!s[a].done) left++; } }
  grt_fence(); }
static void fill(void){
  memset(X,0,sizeof(X));
  for(int r=0;r<M;r++){ X[r*KK+r]=1; X[r*KK+r+1]=1; }
  for(int r=0;r<KK;r++) for(int c=0;c<NN;c++){
    int8_t v=(int8_t)((r*3+c*5)%7-3);
    for(int t=0;t<ROT;t++) for(int w=0;w<3;w++) W[t][w][(size_t)r*NN+c]=v; }
  for(int r=0;r<M;r++) for(int c=0;c<NN;c++)
    REF[r*NN+c]=(int8_t)(W[0][0][(size_t)r*NN+c]+W[0][0][(size_t)(r+1)*NN+c]); }
static int chk(void){ int b=0;
  for(int a=0;a<NACC;a++) for(int r=0;r<M;r++) for(int c=0;c<NN;c++)
    if(O[a][r*NN+c]!=REF[r*NN+c]) b++; return b; }
int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/coldw.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  fill();
  double comp=(double)M*NN*KK/256.0, uniq=(double)M*KK+(double)KK*NN;
  mark("=== projection의 DRAM 대역폭이 L2 잔류 산물인가 (Q/K/V 형상, 블록 (%d,%d)) ===",BI,BJ);
  mark("W 한 벌 %.0f KB, 사본 %d개. 더움=같은 사본 반복, 차가움=회차마다 다른 사본",
       (double)KK*NN/1024, ROT);
  mark("%6s %5s | %10s %10s | %7s | %9s | %s",
       "가중치","개수","시간ms","matmul당","처리량","DRAM B/cyc","정확성");
  for(COLD=0; COLD<2; COLD++){
    double base=0;
    for(NACC=1; NACC<=3; NACC++){
      ROTI=0; run();                                   /* 웜업 */
      double best=1e30; int worst=0;
      for(int t=0;t<ROT;t++){
        ROTI = COLD ? t : 0;                           /* 차가움이면 매 회차 다른 사본 */
        for(int a=0;a<3;a++) memset(O[a],0,sizeof(O[a]));
        double t0=now(); run(); double dt=now()-t0; if(dt<best)best=dt;
        int b=chk(); if(b>worst)worst=b; }
      double per=best/NACC; if(NACC==1) base=per;
      mark("%6s %5d | %10.3f %10.3f | %6.2fx | %9.2f | %s",
           COLD?"차가움":"더움", NACC, best*1e3, per*1e3, base/per,
           uniq*NACC/(best*FREQ), worst?"FAIL":"PASS");
    }
    mark(""); }
  mark("=== COLDW_DONE ===");
  return 0; }
