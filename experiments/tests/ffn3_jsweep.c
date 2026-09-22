// ffn3.c — K 분할이 열어준 FFN2를 세 가속기로 (E199 2부).
// [예측] 강도 6.08이라 온칩 수요 18.2 < 공급 24로 여유가 있지만,
//        가중치가 2.36 MB로 L2를 한참 넘어 DRAM이 구속할 것이다.
//        고유 트래픽 2.32 B/연산사이클, 천장 4.75 => 약 2.05배가 상한.
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
#define KK 3072
#define FREQ 50.0e6
static int BI=8, BJ=4;
#define KC 16
static int8_t X[M*KK] __attribute__((aligned(64)));
static int8_t W[3][KK*NN] __attribute__((aligned(64)));
static int8_t O[3][M*NN] __attribute__((aligned(64)));
static int8_t REF[M*NN];
static int NACC;
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
  grt_block_ksplit(&AC[a], I,J,TK,KC, X+(size_t)s->i0*16*KK, W[a]+(size_t)s->j0*16,
                   O[a]+(size_t)s->i0*16*NN+(size_t)s->j0*16, KK, NN, NN, 1);
  s->i0+=BI; if(s->i0>=TI){ s->i0=0; s->j0+=BJ; if(s->j0>=TJ) s->done=1; } }  // i 내곽(E196)
static void run(void){
  for(int a=0;a<NACC;a++) grt_loop_ws_config(&AC[a], KK, NN, NN);
  blk_cur s[3]={{0,0,0},{0,0,0},{0,0,0}}; int left=NACC;
  while(left>0){ left=0; for(int a=0;a<NACC;a++){ blk_step(a,&s[a]); if(!s[a].done) left++; } }
  grt_fence(); }
static void fill(void){
  memset(X,0,sizeof(X));
  for(int r=0;r<M;r++){ X[(size_t)r*KK+r]=1; X[(size_t)r*KK+r+1]=1; }
  for(int r=0;r<KK;r++) for(int c=0;c<NN;c++){
    int8_t v=(int8_t)((r*3+c*5)%7-3);
    for(int w=0;w<3;w++) W[w][(size_t)r*NN+c]=v; }
  for(int r=0;r<M;r++) for(int c=0;c<NN;c++)
    REF[r*NN+c]=(int8_t)(W[0][(size_t)r*NN+c]+W[0][(size_t)(r+1)*NN+c]); }
static int chk(void){ int b=0;
  for(int a=0;a<NACC;a++) for(int r=0;r<M;r++) for(int c=0;c<NN;c++)
    if(O[a][r*NN+c]!=REF[r*NN+c]) b++; return b; }
int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/ffn3.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  fill();
  double comp=(double)M*NN/256.0*KK;
  double uniq=(double)M*KK + (double)KK*NN;
  mark("=== FFN2를 세 가속기로 (K분할 (%d,%d) Kc=%d, i 내곽) ===", BI,BJ,KC);
  mark("이론 연산 %.3f ms, 가속기당 고유 %.1f MB (%.2f B/연산사이클)",
       comp/FREQ*1e3, uniq/1048576.0, uniq/comp);
  mark("[가설] 강도가 같은 (8,4)와 (4,8)에서 J가 넓은 쪽이 DRAM 효율이 좋아야 한다.");
  mark("  W 블록 행 폭: J=4 -> 64B, J=8 -> 128B, J=16 -> 256B, J=32 -> 512B (간격 768B)");
  mark("%3s %3s %7s %5s | %10s %10s | %7s | %9s | %s",
       "I","J","강도","개수","시간ms","matmul당","처리량","DRAM B/cyc","정확성");
  int pairs[][2]={{8,4},{4,8},{2,16},{1,32}};
  for(unsigned q=0;q<sizeof(pairs)/sizeof(pairs[0]);q++){
  BI=pairs[q][0]; BJ=pairs[q][1];
  double inten=16.0*(1.0/BI+1.0/BJ+1.0/(KK/16));
  double base=0;
  for(NACC=1; NACC<=3; NACC+=2){
    run();
    double best=1e30; int worst=0;
    for(int t=0;t<3;t++){ for(int a=0;a<3;a++) memset(O[a],0,sizeof(O[a]));
      double t0=now(); run(); double dt=now()-t0; if(dt<best)best=dt;
      int b=chk(); if(b>worst)worst=b; }
    double per=best/NACC; if(NACC==1) base=per;
    mark("%3d %3d %7.2f %5d | %10.3f %10.3f | %6.2fx | %9.2f | %s",
         BI,BJ,inten,NACC, best*1e3, per*1e3,
         base/per, uniq*NACC/(best*FREQ), worst?"FAIL":"PASS");
  } mark(""); }
  mark("=== FFN3_DONE ===");
  return 0; }
