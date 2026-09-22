// roofline2.c — E179의 법칙을 예측 가능한 수치로: 천장은 버스 폭인가 (E180).
//
// E179: "가속기를 더 붙여 얻는 것 = 지금 가속기에 남은 여유". 관찰은 맞지만 예측이 안 된다.
// 병목이 온칩 대역폭이라면 **달성 바이트율이 어떤 천장에서 멈춰야** 하고,
// 3i8f50은 SystemBus가 128비트이므로 그 천장은 **16 B/cycle**이어야 한다.
//
// 블록 구성이 결정론적이므로 이동 바이트를 정확히 셀 수 있다:
//   블록마다 A 타일 I*K개 + B 타일 K*J개 mvin, C 타일 I*J개 mvout. 타일 = 16*16*1 B.
// 크기 x 가속기 수를 훑어 B/cycle이 한 값으로 수렴하는지 본다. 수렴하면 법칙이
// "16 B/cycle에 닿을 때까지 가속기를 늘려라"로 정량화된다.
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
#define MAXN 192
#define FREQ 50.0e6
#define PEAK (16.0*16.0*FREQ)
#define BLK 4                 // I*J = 16 <= 32 (누산기 절반) — E179의 안전 규칙
static int8_t A[3][MAXN*MAXN], B[3][MAXN*MAXN], C[3][MAXN*MAXN] __attribute__((aligned(64)));
static int8_t REF[MAXN*MAXN];
static int SZ, NACC;

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }

// matmul 하나가 실제로 옮기는 바이트 (블록 구성에서 정확히 셈)
static double bytes_one(void){
  int T = SZ/16; double tot = 0;
  for (int i0=0;i0<T;i0+=BLK) for (int j0=0;j0<T;j0+=BLK){
    int I=(i0+BLK<=T)?BLK:(T-i0), J=(j0+BLK<=T)?BLK:(T-j0);
    tot += (double)(I*T + T*J + I*J) * 16.0*16.0*1.0;
  }
  return tot;
}

typedef struct { int i0,j0,done; } blk_cur;
static void blk_step(int a, blk_cur *s){
  if (s->done) return;
  int T=SZ/16;
  int I=(s->i0+BLK<=T)?BLK:(T-s->i0), J=(s->j0+BLK<=T)?BLK:(T-s->j0);
  grt_loop_ws(&AC[a], I,J,T, A[a]+(size_t)s->i0*16*SZ, B[a]+(size_t)s->j0*16,
              C[a]+(size_t)s->i0*16*SZ+(size_t)s->j0*16, SZ,SZ,SZ);
  s->j0+=BLK; if(s->j0>=T){ s->j0=0; s->i0+=BLK; if(s->i0>=T) s->done=1; } }
static void run_con(void){
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
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/roofline2.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);

  mark("=== 달성 바이트율의 천장을 찾는다 (3i8f50, 128비트 버스 = 16 B/cycle, best-of-5) ===");
  mark("%5s %4s | %8s | %7s | %9s | %8s | %s",
       "크기","개수","시간ms","활용%","MB/s","B/cycle","정확성");
  int Ns[]={64,96,128,160,192};
  for(unsigned i=0;i<sizeof(Ns)/sizeof(Ns[0]);i++){
    SZ=Ns[i]; fill(); double b1=bytes_one();
    for(NACC=1; NACC<=3; NACC++){
      run_con(); run_con();                        /* 웜업 */
      double best=1e30; int worst=0;
      for(int t=0;t<5;t++){ for(int a=0;a<3;a++) memset(C[a],0,(size_t)SZ*SZ);
        double t0=now(); run_con(); double dt=now()-t0; if(dt<best)best=dt;
        int b=chk(); if(b>worst) worst=b; }
      double tot = b1*NACC;
      mark("%4d³ %4d | %8.3f | %6.1f%% | %9.1f | %8.2f | %s",
           SZ, NACC, best*1e3,
           100.0*((double)NACC*SZ*SZ*SZ/best)/(PEAK*NACC),
           tot/best/1e6, tot/(best*FREQ), worst?"FAIL":"PASS");
    }
  }
  mark("=== ROOFLINE2_DONE ===");
  return 0; }
