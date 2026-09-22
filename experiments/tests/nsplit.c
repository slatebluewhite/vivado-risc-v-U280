// nsplit.c — E360: 바이트와 청크를 고정하고 K:N 분할만 바꾼다.
// 사전 등록: experiments/model/PREREG_E360.txt
//
// K·N을 고정하면 MAC 수·A 트래픽·B 트래픽이 전부 같고, 청크 4와 블록 (4,4)를 고정하면
// 남는 차이는 C 트래픽(총 바이트의 2.3%)뿐이다. 그 상태에서 N을 4배 범위로 바꾼다.
//
// H0: 한 세트 안 세 점이 3% 안 -> b(N)은 바이트의 대리변수였다.
// H1: 스프레드 10% 초과      -> 모양이 독립으로 문다.
//
// (K=8192, N=512/1024)는 sp=1024 타일이라 현 하드웨어에서 불법이고 mem2 판에서만 돈다.
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

#ifndef NACC
#define NACC 3
#endif
static const grt_ctx AC[3] = {
  { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_INT8 },
  { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_FP32 },
  { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_C1   },
};
#define MMAX 128
#define KMAX 8192
#define NMAX 8192
#define BMAX ((size_t)8388608)   /* K·N 최대 */
#define FREQ 62.5e6
static int BI=4, BJ=4, KCv=16;
static int8_t A[MMAX*KMAX]  __attribute__((aligned(64)));
static int8_t B[BMAX]       __attribute__((aligned(64)));
static int8_t C[MMAX*NMAX]  __attribute__((aligned(64)));
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
  double roof=(double)MM*KK*NN/256.0/FREQ, t[3]={0,0,0}; int bad=0;
  for(int m=1;m<=NACC;m++){
    run(m); double b=1e30;
    for(int r=0;r<5;r++){ memset(C,0,(size_t)MM*NN);
      double t0=now(); run(m); double d=now()-t0; if(d<b)b=d; bad+=chk(); }
    t[m-1]=b; }
  mark("[%5dx%5d] (%d,%d) Kc=%3d 청크%2d sp%5d | %9.3f %9.3f %9.3f | %5.2f %5.2f %5.2f | %s",
       KK,NN,BI,BJ,KCv,(KK/16+KCv-1)/KCv,KCv*(BI+BJ),
       t[0]*1e3,t[1]*1e3,t[2]*1e3,
       t[0]/roof, t[1]>0?t[1]/(roof/2):0.0, t[2]>0?t[2]/(roof/3):0.0, bad?"FAIL":"PASS"); }

int main(int argc,char**argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/nsplit.log","w");
  if(!g){perror("로그");exit(1);}
  if(mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof(cs),&cs); sched_yield();
  for(int a=0;a<NACC;a++) grt_flush_ctx(&AC[a]);

  mark("=== E360: K·N·청크·블록 고정, K:N 분할만 변경 (M=128, 가속기 %d개, best-of-5) ===",NACC);
  mark("한 세트 안에서 MAC·A트래픽·B트래픽·청크·블록이 전부 동일. 다른 것은 C(총 바이트의 2.3%%)뿐.");
  mark("H0: 세 점이 3%% 안 (바이트가 전부) | H1: 스프레드 10%% 초과 (모양이 독립으로 문다)");
  mark("%13s %5s %6s %4s %6s | %9s %9s %9s | %17s | %s",
       "형상","블록","Kc","청크","sp","m=1","m=2","m=3","η(1) η(2) η(3)","정확성");
  MM=128;
  const size_t KN[2]={4194304ull, 8388608ull};
  const int KS[4]={1024,2048,4096,8192};
  for(int s=0;s<2;s++){
    mark("--- 세트 %d: K·N = %.2fM, 루프라인 %.2f ms ---",
         s+1, KN[s]/1e6, (double)MM*KN[s]/256.0/FREQ*1e3);
    for(int bi=0; bi<2; bi++){
      BI = bi?8:4; BJ = 4;
      if ((MM/16) % BI) continue;
      // 청크 수를 두 번째 축으로 둔다. 청크를 고정하면 Kc = Ktil/청크 이므로 K와 함께
      // 움직여 K:N 분할과 교락된다. 청크를 바꾸면 같은 (K,N)에서 Kc만 변하므로,
      // 효과가 N을 따라가는지 Kc를 따라가는지 갈린다.
      const int CH[3]={2,4,8};
      for(int c=0;c<3;c++){
        for(int k=0;k<4;k++){
          KK=KS[k]; NN=(int)(KN[s]/KK);
          if (NN<512 || NN>NMAX || KK>KMAX) continue;
          if ((NN/16)%BJ) continue;
          int Ktil=KK/16; if (Ktil%CH[c]) continue;
          KCv = Ktil/CH[c];
          if (KCv<4) continue;                // E331: Kc >= 4 타일
          if (KCv*(BI+BJ) > 1024) continue;   // mem2 판에서도 불법이면 건너뛴다
          one(); } } } }
  mark("=== NSPLIT_DONE ===");
  return 0; }
