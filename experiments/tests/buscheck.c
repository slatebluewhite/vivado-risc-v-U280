// buscheck.c — 지금 올라간 비트스트림이 정말 256비트 버스인가 (E249 검증).
// E194가 정방 192³ (4,4)에서 두 버스를 뚜렷이 갈랐다:
//   128비트: m=1/2/3 = 0.622 / 0.714 / 1.032 ms
//   256비트: 0.620 / 0.630 / 0.656 ms      <- m=3이 1.57배 빠르다
// 같은 조건을 재서 어느 쪽인지 판정한다.
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
#define N 192
#define BLK 4
#define FREQ 50.0e6
// E194와 같은 구성: 가속기마다 **독립** 192³ matmul (J 분할이 아니다)
static int8_t A[3][N*N], B[3][N*N], C[3][N*N] __attribute__((aligned(64)));
static int8_t REF[N*N];
static FILE *g;
static void mark(const char *f,...){ if(!g)return; va_list ap; va_start(ap,f);
  vfprintf(g,f,ap); va_end(ap); fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }
typedef struct { int i0,j0,done; } cur;
static void step(int a, cur *s){
  if(s->done) return;
  int T=N/16;
  int I=(s->i0+BLK<=T)?BLK:(T-s->i0), J=(s->j0+BLK<=T)?BLK:(T-s->j0);
  grt_loop_ws(&AC[a], I,J,T, A[a]+(size_t)s->i0*16*N, B[a]+(size_t)s->j0*16,
              C[a]+(size_t)s->i0*16*N+(size_t)s->j0*16, N,N,N);
  s->j0+=BLK; if(s->j0>=T){ s->j0=0; s->i0+=BLK; if(s->i0>=T) s->done=1; } }
static void run(int m){
  for(int a=0;a<m;a++) grt_loop_ws_config(&AC[a], N,N,N);
  cur s[3]={{0,0,0},{0,0,0},{0,0,0}}; int left;
  do{ left=0; for(int a=0;a<m;a++){ step(a,&s[a]); if(!s[a].done) left++; } }while(left);
  grt_fence(); }
int main(int argc,char**argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/buscheck.log","w");
  if(!g){perror("로그");exit(1);}
  if(mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof(cs),&cs); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  for(int r=0;r<N;r++) for(int c=0;c<N;c++){
    int8_t av=(c==r)||(c==(r+1)%N), bv=(int8_t)((r*3+c*5)%7-3);
    for(int k=0;k<3;k++){ A[k][r*N+c]=av; B[k][r*N+c]=bv; } }
  for(int r=0;r<N;r++) for(int c=0;c<N;c++)
    REF[r*N+c]=(int8_t)(B[0][r*N+c]+B[0][((r+1)%N)*N+c]);
  mark("=== 버스 폭 판정: 정방 192³ (4,4), 가속기마다 독립 matmul (best-of-5) ===");
  mark("E194 128비트: 0.622 / 0.714 / 1.032    256비트: 0.620 / 0.630 / 0.656");
  double t[3];
  for(int m=1;m<=3;m++){
    run(m); double b=1e30; int bad=0;
    for(int r=0;r<5;r++){ for(int a=0;a<3;a++) memset(C[a],0,(size_t)N*N);
      double t0=now(); run(m); double d=now()-t0; if(d<b)b=d;
      for(int a=0;a<m;a++) for(int i=0;i<N*N;i++) if(C[a][i]!=REF[i]) { bad++; break; } }
    t[m-1]=b; mark("  m=%d : %8.3f ms  %s", m, b*1e3, bad?"FAIL":"PASS"); }
  mark("  m=3/m=1 = %.2f배  ->  %s", t[2]/t[0],
       t[2]/t[0] > 1.35 ? "128비트 (좁은 버스)" : "256비트 (넓은 버스)");
  mark("=== BUSCHECK_DONE ===");
  return 0; }
