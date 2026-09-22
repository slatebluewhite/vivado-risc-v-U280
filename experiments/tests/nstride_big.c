// nstride.c — E312: N=1024의 나쁜 점은 일량인가 메모리 배치인가.
// 사전 등록: experiments/model/PREREG_E312.txt
//
// 지금까지 N은 일량과 행 스트라이드를 동시에 정했다. grt_block_ksplit은 스트라이드를
// 별도 인자로 받으므로 분리할 수 있다. 일량 N=1024를 고정하고 LD만 바꾼다 —
// 명령 수·블록 분해·산술 강도가 전부 같고 메모리 배치만 다르다.
//   H_layout: LD=1024만 산포 크다 -> 스트라이드 패딩이 규칙 (추가 연산 0!)
//   H_work  : LD를 바꿔도 전부 크다 -> 배치 무관, 분해가 원인
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
static int8_t C[MMAX*6144] __attribute__((aligned(64)));
static int8_t REF[MMAX*6144];
static int MM,KK,NN;
static int LD;      /* B·C의 행 스트라이드 (일량 NN과 분리) */
typedef struct { int jb,i0,step,done; } iter;
static void it_step(int a, iter *s){
  if(s->done) return;
  int TI=MM/16, Ntil=NN/16;
  int j0=s->jb*BJ; int J=(j0+BJ<=Ntil)?BJ:(Ntil-j0);
  int I=(s->i0+BI<=TI)?BI:(TI-s->i0);
  grt_block_ksplit(&AC[a], I,J,KK/16,KCv,
                   A+(size_t)s->i0*16*KK, B+(size_t)j0*16,
                   C+(size_t)s->i0*16*LD+(size_t)j0*16, KK,LD,LD, 1);
  s->i0 += BI;
  if(s->i0>=TI){ s->i0=0; s->jb+=s->step; if(s->jb*BJ>=Ntil) s->done=1; } }
static void run(int m){
  iter s[3];
  for(int a=0;a<m;a++){ grt_loop_ws_config(&AC[a], KK,LD,LD);
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
  memset(B,0,(size_t)KK*LD);
  for(int r=0;r<KK;r++) for(int c=0;c<NN;c++) B[(size_t)r*LD+c]=(int8_t)((r*3+c*5)%7-3);
  for(int r=0;r<MM;r++){ int p=r%(KK-1);
    for(int c=0;c<NN;c++)
      REF[(size_t)r*LD+c]=(int8_t)(B[(size_t)p*LD+c]+B[(size_t)(p+1)*LD+c]); } }
static int chk(void){ int b=0;
  for(int r=0;r<MM;r++) for(int c=0;c<NN;c++)
    if(C[(size_t)r*LD+c]!=REF[(size_t)r*LD+c]) b++; return b; }
static void one(void){
  fill();
  double roof=(double)MM*KK*NN/256.0/FREQ, t[3]; int bad=0;
  for(int m=1;m<=3;m++){
    run(m); double b=1e30;
    for(int r=0;r<5;r++){ memset(C,0,(size_t)MM*LD);
      double t0=now(); run(m); double d=now()-t0; if(d<b)b=d; bad+=chk(); }
    t[m-1]=b; }
  mark("[%4dx%4d] LD=%4d (%d,%d) Kc=%2d 청크%3d nj=%2d 타일%4d | %9.3f %9.3f %9.3f | %5.2f %5.2f | %s",
       KK,NN,LD,BI,BJ,KCv,(KK/16+KCv-1)/KCv,(NN/16+BJ-1)/BJ,KCv*(BI+BJ),
       t[0]*1e3,t[1]*1e3,t[2]*1e3, t[1]/(roof/2),t[2]/(roof/3), bad?"FAIL":"PASS"); }
int main(int argc,char**argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/nstride_big.log","w");
  if(!g){perror("로그");exit(1);}
  if(mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof(cs),&cs); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  mark("=== E312b [C·REF를 6144폭으로: 배치만 다름] 일량 고정(N=1024), 스트라이드만 변경 (K=1024, (8,4), Kc=32, M=128) ===");
  mark("%16s %6s %4s %5s | %9s %9s %9s | %11s | %s",
       "형상","블록","Kc","청크","m=1","m=2","m=3","η(2) η(3)","정확성");
  MM=128; KK=1024; NN=1024; BI=8; BJ=4; KCv=32;
  int LDS[]={1024,1040,1056,1088,1152,1280};
  for(unsigned h=0;h<6;h++){ LD=LDS[h]; one(); }
  mark("=== NSTRIDEBIG_DONE ===");
  return 0; }
