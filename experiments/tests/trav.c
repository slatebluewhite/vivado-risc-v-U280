// trav.c — k의 비대칭은 내 **블록 순회 순서** 때문인가 (E186).
//
// E185: 블록 하나는 I<->J에 대칭인데((2,3)=(3,2)=66.00us) 전체를 덮으면 (3,2)가 34% 느리다.
// 비대칭은 블록을 이어 붙일 때 생긴다. 내 코드는 `for i0 { for j0 }`로 j0가 안쪽이다.
//
// [측정 전 예측]
//   순회 순서가 원인이라면, i0를 안쪽으로 바꿨을 때 k 표가 **전치**되어야 한다 —
//   즉 j-내곽에서 (3,2)가 느렸다면 i-내곽에서는 (2,3)이 느려야 한다.
//   전치되면 비대칭의 정체는 하드웨어가 아니라 순회 순서이고, "블록 모양을 골라라"에
//   "순회 순서도 골라라"가 붙는다. 전치되지 않으면 하드웨어 쪽 성질이다.
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

static void emit(int i0, int j0){
  int T=SZ/16;
  int I=(i0+BI<=T)?BI:(T-i0), J=(j0+BJ<=T)?BJ:(T-j0);
  grt_loop_ws(&AC1, I,J,T, A+(size_t)i0*16*SZ, B+(size_t)j0*16,
              C+(size_t)i0*16*SZ+(size_t)j0*16, SZ,SZ,SZ); }
static void run_j_inner(void){            // for i0 { for j0 }  — 열 방향 진행 (기존)
  int T=SZ/16;
  grt_loop_ws_config(&AC1,(uint64_t)SZ,(uint64_t)SZ,(uint64_t)SZ);
  for(int i0=0;i0<T;i0+=BI) for(int j0=0;j0<T;j0+=BJ) emit(i0,j0);
  grt_fence(); }
static void run_i_inner(void){            // for j0 { for i0 }  — 행 방향 진행
  int T=SZ/16;
  grt_loop_ws_config(&AC1,(uint64_t)SZ,(uint64_t)SZ,(uint64_t)SZ);
  for(int j0=0;j0<T;j0+=BJ) for(int i0=0;i0<T;i0+=BI) emit(i0,j0);
  grt_fence(); }

static void fill(void){
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++){
    A[r*SZ+c]=(c==r)||(c==(r+1)%SZ); B[r*SZ+c]=(int8_t)((r*3+c*5)%7-3); }
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++)
    REF[r*SZ+c]=(int8_t)(B[r*SZ+c]+B[((r+1)%SZ)*SZ+c]); }
static int chk(void){ int b=0;
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++) if(C[r*SZ+c]!=REF[r*SZ+c]) b++; return b; }
static double meas(void (*fn)(void), int *worst){
  double best=1e30; *worst=0;
  for(int t=0;t<5;t++){ memset(C,0,(size_t)SZ*SZ);
    double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt;
    int b=chk(); if(b>*worst)*worst=b; }
  return best; }

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/trav.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  grt_flush_ctx(&AC1); fill();
  double fl=(double)SZ*SZ*SZ/256.0;

  mark("=== 블록 순회 순서가 비대칭의 원인인가 (192³, 가속기 1개, best-of-5) ===");
  mark("[예측] 순회 탓이면 i-내곽에서 k 표가 전치된다: (3,2)가 빨라지고 (2,3)이 느려진다.");
  mark("%3s %3s | %9s %7s | %9s %7s | %7s | %s",
       "I","J","j내곽us","k","i내곽us","k","비","정확성");
  int pairs[][2]={{2,3},{3,2},{2,4},{4,2},{3,4},{4,3},{2,6},{6,2},{4,4},{3,3}};
  for(unsigned q=0;q<sizeof(pairs)/sizeof(pairs[0]);q++){
    BI=pairs[q][0]; BJ=pairs[q][1];
    run_j_inner(); run_i_inner();
    int w1,w2;
    double tj=meas(run_j_inner,&w1);
    double ti=meas(run_i_inner,&w2);
    mark("%3d %3d | %9.2f %7.3f | %9.2f %7.3f | %6.3f | %s",
         BI,BJ, tj*1e6, tj*FREQ/fl, ti*1e6, ti*FREQ/fl, ti/tj,
         (w1||w2)?"FAIL":"PASS");
  }
  mark("=== TRAV_DONE ===");
  return 0; }
