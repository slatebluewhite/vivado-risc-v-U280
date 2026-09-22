// blkshape.c — 블록 크기가 **연산 효율**에도 영향을 주는가, I인가 J인가 (E182).
//
// E181: 192³에서 BLK=3(정사각 3x3 블록)이 BLK=4보다 19% 느리다. 바이트도 블록수도 적은데
// 느리고, 둘 다 대역폭 천장 아래라 대역폭으로는 설명이 안 된다. 즉 **연산 효율**의 차이다.
// (연산 바닥 대비: BLK=4는 1.12배, BLK=3은 1.33배)
//
// 블록을 정사각으로만 쓸 이유가 없으므로 I와 J를 따로 준다. 192³은 T=12라 2·3·4·6으로
// 모두 나누어떨어져 깨끗한 2차원 훑기가 된다. 제약은 누산기 절반 규칙 I*J <= 32 (E179).
//
// WS 데이터플로우에서 B 타일이 **가중치**이고 A가 흘러가므로, J가 가중치 교체 빈도를,
// I가 스트리밍 길이를 정한다. 어느 쪽이 효율을 정하는지 여기서 갈린다.
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

static void stats(double *bytes, int *nblk){
  int T=SZ/16; double b=0; int n=0;
  for(int i0=0;i0<T;i0+=BI) for(int j0=0;j0<T;j0+=BJ){
    int I=(i0+BI<=T)?BI:(T-i0), J=(j0+BJ<=T)?BJ:(T-j0);
    b += (double)(I*T + T*J + I*J)*256.0; n++; }
  *bytes=b; *nblk=n; }
static void run(void){
  int T=SZ/16;
  grt_loop_ws_config(&AC1,(uint64_t)SZ,(uint64_t)SZ,(uint64_t)SZ);
  for(int i0=0;i0<T;i0+=BI) for(int j0=0;j0<T;j0+=BJ){
    int I=(i0+BI<=T)?BI:(T-i0), J=(j0+BJ<=T)?BJ:(T-j0);
    grt_loop_ws(&AC1, I,J,T, A+(size_t)i0*16*SZ, B+(size_t)j0*16,
                C+(size_t)i0*16*SZ+(size_t)j0*16, SZ,SZ,SZ); }
  grt_fence(); }
static void fill(void){
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++){
    A[r*SZ+c]=(c==r)||(c==(r+1)%SZ); B[r*SZ+c]=(int8_t)((r*3+c*5)%7-3); }
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++)
    REF[r*SZ+c]=(int8_t)(B[r*SZ+c]+B[((r+1)%SZ)*SZ+c]); }
static int chk(void){ int b=0;
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++) if(C[r*SZ+c]!=REF[r*SZ+c]) b++; return b; }

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/blkshape.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  grt_flush_ctx(&AC1);
  fill();
  double floor_c = (double)SZ*SZ*SZ/256.0;      // 이론 연산 cycle

  mark("=== 블록 모양 I x J 훑기 (192³ = 12타일, 가속기 1개, best-of-5) ===");
  mark("이론 연산 바닥 = %.0f cycle. 대역폭 천장 15.0 B/cycle.", floor_c);
  mark("%3s %3s %6s %9s | %8s %8s | %8s %8s | %s",
       "I","J","블록수","바이트","시간us","cycle","연산비","B/cyc","정확성");
  int pairs[][2] = {{2,2},{2,3},{3,2},{2,4},{4,2},{3,3},{3,4},{4,3},
                    {4,4},{2,6},{6,2},{3,6},{6,3},{4,6},{6,4},{6,6}};
  for(unsigned q=0;q<sizeof(pairs)/sizeof(pairs[0]);q++){
    BI=pairs[q][0]; BJ=pairs[q][1];
    if (BI*BJ > 32) { mark("%3d %3d  (I*J=%d > 32, 누산기 절반 규칙 위반 — 건너뜀)",
                           BI,BJ,BI*BJ); continue; }
    double by; int nb; stats(&by,&nb);
    run(); run();
    double best=1e30; int worst=0;
    for(int t=0;t<5;t++){ memset(C,0,(size_t)SZ*SZ);
      double t0=now(); run(); double dt=now()-t0; if(dt<best)best=dt;
      int b=chk(); if(b>worst) worst=b; }
    double cyc = best*FREQ;
    mark("%3d %3d %6d %9.0f | %8.2f %8.0f | %8.3f %8.2f | %s",
         BI,BJ,nb,by, best*1e6, cyc, cyc/floor_c, by/cyc, worst?"FAIL":"PASS");
  }
  mark("=== BLKSHAPE_DONE ===");
  return 0; }
