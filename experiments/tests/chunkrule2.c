// chunkrule2.c — E303: 청크 임계가 가속기 수에 비례하는가.
// 사전 등록: experiments/model/PREREG_E303.txt
//
// E302: 청크 목표 = 4 if K·N >= 1.4MB else 2 (50칸, 평균 0.14%). K·N은 B의 바이트 수고,
// 1.4M/3 = 467KB로 L2(512KB)에 붙는다. 기제가 "가속기당 B 대 L2"라면 임계는 m에 비례해야
// 한다 — m=2에서 0.93M. 무관하다면 m=2에서도 1.4M.
// 판별 구간 0.93M < K·N < 1.4M: H_L2는 청크4, H_indep는 청크2를 예측.
// m=2를 찍는 것이 이 판의 유일한 목적이다 (chunkrule.c는 계산만 하고 버렸다).
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
#define MMAX 128
#define KMAX 2560
#define NMAX 6144
#define FREQ 50.0e6
static int BI=8, BJ=4;
static int KCv=16;
static int8_t A[MMAX*KMAX] __attribute__((aligned(64)));
#define BMAX (1536*6144)
static int8_t B[BMAX]      __attribute__((aligned(64)));
static int8_t C[MMAX*NMAX] __attribute__((aligned(64)));
static int8_t REF[MMAX*NMAX];
static int MM,KK,NN;
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
  iter s[3];
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
  fill();
  double roof=(double)MM*KK*NN/256.0/FREQ, t[3]; int bad=0;
  for(int m=1;m<=3;m++){
    run(m); double b=1e30;
    for(int r=0;r<5;r++){ memset(C,0,(size_t)MM*NN);
      double t0=now(); run(m); double d=now()-t0; if(d<b)b=d; bad+=chk(); }
    t[m-1]=b; }
  mark("[%4dx%4d] (%d,%d) Kc=%2d 청크%3d nj=%2d 타일%4d | %9.3f %9.3f %9.3f | %5.2f %5.2f | %s",
       KK,NN,BI,BJ,KCv,(KK/16+KCv-1)/KCv,(NN/16+BJ-1)/BJ,KCv*(BI+BJ),
       t[0]*1e3,t[1]*1e3,t[2]*1e3, t[1]/(roof/2),t[2]/(roof/3), bad?"FAIL":"PASS"); }
int main(int argc,char**argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/chunkrule2.log","w");
  if(!g){perror("로그");exit(1);}
  if(mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof(cs),&cs); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  mark("=== E303 청크 임계의 m 의존성 (권장 구성, M=128, best-of-5) ===");
  mark("판별 구간 0.93M<K·N<1.4M | H_L2: m=2에서 청크4 | H_indep: m=2에서 청크2");
  mark("%11s %6s %4s %5s | %9s %9s %9s | %11s | %s",
       "형상","블록","Kc","청크","m=1","m=2","m=3","η(2) η(3)","정확성");
  MM=128;
  struct { int K,N; const char*g; } SH[]={
    { 768,1024,"아래대조 0.79M"},{ 896,1024,"아래대조 0.92M"},
    {1024,1024,"판별 1.05M"},{ 768,1536,"판별 1.18M"},
    {1024,1280,"판별 1.31M"},{1152,1152,"판별 1.33M"},
    {1280,1280,"위대조 1.64M"},{1024,2048,"위대조 2.10M"}};
  int BLI[]={4,4}, BLJ[]={4,8};
  int CH[]={2,4,8};
  for(unsigned h=0;h<8;h++){ KK=SH[h].K; NN=SH[h].N;
    int Ktil=KK/16;
    mark("--- %s  [%dx%d] Ktil=%d ---",SH[h].g,KK,NN,Ktil);
    for(int b=0;b<2;b++){ BI=BLI[b]; BJ=BLJ[b];
      if ((NN/16) % BJ) continue;
      for(int c=0;c<3;c++){
        if (Ktil % CH[c]) continue;
        KCv = Ktil / CH[c];
        if (KCv*(BI+BJ) > 512) continue;
        one(); } } }
  mark("=== CHUNKRULE2_DONE ===");
  return 0; }
