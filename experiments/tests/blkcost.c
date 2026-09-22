// blkcost.c — E180 모델의 나머지 항: 블록당 고정 비용 (E181).
//
// E180의 예측식 "쓸모 있는 가속기 수 = 15.0 / (가속기 하나의 B/cycle)"은 160³ 이상에서
// 1% 이내로 맞고 그 아래에서는 과대예측한다. 작은 행렬에서는 대역폭이 아니라
// **블록당 고정 오버헤드**가 먼저 닿기 때문이라고 적었지만, 그건 추정이었다.
//
// 여기서 그 항을 잰다. 같은 크기에서 블록을 잘게 쪼개면 (a) 이동 바이트가 늘고
// (b) 블록 수가 는다. 둘 다 정확히 셀 수 있으므로
//     시간 = 바이트/대역폭 + 블록수 x 고정비용
// 을 맞춰 대역폭과 고정비용을 동시에 뽑는다.
//
// 블록은 누산기 절반 규칙(I*J <= 32)을 지켜 BLK <= 5만 쓴다 (E179).
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
#define MAXN 192
#define FREQ 50.0e6
static int8_t A[MAXN*MAXN], B[MAXN*MAXN], C[MAXN*MAXN] __attribute__((aligned(64)));
static int8_t REF[MAXN*MAXN];
static int SZ, BLK;

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }

static void stats(double *bytes, int *nblk){
  int T=SZ/16; double b=0; int n=0;
  for(int i0=0;i0<T;i0+=BLK) for(int j0=0;j0<T;j0+=BLK){
    int I=(i0+BLK<=T)?BLK:(T-i0), J=(j0+BLK<=T)?BLK:(T-j0);
    b += (double)(I*T + T*J + I*J)*256.0; n++; }
  *bytes=b; *nblk=n; }

static void run(void){
  int T=SZ/16;
  grt_loop_ws_config(&AC1,(uint64_t)SZ,(uint64_t)SZ,(uint64_t)SZ);
  for(int i0=0;i0<T;i0+=BLK) for(int j0=0;j0<T;j0+=BLK){
    int I=(i0+BLK<=T)?BLK:(T-i0), J=(j0+BLK<=T)?BLK:(T-j0);
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
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/blkcost.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  grt_flush_ctx(&AC1);

  mark("=== 블록당 고정 비용 뽑기 (3i8f50, 가속기 1개, best-of-5) ===");
  mark("시간 = 바이트/대역폭 + 블록수 x 고정비용 을 맞춘다");
  mark("%5s %4s %6s %9s | %9s %9s | %s",
       "크기","블록","블록수","바이트","시간us","cycle","정확성");
  int Ns[]={96,128,160,192};
  for(unsigned i=0;i<sizeof(Ns)/sizeof(Ns[0]);i++){
    SZ=Ns[i]; fill();
    for(BLK=2; BLK<=5; BLK++){
      double by; int nb; stats(&by,&nb);
      run(); run();                                  /* 웜업 */
      double best=1e30; int worst=0;
      for(int t=0;t<5;t++){ memset(C,0,(size_t)SZ*SZ);
        double t0=now(); run(); double dt=now()-t0; if(dt<best)best=dt;
        int b=chk(); if(b>worst) worst=b; }
      mark("%4d³ %4d %6d %9.0f | %9.2f %9.0f | %s",
           SZ, BLK, nb, by, best*1e6, best*FREQ, worst?"FAIL":"PASS");
    }
  }
  mark("=== BLKCOST_DONE ===");
  return 0; }
