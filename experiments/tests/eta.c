// eta.c — 분할 효율 손실 η(m)의 함수 형태를 찾는다 (E241).
//
// E240이 코스트 모델의 남은 미지수를 하나로 좁혔다:
//     η(m) = 실측 시간 / (루프라인/m)
// m=1에서는 여섯 형상 전부 1.05~1.07로 붙어 있고(즉 단일 가속기는 다 안다),
// m=2에서 1.05~1.60, m=3에서 1.29~2.42로 형상마다 벌어진다. 이것만 모른다.
//
// 여섯 점으로 형태를 추측하지 말고 **한 변수씩** 훑는다:
//     훑기 A: K=768 고정, N = 384/768/1536/3072
//     훑기 B: N=768 고정, K = 256/512/1024/2048/3072
// 두 훑기가 만나는 점(768x768)이 서로의 검증이 된다.
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
#define KMAX 3072
#define NMAX 3072
#define BMAX (3072*768)
#define FREQ 50.0e6
#define BI 4
#define BJ 6
#define KC 16

static int8_t A[M*KMAX] __attribute__((aligned(64)));
static int8_t B[BMAX]   __attribute__((aligned(64)));
static int8_t C[M*NMAX] __attribute__((aligned(64)));
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

static void one(const char *tag){
  fill();
  double roof = (double)M*KK*NN/256.0/FREQ;      /* 초 */
  double t[3]; int bad=0;
  for(int m=1;m<=3;m++){
    run(m); double b=1e30;
    for(int r=0;r<3;r++){ memset(C,0,(size_t)M*NN);
      double t0=now(); run(m); double d=now()-t0; if(d<b)b=d; bad+=chk(); }
    t[m-1]=b; }
  int nj=(NN/16+BJ-1)/BJ;
  mark("%-3s [%4dx%4d] nj=%2d 루프라인%8.3f | %8.3f %8.3f %8.3f | "
       "η %5.2f %5.2f %5.2f | 향상 %5.2f %5.2f | %s",
       tag, KK,NN,nj, roof*1e3, t[0]*1e3,t[1]*1e3,t[2]*1e3,
       t[0]/roof, t[1]/(roof/2), t[2]/(roof/3), t[0]/t[1], t[0]/t[2],
       bad?"FAIL":"PASS"); }

int main(int argc,char**argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/eta.log","w");
  if(!g){perror("로그");exit(1);}
  if(mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof(cs),&cs); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  mark("=== 분할 효율 손실 η(m) = t_m/(루프라인/m)  (3i8f50, M=128, (4,6), Kc=16, best-of-3) ===");
  mark("η=1이면 완벽. E240: m=1은 어디서나 1.05~1.07, m=2/3만 모른다.");
  mark("%-3s %13s %5s %8s | %8s %8s %8s | %17s | %11s | %s",
       "","형상","nj","루프라인","m=1","m=2","m=3","η(1) η(2) η(3)","2개  3개","정확성");
  int Ns[]={384,768,1536,3072};
  for(unsigned i=0;i<sizeof(Ns)/sizeof(Ns[0]);i++){ KK=768; NN=Ns[i]; one("A"); }
  int Ks[]={256,512,1024,2048,3072};
  for(unsigned i=0;i<sizeof(Ks)/sizeof(Ks[0]);i++){ KK=Ks[i]; NN=768; one("B"); }
  mark("=== ETA_DONE ===");
  return 0; }
