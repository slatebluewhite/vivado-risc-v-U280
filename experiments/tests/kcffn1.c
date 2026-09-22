// kcbig.c — E395: 규칙이 권하지만 이 트랙이 한 번도 안 재본 **큰 Kc**를 잰다.
//
// E394의 출력 채점에서 두 자리가 미측정으로 드러났다:
//   [8192x2048] 1순위 (4,4) Kc=128 (4청크)  — 실측은 Kc=64뿐
//   [3072x3072] 1순위 (8,8) Kc=48  (4청크)  — 실측은 Kc=32뿐
// 블록 축은 전수로 훑었지만 Kc 축은 "규칙이 고른 값"에 묶여 있었다.
//
// E383이 h1536 FFN2에서 "(4,4) Kc=96 4청크"가 "(8,8) Kc=64 6청크"보다 31.5% 나쁜 것을
// 봤으므로, 큰 Kc + 4청크가 나쁠 수 있다. 여기서 직접 확인한다.
//
// 3가속기 판이므로 문맥 셋 다 플러시한다.
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

#define NACC 3
static const grt_ctx AC[NACC] = {
  { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_INT8 },
  { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_FP32 },
  { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_C1   },
};
#define MM   128
#define KMAX 8192
#define NMAX 3072
#define BMAX ((size_t)16777216)   /* 8192*2048 */
#define FREQ 62.5e6
static int BI=4, BJ=4, KCv=64, KK, NN;
static int8_t A[MM*KMAX]  __attribute__((aligned(64)));
static int8_t B[BMAX]     __attribute__((aligned(64)));
static int8_t C[MM*NMAX]  __attribute__((aligned(64)));
static int8_t REF[MM*NMAX];

typedef struct { int jb,i0,step,done; } iter;
static void it_step(int a, iter *s){
  if(s->done) return;
  int TI=MM/16, Ntil=NN/16;
  int j0=s->jb*BJ; int J=(j0+BJ<=Ntil)?BJ:(Ntil-j0);
  int I=(s->i0+BI<=TI)?BI:(TI-s->i0);
  grt_block_ksplit(&AC[a], I,J,KK/16,KCv,
                   A+(size_t)s->i0*16*KK, B+(size_t)j0*16,
                   C+(size_t)s->i0*16*NN+(size_t)j0*16, KK,NN,NN, 1);
  s->i0 += BI;
  if(s->i0>=TI){ s->i0=0; s->jb+=s->step; if(s->jb*BJ>=Ntil) s->done=1; } }
static void run(int m){
  iter s[NACC];
  for(int a=0;a<m;a++){ grt_loop_ws_config(&AC[a], KK,NN,NN);
    s[a]=(iter){a,0,m,0}; if(a*BJ>=NN/16) s[a].done=1; }
  int left; do{ left=0;
    for(int a=0;a<m;a++){ it_step(a,&s[a]); if(!s[a].done) left++; } }while(left);
  grt_fence(); }

static FILE *g;
static void mark(const char *f,...){ if(!g)return; va_list ap; va_start(ap,f);
  vfprintf(g,f,ap); va_end(ap); fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }
static void fill(void){
  memset(A,0,(size_t)MM*KK);
  for(int r=0;r<MM;r++){ int p=r%(KK-1); A[(size_t)r*KK+p]=1; A[(size_t)r*KK+p+1]=1; }
  for(int r=0;r<KK;r++) for(int c=0;c<NN;c++) B[(size_t)r*NN+c]=(int8_t)((r*3+c*5)%7-3);
  for(int r=0;r<MM;r++){ int p=r%(KK-1);
    for(int c=0;c<NN;c++)
      REF[(size_t)r*NN+c]=(int8_t)(B[(size_t)p*NN+c]+B[(size_t)(p+1)*NN+c]); } }
static int chk(void){ int b=0;
  for(int r=0;r<MM;r++) for(int c=0;c<NN;c++)
    if(C[(size_t)r*NN+c]!=REF[(size_t)r*NN+c]) b++; return b; }

static void one(void){
  double roof=(double)MM*KK*NN/256.0/FREQ, t[NACC]; int bad=0;
  for(int m=1;m<=NACC;m++){
    run(m); double b=1e30;
    for(int r=0;r<4;r++){ memset(C,0,(size_t)MM*NN);
      double t0=now(); run(m); double d=now()-t0; if(d<b)b=d; bad+=chk(); }
    t[m-1]=b; }
  int ch=(KK/16+KCv-1)/KCv;
  mark("[%5dx%5d] (%d,%d) Kc=%3d 청크%3d sp%5d | %9.3f %9.3f %9.3f | %5.2f | %s",
       KK,NN,BI,BJ,KCv,ch,KCv*(BI+BJ), t[0]*1e3,t[1]*1e3,t[2]*1e3,
       t[2]*3/roof, bad?"FAIL":"PASS"); }

int main(int argc,char**argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/kcbig.log","w");
  if(!g){perror("로그");exit(1);}
  if(mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof(cs),&cs); sched_yield();
  for(int a=0;a<NACC;a++) grt_flush_ctx(&AC[a]);
  mark("=== E400: FFN1에서 청크 수 훑기 ( 규칙 Kc 대 합법 최대 Kc (3가속기+mem2, best-of-4) ===");
  mark("%13s %5s %6s %5s %6s | %9s %9s %9s | %5s | %s",
       "형상","블록","Kc","청크","sp","m=1","m=2","m=3","η(3)","정확성");

  /* E400: "청크가 적을수록 낫다, 단 2까지"를 미측정 자리에서 검정한다.
     FFN1 [768x3072]는 규칙이 4청크(Kc=12)를 주는데 2청크(Kc=24)·1청크(Kc=48)가 가능하다. */
  struct { int K,N; int kcs[5]; } S[2] = {
    {  768, 3072, {48,24,16,12,0} },   /* BERT-base FFN1 */
    { 1024, 4096, {64,32,16,0,0} },    /* BERT-large FFN1: Ktil=64 */
  };
  BI=8; BJ=8;
  for(int i=0;i<2;i++){
    KK=S[i].K; NN=S[i].N; fill();
    mark("--- [%dx%d] (8,8) Kc 훑기 ---",KK,NN);
    for(int j=0;j<5 && S[i].kcs[j];j++){ KCv=S[i].kcs[j]; one(); }
  }
  mark("=== E400_DONE ===");
  return 0; }
