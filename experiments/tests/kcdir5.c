// kcdir5.c — E408: 임계 구간을 실측 교차점으로 좁힌다. 사전 등록: PREREG_E408.txt
// Ktil=64 고정, N만 2304/2560/2816/3072 (K*N = 2.25/2.50/2.75/3.00 MB).
// Kc in {8,16,32,64} = 청크 {8,4,2,1}. [1024x2816]은 Llama gate/up의 실제 형상이다.
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
#define KMAX 3072
#define NMAX 3072
#define BMAX ((size_t)KMAX*NMAX)
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
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/kcdir5.log","w");
  if(!g){perror("로그");exit(1);}
  if(mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof(cs),&cs); sched_yield();
  for(int a=0;a<NACC;a++) grt_flush_ctx(&AC[a]);
  mark("=== E408: Ktil=64 고정, K*N 2.25->3.00 MB 교차점 (블록 (8,8), best-of-3) ===");

  static const int KCs1[]={8,16,32,64,0};
  static const int KCs2[]={8,16,32,64,0};
  static const int KCs3[]={8,16,32,64,0};
  static const int KCs4[]={8,16,32,64,0};
  struct { int K,N; const int *kcs; const char *nm; } S[] = {
    { 1024,2304, KCs1, "2.25 MB" },
    { 1024,2560, KCs2, "2.50 MB" },
    { 1024,2816, KCs3, "2.75 MB (Llama gate/up)" },
    { 1024,3072, KCs4, "3.00 MB (E406 기확정)" },
  };
  for(unsigned i=0;i<sizeof(S)/sizeof(S[0]);i++){
    KK=S[i].K; NN=S[i].N; fill();
    mark("--- %s [%dx%d] Ktil=%d ---", S[i].nm, KK, NN, KK/16);
    for(const int *p=S[i].kcs; *p; p++){ KCv=*p; one(); } }
  mark("=== E408_DONE ===");
  return 0; }
