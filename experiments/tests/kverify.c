// kverify.c — E182의 k(I,J) 비대칭은 재현되는가, 그리고 블록 간 효과인가 (E185).
//
// E182(192³, 가속기 1개)는 k가 1.110~1.664로 흩어지고 I·J에 비대칭이라고 보고했다.
// E184에서 그 k를 외삽했다가 예측이 빗나갔으므로, 기제를 찾기 전에 **값 자체를 검증**한다.
// 이 저널에는 재현되지 않은 측정이 세 번 있었다(E171·E174·E178).
//
// 두 가지를 잰다:
//   전체 — E182와 동일하게 행렬 전체를 블록으로 덮는다 (블록 간 효과 포함)
//   단독 — 블록 **하나만** 발행하고 그것만 잰다 (블록 간 효과 제외)
// 둘이 갈리면 비대칭은 블록 사이에서 생기는 것이고, 같이 가면 블록 안의 성질이다.
//
// 실행 FSM은 i가 최내곽, j 중간, k 최외곽이다(LoopMatmul.scala:474-476).
// A는 (k,i)마다, B는 (k,j)마다 적재된다. i 최내곽이면 가중치 교체가 I에 반비례해
// 상각되므로 k ~ 1 + c/I가 기대된다.
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

static const grt_ctx AC1 = { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_INT8 };
#define SZ 192
#define FREQ 50.0e6
static int8_t A[SZ*SZ], B[SZ*SZ], C[SZ*SZ] __attribute__((aligned(64)));
static int8_t REF[SZ*SZ];
static int BI, BJ;

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }

static void run_all(void){                 // 전체를 블록으로 덮는다
  int T=SZ/16;
  grt_loop_ws_config(&AC1,(uint64_t)SZ,(uint64_t)SZ,(uint64_t)SZ);
  for(int i0=0;i0<T;i0+=BI) for(int j0=0;j0<T;j0+=BJ){
    int I=(i0+BI<=T)?BI:(T-i0), J=(j0+BJ<=T)?BJ:(T-j0);
    grt_loop_ws(&AC1, I,J,T, A+(size_t)i0*16*SZ, B+(size_t)j0*16,
                C+(size_t)i0*16*SZ+(size_t)j0*16, SZ,SZ,SZ); }
  grt_fence(); }
static void run_one(void){                 // 블록 하나만
  int T=SZ/16;
  grt_loop_ws_config(&AC1,(uint64_t)SZ,(uint64_t)SZ,(uint64_t)SZ);
  grt_loop_ws(&AC1, BI,BJ,T, A, B, C, SZ,SZ,SZ);
  grt_fence(); }

static void fill(void){
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++){
    A[r*SZ+c]=(c==r)||(c==(r+1)%SZ); B[r*SZ+c]=(int8_t)((r*3+c*5)%7-3); }
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++)
    REF[r*SZ+c]=(int8_t)(B[r*SZ+c]+B[((r+1)%SZ)*SZ+c]); }
static int chk_all(void){ int b=0;
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++) if(C[r*SZ+c]!=REF[r*SZ+c]) b++; return b; }
static int chk_one(void){ int b=0;      // 블록 하나가 덮는 영역만
  for(int r=0;r<BI*16;r++) for(int c=0;c<BJ*16;c++)
    if(C[r*SZ+c]!=REF[r*SZ+c]) b++; return b; }

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/kverify.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  grt_flush_ctx(&AC1); fill();
  int T=SZ/16;

  mark("=== k(I,J) 재현 확인 + 블록 단독 (192³, 가속기 1개, best-of-5) ===");
  mark("전체 연산 바닥 %d cycle. 단독은 I*J*K 타일이므로 바닥 = I*J*K*16 cycle.",
       SZ*SZ*SZ/256);
  mark("%3s %3s | %9s %8s %8s | %9s %8s %8s | %s",
       "I","J","전체us","전체k","E182k","단독us","단독k","블록수","정확성");
  int pairs[][2]={{2,2},{2,3},{3,2},{2,4},{4,2},{3,3},{3,4},{4,3},
                  {4,4},{2,6},{6,2},{3,6},{6,3},{4,6},{6,4}};
  double e182[]={1.179,1.223,1.664,1.497,1.208,1.331,1.450,1.121,
                 1.121,1.168,1.110,1.125,1.114,1.139,1.143};
  for(unsigned q=0;q<sizeof(pairs)/sizeof(pairs[0]);q++){
    BI=pairs[q][0]; BJ=pairs[q][1];
    double fall=(double)SZ*SZ*SZ/256.0, fone=(double)BI*BJ*T*16.0;
    /* 전체 */
    run_all(); run_all();
    double ba=1e30; int wa=0;
    for(int t=0;t<5;t++){ memset(C,0,(size_t)SZ*SZ);
      double t0=now(); run_all(); double dt=now()-t0; if(dt<ba)ba=dt;
      int b=chk_all(); if(b>wa)wa=b; }
    /* 단독 */
    run_one(); run_one();
    double bo=1e30; int wo=0;
    for(int t=0;t<5;t++){ memset(C,0,(size_t)SZ*SZ);
      double t0=now(); run_one(); double dt=now()-t0; if(dt<bo)bo=dt;
      int b=chk_one(); if(b>wo)wo=b; }
    mark("%3d %3d | %9.2f %8.3f %8.3f | %9.2f %8.3f %8d | %s",
         BI,BJ, ba*1e6, ba*FREQ/fall, e182[q],
         bo*1e6, bo*FREQ/fone, (T/BI)*(T/BJ), (wa||wo)?"FAIL":"PASS");
  }
  mark("=== KVERIFY_DONE ===");
  return 0; }
