// shape160.c — 모델의 **사전 예측** 시험: 160³에서는 역전이 없어야 한다 (E184).
//
// E182: 192³에서 최적 블록 모양이 가속기 1개일 때 (6,2), 3개일 때 (4,6)으로 뒤집혔고,
// 1개에서의 차이는 2.2%(잡음)인데 3개에서는 44%였다. 결론은 "단독 프로파일은 틀린 답을 준다"였다.
// **한 크기에서만 본 것**이라 일반성을 확인해야 한다.
//
// 128³은 T=8이라 2·4·8로 모두 나누어떨어져 (I,J)를 정확히 훑을 수 있다.
// 누산기 절반 규칙 I*J <= 32 (E179)에 걸리는 (8,8)만 빠진다.
// 가속기 1개와 3개를 같은 바이너리에서 재서 순위가 뒤집히는지 본다.
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
#define SZ 160
#define FREQ 50.0e6
static int8_t A[3][SZ*SZ], B[3][SZ*SZ], C[3][SZ*SZ] __attribute__((aligned(64)));
static int8_t REF[SZ*SZ];
static int BI, BJ, NACC;

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }
static double bytes_one(void){
  int T=SZ/16; double b=0;
  for(int i0=0;i0<T;i0+=BI) for(int j0=0;j0<T;j0+=BJ){
    int I=(i0+BI<=T)?BI:(T-i0), J=(j0+BJ<=T)?BJ:(T-j0);
    b += (double)(I*T + T*J + I*J)*256.0; }
  return b; }
typedef struct { int i0,j0,done; } blk_cur;
static void blk_step(int a, blk_cur *s){
  if (s->done) return;
  int T=SZ/16;
  int I=(s->i0+BI<=T)?BI:(T-s->i0), J=(s->j0+BJ<=T)?BJ:(T-s->j0);
  grt_loop_ws(&AC[a], I,J,T, A[a]+(size_t)s->i0*16*SZ, B[a]+(size_t)s->j0*16,
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

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/shape128.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  fill();
  double floor_c = (double)SZ*SZ*SZ/256.0;

  mark("=== 모델의 사전 예측 시험 (160³ = 10타일, best-of-5) ===");
  mark("이론 연산 바닥 %.0f cycle. I*J<=32이라 (5,10)/(10,5)는 제외.", floor_c);
  mark("");
  mark("[측정 전에 적는 예측]");
  mark("  바이트 최소는 (5,5) = 128,000 B.  E181의 160³ BLK 훑기에서 연산비 최소도 BLK=5(1.206).");
  mark("  즉 **연산 최적과 바이트 최적이 같은 모양**이므로 E182/E183과 달리 역전이 없어야 한다.");
  mark("  N=3 예측(바이트/5): (5,5) 0.512 / (2,10)(10,2) 0.717 / (2,5)(5,2) 0.819 / (2,2) 1.126 ms");
  mark("  N=1 예측: 전부 연산이 지배 → 폭이 좁고 (5,5)가 1등.");
  mark("  이 예측이 빗나가면 모델이 크기를 넘어 일반화되지 않는 것이다.");
  int pairs[][2]={{2,2},{2,5},{5,2},{5,5},{2,10},{10,2}};   // T=10의 약수 2,5,10
  for(NACC=1; NACC<=3; NACC+=2){
    mark("");
    mark("--- 가속기 %d개 ---", NACC);
    mark("%3s %3s %4s %9s | %8s | %8s | %8s | %s",
         "I","J","블록","바이트","시간ms","연산비","B/cyc","정확성");
    for(unsigned q=0;q<sizeof(pairs)/sizeof(pairs[0]);q++){
      BI=pairs[q][0]; BJ=pairs[q][1];
      double b1=bytes_one(); int T=SZ/16; int nb=(T/BI)*(T/BJ);
      run(); run();
      double best=1e30; int worst=0;
      for(int t=0;t<5;t++){ for(int a=0;a<3;a++) memset(C[a],0,(size_t)SZ*SZ);
        double t0=now(); run(); double dt=now()-t0; if(dt<best)best=dt;
        int b=chk(); if(b>worst) worst=b; }
      double cyc=best*FREQ;
      mark("%3d %3d %4d %9.0f | %8.3f | %8.3f | %8.2f | %s",
           BI,BJ,nb,b1, best*1e3, cyc/(floor_c*NACC)*NACC/NACC*NACC, /* 연산비: 1개 기준 */
           b1*NACC/cyc, worst?"FAIL":"PASS");
    }
  }
  mark("=== SHAPE128_DONE ===");
  return 0; }
