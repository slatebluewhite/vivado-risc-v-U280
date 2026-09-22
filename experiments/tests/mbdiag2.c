// mbdiag2.c — 누산기 용량 가설 확인 + 192³에서 다중 가속기 재측정 (E179).
//
// E178/1차 진단: 192³에서 블록을 I*J=64 타일(BLK=8, 누산기 전부)로 끊으면 틀리고,
// BLK=4(16타일, 1/4)로 끊으면 fence 없이 맞으며 **더 빠르다**(0.624 대 0.688 ms).
// 원인은 동기화 누락이 아니라 **누산기 용량**으로 보인다 — 연속 블록이 같은 영역을
// 통째로 덮으면 이전 mvout을 앞지른다.
//
// 여기서 BLK를 훑어 안전 경계를 찾고, 안전한 BLK로 192³에서 가속기 1/2/3개를 다시 잰다
// (E177은 다중 블록이 깨져 128³까지밖에 못 갔다).
//
// spad_id=1/2 팔은 뺐다 — NULL 포인터 규약 없이 넘기면 **머신이 멎는다**(실측).
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
static int8_t A[3][MAXN*MAXN], B[3][MAXN*MAXN], C[3][MAXN*MAXN] __attribute__((aligned(64)));
static int8_t REF[MAXN*MAXN];
static int SZ, BLK, NACC;

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }

typedef struct { int i0, j0, done; } blk_cur;
static void blk_step(int a, blk_cur *s){
  if (s->done) return;
  int T = SZ/16;
  int I = (s->i0+BLK<=T)?BLK:(T-s->i0), J = (s->j0+BLK<=T)?BLK:(T-s->j0);
  grt_loop_ws(&AC[a], I, J, T,
              A[a] + (size_t)s->i0*16*SZ,
              B[a] + (size_t)s->j0*16,
              C[a] + (size_t)s->i0*16*SZ + (size_t)s->j0*16, SZ, SZ, SZ);
  s->j0 += BLK;
  if (s->j0 >= T){ s->j0=0; s->i0 += BLK; if (s->i0 >= T) s->done=1; }
}
static void run_seq(void){
  for(int a=0;a<NACC;a++) grt_loop_ws_config(&AC[a],(uint64_t)SZ,(uint64_t)SZ,(uint64_t)SZ);
  for(int a=0;a<NACC;a++){ blk_cur s={0,0,0}; while(!s.done) blk_step(a,&s); grt_fence(); }
}
static void run_con(void){
  for(int a=0;a<NACC;a++) grt_loop_ws_config(&AC[a],(uint64_t)SZ,(uint64_t)SZ,(uint64_t)SZ);
  blk_cur s[3]={{0,0,0},{0,0,0},{0,0,0}}; int left=NACC;
  while(left>0){ left=0; for(int a=0;a<NACC;a++){ blk_step(a,&s[a]); if(!s[a].done) left++; } }
  grt_fence();
}
static void fill(void){
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++){
    int8_t a=(c==r)||(c==(r+1)%SZ), b=(int8_t)((r*3+c*5)%7-3);
    for(int k=0;k<3;k++){ A[k][r*SZ+c]=a; B[k][r*SZ+c]=b; } }
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++)
    REF[r*SZ+c]=(int8_t)(B[0][r*SZ+c]+B[0][((r+1)%SZ)*SZ+c]); }
static int chk(void){ int b=0;
  for(int a=0;a<NACC;a++) for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++)
    if(C[a][r*SZ+c]!=REF[r*SZ+c]) b++; return b; }
static double trial(void (*fn)(void), int *worst){
  double best=1e30; *worst=0;
  for(int t=0;t<5;t++){ for(int a=0;a<3;a++) memset(C[a],0,(size_t)SZ*SZ);
    double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt;
    int b=chk(); if(b>*worst) *worst=b; }
  return best; }

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/mbdiag2.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  SZ=192; fill(); NACC=1;

  mark("=== 1부: 블록 크기와 정확성 (192³, 가속기 1개, fence 없음) ===");
  mark("%5s %6s %6s %8s | %8s | %s","블록","누산기","블록수","활용%","시간ms","최대불일치");
  for(BLK=3; BLK<=8; BLK++){
    int T=SZ/16, nb=((T+BLK-1)/BLK)*((T+BLK-1)/BLK);
    int worst; double t=trial(run_con,&worst);
    mark("%5d %5d/64 %6d %7.1f%% | %8.3f | %d %s", BLK, BLK*BLK, nb,
         100.0*((double)SZ*SZ*SZ/t)/PEAK, t*1e3, worst, worst?"FAIL":"PASS");
  }

  mark("");
  mark("=== 2부: 안전한 블록으로 192³ 다중 가속기 (BLK=4) ===");
  mark("%5s | %9s %9s | %7s | %7s | %s","개수","순차ms","동시ms","이득","활용%","정확성");
  BLK=4;
  for(NACC=1; NACC<=3; NACC++){
    run_seq(); run_con(); run_seq(); run_con();       /* 웜업 */
    int w1,w2;
    double ts=trial(run_seq,&w1), tc=trial(run_con,&w2);
    mark("%5d | %9.3f %9.3f | %6.2fx | %6.1f%% | %s", NACC, ts*1e3, tc*1e3, ts/tc,
         100.0*((double)SZ*SZ*SZ/tc)/PEAK, (w1||w2)?"FAIL":"PASS");
  }
  mark("=== MBDIAG2_DONE ===");
  return 0; }
