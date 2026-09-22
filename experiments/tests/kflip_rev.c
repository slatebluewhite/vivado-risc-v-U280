// kramp.c — E367: N을 고정하고 K만 훑어 온칩 메모리 2배의 이득이 어디서 오르는지 본다.
//
// E366에서 FFN2 이득이 K=3072에 +7.4%, K=4096에 +33.1%, 그 위는 평탄(37~39%)이었다.
// 어느 부등식으로도 설명이 안 된다 — 계산상 (4,8)이 목표 청크에서 합법인 한계는
// K <= 2730이므로 3072은 이미 넘었는데도 이득이 작다.
//
// N=1024 고정, K = 2048 ~ 4096. 각 K에서 블록 4개 x 청크 {2,4,6,8}을 재고
// 옛 한계(sp<=512, acc<=32)에서의 최선과 mem2 한계에서의 최선을 같은 실행에서 비교한다.
// 같은 판이므로 클럭·구현·보드 상태가 전부 같다.
//
// 가속기 2개 판이므로 문맥 2는 건드리지 않는다 (E337).
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
#define MMAX 128
#define KMAX 3584
#define NMAXV 1024
static int NN_ = 1024;   /* E368: 런타임 인자 */
#define FREQ 62.5e6
static int BI=4, BJ=4, KCv=64;
static int8_t A[MMAX*KMAX]        __attribute__((aligned(64)));
static int8_t B[(size_t)KMAX*NMAXV] __attribute__((aligned(64)));
static int8_t C[MMAX*NMAXV]       __attribute__((aligned(64)));
static int8_t REF[MMAX*NMAXV];
static int MM=128, KK;

typedef struct { int jb,i0,step,done; } iter;
static void it_step(int a, iter *s){
  if(s->done) return;
  int TI=MM/16, Ntil=NN_/16;
  int j0=s->jb*BJ; int J=(j0+BJ<=Ntil)?BJ:(Ntil-j0);
  int I=(s->i0+BI<=TI)?BI:(TI-s->i0);
  grt_block_ksplit(&AC[a], I,J,KK/16,KCv,
                   A+(size_t)s->i0*16*KK, B+(size_t)j0*16,
                   C+(size_t)s->i0*16*NN_+(size_t)j0*16, KK,NN_,NN_, 1);
  s->i0 += BI;
  if(s->i0>=TI){ s->i0=0; s->jb+=s->step; if(s->jb*BJ>=Ntil) s->done=1; } }
static void run(int m){
  iter s[NACC];
  for(int a=0;a<m;a++){ grt_loop_ws_config(&AC[a], KK,NN_,NN_);
    s[a]=(iter){a,0,m,0}; if(a*BJ>=NN_/16) s[a].done=1; }
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
  for(int r=0;r<KK;r++) for(int c=0;c<NN_;c++) B[(size_t)r*NN_+c]=(int8_t)((r*3+c*5)%7-3);
  for(int r=0;r<MM;r++){ int p=r%(KK-1);
    for(int c=0;c<NN_;c++)
      REF[(size_t)r*NN_+c]=(int8_t)(B[(size_t)p*NN_+c]+B[(size_t)(p+1)*NN_+c]); } }
static int chk(void){ int b=0;
  for(int r=0;r<MM;r++) for(int c=0;c<NN_;c++)
    if(C[(size_t)r*NN_+c]!=REF[(size_t)r*NN_+c]) b++; return b; }

static void one(int ch){
  double roof=(double)MM*KK*NN_/256.0/FREQ, t[NACC]; int bad=0;
  for(int m=1;m<=NACC;m++){
    run(m); double b=1e30;
    for(int r=0;r<5;r++){ memset(C,0,(size_t)MM*NN_);
      double t0=now(); run(m); double d=now()-t0; if(d<b)b=d; bad+=chk(); }
    t[m-1]=b; }
  int sp=KCv*(BI+BJ), acc=BI*BJ;
  mark("[%5dx%5d] (%d,%d) Kc=%3d 청크%2d 강도%5.2f sp%5d acc%3d | %8.3f %8.3f %8.3f | %5.2f %5.2f | %-3s %s",
       KK,NN_,BI,BJ,KCv,ch,16.0*(1.0/BI+1.0/BJ),sp,acc,
       t[0]*1e3,t[1]*1e3,t[2]*1e3, t[0]/roof, t[2]*3/roof,
       (sp<=512 && acc<=32) ? "옛" : "새", bad?"FAIL":"PASS"); }

int main(int argc,char**argv){
  if(argc>2) NN_ = atoi(argv[2]);
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/kramp.log","w");
  if(!g){perror("로그");exit(1);}
  if(mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof(cs),&cs); sched_yield();
  for(int a=0;a<NACC;a++) grt_flush_ctx(&AC[a]);
  mark("=== E389 (블록 순서 뒤집음):  (8,4) 대 (8,8) 교차 탐색 (N=%d, m=3, mem2 3가속기) ===", NN_);
  mark("'옛'은 sp<=512 & acc<=32 (옛 하드웨어에서도 합법), '새'는 mem2에서만 합법.");
  mark("%13s %5s %6s %4s %6s %6s %4s | %8s %8s %8s | %11s | %s",
       "형상","블록","Kc","청크","강도","sp","acc","m=1","m=2","m=3","η(1) η(3)","분류 정확성");
  const int KS[5]={2560,2816,3072,3328,3584};
  const int CH[1]={4};
  struct { int I,J; } BL[2]={{8,8},{8,4}};   /* E389: 순서 뒤집기 */
  for(int k=0;k<5;k++){
    KK=KS[k]; int Ktil=KK/16;
    mark("--- K=%d (Ktil=%d) ---",KK,Ktil);
    fill();
    for(int b=0;b<2;b++){
      BI=BL[b].I; BJ=BL[b].J;
      if((MM/16)%BI || (NN_/16)%BJ) continue;
      for(int c=0;c<1;c++){
        if(Ktil%CH[c]) continue;
        KCv=Ktil/CH[c];
        if(KCv<4) continue;
        if(KCv*(BI+BJ)>1024 || BI*BJ>64) continue;
        one(CH[c]); } } }
  mark("=== KRAMP_DONE ===");
  return 0; }
