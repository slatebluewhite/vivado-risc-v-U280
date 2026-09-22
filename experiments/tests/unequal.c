// unequal.c — 동일 가속기에 **서로 다른 양의 일**을 주어 결합 지수를 재검증 (E218).
//
// E217은 blf50(INT8 16x16 + FP32 8x8) 한 쌍, 네 점으로 p≈4~5를 얻었다.
// "지수가 가속기 쌍에 의존하는가"를 보려면 다른 쌍이 필요한데 빌드가 3시간이다.
//
// 대신 **동일 가속기에 크기가 다른 matmul을 주면** t_a != t_b가 되어 결합 층이
// 그대로 작동한다. 구조적으로 같다 — 각자 어레이·스크래치패드를 갖고 버스와 발행만 공유.
// 하드웨어가 같으므로 "지수가 하드웨어 쌍의 성질인가, 결합 자체의 성질인가"를 가른다.
//
// w256 보드(동일 INT8 16x16 x3), 블록 (4,4), 루프 FSM.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <math.h>
#include <sys/mman.h>
#include "include/gemmini_rt.h"
static const grt_ctx AC[2] = {
  { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_INT8 },
  { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_FP32 },   // w256에서도 INT8 16x16
};
#define MAXN 320
#define FREQ 50.0e6
#define BLK 4
static int8_t A[2][MAXN*MAXN], B[2][MAXN*MAXN], C[2][MAXN*MAXN] __attribute__((aligned(64)));
static int8_t REF[2][MAXN*MAXN];
static int SZ[2];
static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }
typedef struct { int i0,j0,done; } cur;
static void step(int a, cur *s){
  if (s->done) return;
  int N=SZ[a], T=N/16;
  int I=(s->i0+BLK<=T)?BLK:(T-s->i0), J=(s->j0+BLK<=T)?BLK:(T-s->j0);
  grt_loop_ws(&AC[a], I,J,T, A[a]+(size_t)s->i0*16*N, B[a]+(size_t)s->j0*16,
              C[a]+(size_t)s->i0*16*N+(size_t)s->j0*16, N,N,N);
  s->j0+=BLK; if(s->j0>=T){ s->j0=0; s->i0+=BLK; if(s->i0>=T) s->done=1; } }
static void one(int a){
  grt_loop_ws_config(&AC[a],(uint64_t)SZ[a],(uint64_t)SZ[a],(uint64_t)SZ[a]);
  cur s={0,0,0}; while(!s.done) step(a,&s); grt_fence(); }
static void only0(void){ one(0); }
static void only1(void){ one(1); }
static void both(void){
  for(int a=0;a<2;a++) grt_loop_ws_config(&AC[a],(uint64_t)SZ[a],(uint64_t)SZ[a],(uint64_t)SZ[a]);
  cur s[2]={{0,0,0},{0,0,0}}; int left=2;
  while(left>0){ left=0; for(int a=0;a<2;a++){ step(a,&s[a]); if(!s[a].done) left++; } }
  grt_fence(); }
static void fill(void){
  for(int a=0;a<2;a++){ int N=SZ[a];
    for(int r=0;r<N;r++) for(int c=0;c<N;c++){
      A[a][r*N+c]=(c==r)||(c==(r+1)%N); B[a][r*N+c]=(int8_t)((r*3+c*5)%7-3); }
    for(int r=0;r<N;r++) for(int c=0;c<N;c++)
      REF[a][r*N+c]=(int8_t)(B[a][r*N+c]+B[a][((r+1)%N)*N+c]); } }
static int chk(int a){ int N=SZ[a], b=0;
  for(int r=0;r<N;r++) for(int c=0;c<N;c++) if(C[a][r*N+c]!=REF[a][r*N+c]) b++; return b; }
static double meas(void (*fn)(void), int m, int *w){
  double best=1e30; *w=0;
  for(int t=0;t<3;t++){ for(int a=0;a<2;a++) memset(C[a],0,(size_t)SZ[a]*SZ[a]);
    double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt;
    int b = (m==0)?chk(0):((m==1)?chk(1):chk(0)+chk(1)); if(b>*w)*w=b; }
  return best; }
int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/unequal.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  for(int a=0;a<2;a++) grt_flush_ctx(&AC[a]);
  mark("=== 동일 가속기 + 불균등 작업으로 결합 지수 재검증 (w256, 블록 (4,4)) ===");
  mark("교대 시간은 반드시 max(단독) 이상이어야 한다 — 아니면 오염이다(E217).");
  mark("%9s | %8s %8s %8s | %8s %8s %8s %8s",
       "크기쌍","A ms","B ms","교대ms","p3","p4","p5","max");
  int P[][2]={{256,128},{256,192},{192,128},{320,256},{256,64},{192,192}};
  for(unsigned q=0;q<sizeof(P)/sizeof(P[0]);q++){
    SZ[0]=P[q][0]; SZ[1]=P[q][1]; fill();
    only0(); only1(); both();
    int w0,w1,w2;
    double ta=meas(only0,0,&w0), tb=meas(only1,1,&w1), tm=meas(both,2,&w2);
    char flag[16]=""; if(tm < (ta>tb?ta:tb)) snprintf(flag,sizeof(flag)," <-오염!");
    double p3=pow(pow(ta,3)+pow(tb,3),1/3.0), p4=pow(pow(ta,4)+pow(tb,4),1/4.0);
    double p5=pow(pow(ta,5)+pow(tb,5),1/5.0), mx=(ta>tb?ta:tb);
    mark("%4d/%4d | %8.3f %8.3f %8.3f | %+7.1f%% %+7.1f%% %+7.1f%% %+7.1f%% %s%s",
         SZ[0],SZ[1], ta*1e3,tb*1e3,tm*1e3,
         (p3-tm)/tm*100,(p4-tm)/tm*100,(p5-tm)/tm*100,(mx-tm)/tm*100,
         (w0||w1||w2)?"FAIL":"PASS", flag);
  }
  mark("=== UNEQUAL_DONE ===");
  return 0; }
