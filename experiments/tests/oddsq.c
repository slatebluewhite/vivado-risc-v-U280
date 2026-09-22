// oddsq.c — 홀수x홀수 블록이 계열로 손해인가 (E193).
//
// E192: T=20에서 공식이 고른 (5,5)가 예측 1.222 대비 1.050으로 크게 빗나갔다.
// E182에서도 (3,3)이 1.331로 (4,3)·(4,4)의 1.121보다 나빴다. 둘 다 홀수x홀수다.
// 반면 홀수x짝수는 멀쩡했다 — (7,4)는 예측을 0.1%로 맞췄고 (4,3)·(6,3)·(3,6)도 정상.
//
// [측정 전 예측]
//   홀수x홀수가 계열로 손해라면, T=20에서 강도가 더 나쁜 (5,4)[8.0]가
//   강도가 좋은 (5,5)[7.2]를 **이기거나 비슷해야** 한다.
//   강도만으로는 (5,5)가 1.11배 앞서야 하므로, 안 그러면 홀수x홀수 페널티가 실재한다.
//   T=12의 (3,3)[강도 9.33]과 (4,3)[9.33, 동일]도 같이 본다 — 강도가 같으므로
//   차이가 나면 그건 순수하게 모양의 효과다.
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
#define MAXN 320
#define FREQ 50.0e6
static int8_t A[3][MAXN*MAXN], B[3][MAXN*MAXN], C[3][MAXN*MAXN] __attribute__((aligned(64)));
static int8_t REF[MAXN*MAXN];
static int SZ, BI, BJ, NACC=3;

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }
typedef struct { int i0,j0,done; } blk_cur;
static void blk_step(int a, blk_cur *s){
  if (s->done) return;
  int T=SZ/16;
  grt_loop_ws(&AC[a], BI,BJ,T, A[a]+(size_t)s->i0*16*SZ, B[a]+(size_t)s->j0*16,
              C[a]+(size_t)s->i0*16*SZ+(size_t)s->j0*16, SZ,SZ,SZ);
  s->j0+=BJ; if(s->j0>=T){ s->j0=0; s->i0+=BI; if(s->i0>=T) s->done=1; } }
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

static void sweep(int sz, int pairs[][2], int n){
  SZ=sz; fill(); int T=SZ/16;
  mark("--- %d³ (T=%d), 가속기 3개 ---", SZ, T);
  mark("%3s %3s %5s %7s | %9s %9s | %s", "I","J","I*J","강도","시간ms","강도예측","정확성");
  double t0ref=0, in0=0;
  for(int q=0;q<n;q++){
    BI=pairs[q][0]; BJ=pairs[q][1];
    double inten=16.0*(1.0/BI+1.0/BJ+1.0/T);
    run();
    double best=1e30; int worst=0;
    for(int t=0;t<3;t++){ for(int a=0;a<3;a++) memset(C[a],0,(size_t)SZ*SZ);
      double tt=now(); run(); double dt=now()-tt; if(dt<best)best=dt;
      int b=chk(); if(b>worst)worst=b; }
    if(q==0){ t0ref=best; in0=inten; }
    mark("%3d %3d %5d %7.2f | %9.3f %9.3f | %s   %s",
         BI,BJ,BI*BJ,inten, best*1e3, t0ref*in0/inten*1e3,
         worst?"FAIL":"PASS", (BI%2 && BJ%2)?"<- 홀수x홀수":"");
  }
  mark(""); }

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/oddsq.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  mark("=== 홀수x홀수 블록이 계열로 손해인가 (best-of-3) ===");
  mark("강도예측 = 첫 줄의 시간을 강도비로 환산한 값. 실측이 이보다 크면 모양 손해다.");
  mark("");
  int p20[][2]={{4,4},{5,4},{4,5},{5,5},{10,2},{2,10}};
  sweep(320, p20, 6);
  int p12[][2]={{4,4},{4,3},{3,4},{3,3},{6,4},{6,2}};
  sweep(192, p12, 6);
  mark("=== ODDSQ_DONE ===");
  return 0; }
