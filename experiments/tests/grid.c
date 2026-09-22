// grid.c — η(m)은 워킹셋 하나의 함수인가, (K,N) 두 축의 함수인가 (E243).
//
// E241은 십자(K=768 행, N=768 열)만 훑었고, E242에서 그 십자를 벗어난 두 형상에서
// 모델이 틀렸다. 또 ws가 같은 전치 쌍의 η(3)이 두 번 다 21% 어긋났다
// (1.53 대 1.86 @1.47MB, 2.23 대 2.70 @2.85MB) — 두 번 다 K가 큰 쪽이 나쁘다.
//
// 내부를 3x3 격자로 채운다. N은 96(=BJ*16)의 배수로 골라 부분 블록을 없앴고,
// K는 256(=Kc*16)의 배수라 K 분할도 딱 떨어진다. 즉 블록킹 잡음이 없는 격자다.
//   K = 512 / 1024 / 2048,  N = 480 / 960 / 1920
// 격자 안에 ws가 거의 같은 세 점이 있다 — 이것이 판정자다:
//   (2048,480) 1.31MB / (1024,960) 1.24MB / (512,1920) 1.29MB
// ws 하나로 되면 셋의 η가 같아야 하고, 두 축이면 K 큰 쪽이 나빠야 한다.
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
#define KMAX 2048
#define NMAX 1920
#define FREQ 50.0e6
#define BI 4
#define BJ 6
#define KC 16

static int8_t A[M*KMAX]      __attribute__((aligned(64)));
static int8_t B[KMAX*NMAX]   __attribute__((aligned(64)));
static int8_t C[M*NMAX]      __attribute__((aligned(64)));
static int8_t REF[M*NMAX];
static int KK,NN;

typedef struct { int jb,i0,step,done; } iter;
static void it_step(int a, iter *s){
  if(s->done) return;
  int TI=M/16, Ntil=NN/16;
  int j0=s->jb*BJ; int J=(j0+BJ<=Ntil)?BJ:(Ntil-j0);
  int I=(s->i0+BI<=TI)?BI:(TI-s->i0);
  grt_block_ksplit(&AC[a], I,J,KK/16,KC,
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
  memset(A,0,(size_t)M*KK);
  for(int r=0;r<M;r++){ A[(size_t)r*KK+r]=1; A[(size_t)r*KK+r+1]=1; }
  for(int r=0;r<KK;r++) for(int c=0;c<NN;c++) B[(size_t)r*NN+c]=(int8_t)((r*3+c*5)%7-3);
  for(int r=0;r<M;r++) for(int c=0;c<NN;c++)
    REF[(size_t)r*NN+c]=(int8_t)(B[(size_t)r*NN+c]+B[(size_t)(r+1)*NN+c]); }
static int chk(void){ int b=0;
  for(int r=0;r<M;r++) for(int c=0;c<NN;c++)
    if(C[(size_t)r*NN+c]!=REF[(size_t)r*NN+c]) b++; return b; }

int main(int argc,char**argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/grid.log","w");
  if(!g){perror("로그");exit(1);}
  if(mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof(cs),&cs); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  mark("=== η의 2차원 격자 (3i8f50, M=128, (4,6), Kc=16, 부분블록 없음, best-of-3) ===");
  mark("판정자: ws 1.24~1.31MB인 세 점 (2048,480)/(1024,960)/(512,1920)");
  mark("%11s %5s %7s %8s | %8s %8s %8s | %17s | %s",
       "형상","nj","ws MB","루프라인","m=1","m=2","m=3","η(1) η(2) η(3)","정확성");
  int Ks[]={512,1024,2048}, Ns[]={480,960,1920};
  for(int a=0;a<3;a++) for(int b=0;b<3;b++){
    KK=Ks[a]; NN=Ns[b]; fill();
    double roof=(double)M*KK*NN/256.0/FREQ;
    double t[3]; int bad=0;
    for(int m=1;m<=3;m++){
      run(m); double bt=1e30;
      for(int r=0;r<3;r++){ memset(C,0,(size_t)M*NN);
        double t0=now(); run(m); double d=now()-t0; if(d<bt)bt=d; bad+=chk(); }
      t[m-1]=bt; }
    double ws=((double)M*KK+(double)KK*NN+(double)M*NN)/1e6;
    mark("[%4dx%4d] %5d %6.2fMB %8.3f | %8.3f %8.3f %8.3f | η %5.2f %5.2f %5.2f | %s",
         KK,NN,(NN/16+BJ-1)/BJ,ws,roof*1e3,t[0]*1e3,t[1]*1e3,t[2]*1e3,
         t[0]/roof,t[1]/(roof/2),t[2]/(roof/3), bad?"FAIL":"PASS");
  }
  mark("=== GRID_DONE ===");
  return 0; }
