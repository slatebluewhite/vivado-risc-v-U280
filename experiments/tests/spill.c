// spill.c — L2를 넘는 크기에서도 다중 가속기가 이득인가 (E189).
//
// E188: 순수 DMA에서 작업셋이 L2(512KB)를 넘으면 집계 대역폭이 3~5배 떨어지고,
// 더 중요하게 **가속기를 늘릴수록 나빠진다**(7.83 -> 7.76 -> 4.75 B/cycle).
// 온칩에서는 정반대였다(9.46 -> 13.80 -> 15.33).
//
// E180~E187의 다중 가속기 결론은 **전부 L2 안**에서 나왔다(192³ 세 세트 = 324 KB).
// 실제 문제 크기에서 그 결론이 살아남는지 확인한다.
//
// [측정 전 예측] N=320/384에서는 세 가속기의 이득이 크게 줄고, 어쩌면 1보다 작아진다.
//
// 블록은 (4,4) 고정 — T가 12/16/20/24 모두 4로 나누어떨어지고, E186에서 안정한 모양이다.
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
#define MAXN 512
#define FREQ 50.0e6
#define BLK 4
static int8_t A[3][MAXN*MAXN], B[3][MAXN*MAXN], C[3][MAXN*MAXN] __attribute__((aligned(64)));
static int8_t REF[MAXN*MAXN];
static int SZ, NACC;

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }
static double bytes_one(void){
  int T=SZ/16; double b=0;
  for(int i0=0;i0<T;i0+=BLK) for(int j0=0;j0<T;j0+=BLK)
    b += (double)(BLK*T + T*BLK + BLK*BLK)*256.0;
  return b; }
typedef struct { int i0,j0,done; } blk_cur;
static void blk_step(int a, blk_cur *s){
  if (s->done) return;
  int T=SZ/16;
  grt_loop_ws(&AC[a], BLK,BLK,T, A[a]+(size_t)s->i0*16*SZ, B[a]+(size_t)s->j0*16,
              C[a]+(size_t)s->i0*16*SZ+(size_t)s->j0*16, SZ,SZ,SZ);
  s->j0+=BLK; if(s->j0>=T){ s->j0=0; s->i0+=BLK; if(s->i0>=T) s->done=1; } }
static void run(void){
  for(int a=0;a<NACC;a++) grt_loop_ws_config(&AC[a],(uint64_t)SZ,(uint64_t)SZ,(uint64_t)SZ);
  blk_cur s[3]={{0,0,0},{0,0,0},{0,0,0}}; int left=NACC;
  while(left>0){ left=0; for(int a=0;a<NACC;a++){ blk_step(a,&s[a]); if(!s[a].done) left++; } }
  grt_fence(); }
static void fill(void){
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++){
    int8_t a=(c==r)||(c==(r+1)%SZ), b=(int8_t)((r*3+c*5)%7-3);
    for(int k=0;k<3;k++){ A[k][r*SZ+c]=a; B[k][r*SZ+c]=b; } }
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++)
    REF[r*SZ+c]=(int8_t)(B[0][r*SZ+c]+B[0][((r+1)%SZ)*SZ+c]); }
static int chk(void){ int b=0;
  for(int a=0;a<NACC;a++) for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++)
    if(C[a][r*SZ+c]!=REF[r*SZ+c]) b++; return b; }

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/spill.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);

  mark("=== L2를 넘는 크기에서의 다중 가속기 (3i8f50, 블록 (4,4), best-of-5) ===");
  mark("L2 = 512 KB. 가속기당 작업셋 = 3*N^2 B.");
  /* E189의 DRAM 해석은 틀렸다: 블록 모양이 고정이면 산술 강도가 크기와 무관하게
     8 B/연산사이클로 일정하고(단독 B/cyc이 평평한 것이 증거), 불가피 DRAM 트래픽은
     연산당 768/N으로 크기가 커질수록 **줄어든다**. 방향이 반대다.
     남은 후보는 **재사용 트래픽의 L2 실패**다 — 블록 한 행을 훑으면 B 전체를 다시 읽는데
     세 가속기 합친 B가 3N²이고, 512³이면 768 KB로 L2(512KB)를 넘는다. */
  mark("[가설] 저하의 원인은 재사용 트래픽의 L2 실패. 3N²(세 가속기의 B 합)이 지표다.");
  mark("  3N^2: 192³=108KB  320³=300KB  384³=432KB  448³=588KB  512³=768KB  (L2=512KB)");
  mark("  맞다면 448³ 부근에서 저하가 뚜렷해지고 512³에서 더 떨어져야 한다.");
  mark("%5s %8s %8s %5s | %9s %9s | %8s | %8s | %s",
       "크기","3N^2KB","총KB","개수","시간ms","matmul당","처리량","B/cyc","정확성");
  int Ns[]={192,320,384,448,512};
  double base[8];
  for(unsigned i=0;i<sizeof(Ns)/sizeof(Ns[0]);i++){
    SZ=Ns[i]; fill(); double b1=bytes_one();
    for(NACC=1; NACC<=3; NACC++){
      run(); run();
      double best=1e30; int worst=0;
      for(int t=0;t<5;t++){ for(int a=0;a<3;a++) memset(C[a],0,(size_t)SZ*SZ);
        double t0=now(); run(); double dt=now()-t0; if(dt<best)best=dt;
        int b=chk(); if(b>worst)worst=b; }
      double per = best/NACC;
      if(NACC==1) base[i]=per;
      mark("%4d³ %8d %8d %5d | %9.3f %9.3f | %7.2fx | %8.2f | %s",
           SZ, 3*SZ*SZ/1024, 3*SZ*SZ*NACC/1024, NACC,
           best*1e3, per*1e3, base[i]/per, b1*NACC/(best*FREQ),
           worst?"FAIL":"PASS");
    }
    mark("");
  }
  mark("=== SPILL_DONE ===");
  return 0; }
