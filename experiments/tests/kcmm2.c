// kcmm2.c — E418: (16,4) 조항이 어디서 끝나는가. 사전 등록: PREREG_E418.txt
// E417이 M=512까지 확인했고 이득이 M과 함께 줄었다. M=1024까지 밀어 상한을 정한다.
// (구 헤더) E417: (16,4) 조항의 M 상한.
// M을 런타임 변수로 바꿔 M=256(대조)과 M=512를 같은 이진·같은 회차에서 잰다.
// 기제(A=M*K가 L2에 들어야 이득)가 맞다면 M=512에서 [768x3072]는 이기고
// [1536x6144]는 (A=768KB로 L2 밖이라) 이득이 사라져야 한다.
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
#define MMAX 1024
static int MMv = 256;
#define KMAX 1536
#define NMAX 6144
#define BMAX ((size_t)1536*6144)
#define FREQ 62.5e6
static int BI=8, BJ=8, KCv=16, KK, NN;
static int8_t A[MMAX*KMAX]  __attribute__((aligned(64)));
static int8_t B[BMAX]     __attribute__((aligned(64)));
static int8_t C[MMAX*NMAX]  __attribute__((aligned(64)));
static int8_t REF[MMAX*NMAX];

typedef struct { int jb,i0,step,done; } iter;
static void it_step(int a, iter *s){
  if(s->done) return;
  int TI=MMv/16, Ntil=NN/16;
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
  memset(A,0,(size_t)MMv*KK);
  for(int r=0;r<MMv;r++){ int p=r%(KK-1); A[(size_t)r*KK+p]=1; A[(size_t)r*KK+p+1]=1; }
  for(int r=0;r<KK;r++) for(int c=0;c<NN;c++) B[(size_t)r*NN+c]=(int8_t)((r*3+c*5)%7-3);
  for(int r=0;r<MMv;r++){ int p=r%(KK-1);
    for(int c=0;c<NN;c++)
      REF[(size_t)r*NN+c]=(int8_t)(B[(size_t)p*NN+c]+B[(size_t)(p+1)*NN+c]); } }
static int chk(void){ int b=0;
  for(int r=0;r<MMv;r++) for(int c=0;c<NN;c++)
    if(C[(size_t)r*NN+c]!=REF[(size_t)r*NN+c]) b++;
  return b; }

static void one(void){
  double t[NACC]; int bad=0;
  for(int mi=0;mi<2;mi++){
    int m = mi ? 3 : 1;
    run(m); double b=1e30;
    for(int r=0;r<3;r++){ memset(C,0,(size_t)MMv*NN);
      double t0=now(); run(m); double d=now()-t0; if(d<b)b=d; bad+=chk(); }
    t[mi]=b; }
  int ch=(KK/16+KCv-1)/KCv;
  mark("M=%3d [%5dx%5d] (%d,%d) Kc=%3d 청크%2d sp%5d | m=1 %9.3f  m=3 %9.3f | %5.2f배 | %s",
       MMv,KK,NN,BI,BJ,KCv,ch,KCv*(BI+BJ), t[0]*1e3,t[1]*1e3, t[0]/t[1],
       bad?"FAIL":"PASS"); }

int main(int argc,char**argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/kcmm2.log","w");
  if(!g){perror("로그");exit(1);}
  if(mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof(cs),&cs); sched_yield();
  for(int a=0;a<NACC;a++) grt_flush_ctx(&AC[a]);
  mark("=== E418: (16,4) 조항의 M 상한 — M=1024까지 ===");

  struct { int M,K,N; int I,J,kc; } C4[] = {
    {  256,  768, 3072,  8,8,  24},
    {  256,  768, 3072, 16,4,  24},
    {  256, 1536, 6144,  8,8,  24},
    {  256, 1536, 6144, 16,4,  24},
    {  512,  768, 3072,  8,8,  24},
    {  512,  768, 3072, 16,4,  24},
    {  512, 1536, 6144,  8,8,  24},
    {  512, 1536, 6144, 16,4,  24},
    { 1024,  768, 3072,  8,8,  24},
    { 1024,  768, 3072, 16,4,  24},
    { 1024, 1536, 6144,  8,8,  24},
    { 1024, 1536, 6144, 16,4,  24},
  };
  int lastK=0,lastN=0;
  for(unsigned i=0;i<sizeof(C4)/sizeof(C4[0]);i++){
    if(C4[i].K!=lastK || C4[i].N!=lastN || C4[i].M!=MMv){
      MMv=C4[i].M; KK=C4[i].K; NN=C4[i].N; fill(); lastK=KK; lastN=NN;
      mark("--- M=%d [%dx%d] Ktil=%d  A=%dKB ---", MMv, KK, NN, KK/16, MMv*KK/1024); }
    BI=C4[i].I; BJ=C4[i].J; KCv=C4[i].kc; one(); }
  mark("=== E418_DONE ===");
  return 0; }
