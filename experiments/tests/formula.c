// formula.c — 닫힌 공식이 크기마다 다른 최적 블록을 맞추는가 (E192).
//
// E191의 규칙: I*J <= 32, T가 I·J 각각으로 나누어떨어질 것, 그 안에서 1/I + 1/J 최소화.
// 512³ 한 점에서만 확인했으므로 여러 크기에 적용해 본다.
//
// 공식이 고른 최적과 예측 이득(강도비, 가속기 3개 기준):
//   T=12(192³) (6,4) 강도 8.000 vs (4,4) 9.333 -> 1.167배
//   T=16(256³) (8,4) 강도 7.000 vs (4,4) 9.000 -> 1.286배
//   T=20(320³) (5,5) 강도 7.200 vs (4,4) 8.800 -> 1.222배
//   T=24(384³) (8,4) 강도 6.667 vs (4,4) 8.667 -> 1.300배
//   T=28(448³) (7,4) 강도 6.857 vs (4,4) 8.571 -> 1.250배
//   T=32(512³) (8,4) 강도 6.500 vs (4,4) 8.500 -> 1.308배 (E191에서 1.32 실측)
//
// (7,4)는 홀수 변이라 E186의 양봉 위험 구간일 수 있다 — 정확성과 재현성을 함께 본다.
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
static int8_t A[3][MAXN*MAXN], B[3][MAXN*MAXN], C[3][MAXN*MAXN] __attribute__((aligned(64)));
static int8_t REF[MAXN*MAXN];
static int SZ, BI, BJ, NACC;

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
static double meas(int *worst){
  double best=1e30; *worst=0;
  for(int t=0;t<3;t++){ for(int a=0;a<3;a++) memset(C[a],0,(size_t)SZ*SZ);
    double t0=now(); run(); double dt=now()-t0; if(dt<best)best=dt;
    int b=chk(); if(b>*worst)*worst=b; }
  return best; }

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/formula.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);

  mark("=== 닫힌 공식이 고른 블록 대 (4,4), 가속기 3개 (best-of-3) ===");
  mark("규칙: I*J<=32, T가 I·J로 나누어떨어질 것, 1/I+1/J 최소화");
  mark("%5s %3s | %7s %10s | %7s %10s | %8s %8s | %s",
       "크기","T","(4,4)","시간ms","최적","시간ms","예측배","실측배","정확성");
  int cfg[][4] = { {192,12,6,4}, {256,16,8,4}, {320,20,5,5},
                   {384,24,8,4}, {448,28,7,4}, {512,32,8,4} };
  double pred[] = {1.167, 1.286, 1.222, 1.300, 1.250, 1.308};
  NACC=3;
  for(unsigned q=0;q<sizeof(cfg)/sizeof(cfg[0]);q++){
    SZ=cfg[q][0]; int T=cfg[q][1]; fill();
    BI=4; BJ=4; run(); int w1; double t44=meas(&w1);
    BI=cfg[q][2]; BJ=cfg[q][3]; run(); int w2; double topt=meas(&w2);
    mark("%4d³ %3d | %7s %10.3f | (%d,%d)%3s %8.3f | %8.3f %8.3f | %s",
         SZ, T, "(4,4)", t44*1e3, BI,BJ,"", topt*1e3, pred[q], t44/topt,
         (w1||w2)?"FAIL":"PASS");
    if(w1||w2) mark("     불일치: (4,4)=%d 최적=%d", w1, w2);
  }
  mark("=== FORMULA_DONE ===");
  return 0; }
