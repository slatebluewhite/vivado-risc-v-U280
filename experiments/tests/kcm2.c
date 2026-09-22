// kcm2.c — E414: E413에서 (16,4)이 [1536x6144]만 이겼다. N/K를 1,2,4,8로 훑어
// 이상치인지 추세인지 가른다. M=256, (8,8) 대 (16,4)만.
// 가설: (16,4)은 B를 **한 번만** 읽고(TI/I=1) A를 4배 더 읽는다. A가 L2에 들면 그 대가가
// 싸므로, B가 클수록(N이 클수록) (16,4)이 유리하다.  A = 256*K 바이트.
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
#define MM   256
#define KMAX 3072
#define NMAX 6144
#define BMAX ((size_t)1536*6144)
#define FREQ 62.5e6
static int BI=8, BJ=8, KCv=16, KK, NN;
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
    if(C[(size_t)r*NN+c]!=REF[(size_t)r*NN+c]) b++;
  return b; }

static void one(void){
  double t[NACC]; int bad=0;
  for(int mi=0;mi<2;mi++){
    int m = mi ? 3 : 1;
    run(m); double b=1e30;
    for(int r=0;r<3;r++){ memset(C,0,(size_t)MM*NN);
      double t0=now(); run(m); double d=now()-t0; if(d<b)b=d; bad+=chk(); }
    t[mi]=b; }
  int ch=(KK/16+KCv-1)/KCv;
  mark("[%5dx%5d] (%d,%d) Kc=%3d 청크%2d sp%5d | m=1 %9.3f  m=3 %9.3f | %5.2f배 | %s",
       KK,NN,BI,BJ,KCv,ch,KCv*(BI+BJ), t[0]*1e3,t[1]*1e3, t[0]/t[1],
       bad?"FAIL":"PASS"); }

int main(int argc,char**argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/kcm2.log","w");
  if(!g){perror("로그");exit(1);}
  if(mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof(cs),&cs); sched_yield();
  for(int a=0;a<NACC;a++) grt_flush_ctx(&AC[a]);
  mark("=== E414: M=256, (8,8) 대 (16,4)을 N/K로 훑기 (best-of-3) ===");

  struct { int K,N; int I,J,kc; } C4[] = {
    { 3072, 3072,  8,8,  48},
    { 3072, 3072, 16,4,  48},
    { 1536, 3072,  8,8,  24},
    { 1536, 3072, 16,4,  24},
    { 1536, 6144,  8,8,  24},
    { 1536, 6144, 16,4,  24},
    {  768, 6144,  8,8,  12},
    {  768, 6144, 16,4,  12},
  };
  int lastK=0,lastN=0;
  for(unsigned i=0;i<sizeof(C4)/sizeof(C4[0]);i++){
    if(C4[i].K!=lastK || C4[i].N!=lastN){
      KK=C4[i].K; NN=C4[i].N; fill(); lastK=KK; lastN=NN;
      mark("--- [%dx%d] Ktil=%d ---", KK, NN, KK/16); }
    BI=C4[i].I; BJ=C4[i].J; KCv=C4[i].kc; one(); }
  mark("=== E414_DONE ===");
  return 0; }
