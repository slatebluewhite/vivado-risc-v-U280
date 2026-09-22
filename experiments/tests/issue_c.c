// issue_c.c — c_issue(명령당 발행 비용)를 제대로 제약한다 (E206).
//
// E205: 모델의 발행 항이 3~4점으로만 제약돼 가장 약하다. 발행이 지배하는 유일한 점에서
// -23% 빗나갔고 c_issue가 13이 아니라 약 17이어야 맞았다.
//
// 발행이 지배하는 점을 여러 개 만든다: 명령 입도 3가지 x 가속기 1~3개.
//   순진   6 명령/타일연산 (grt_step_i8)
//   재사용 3 명령/타일연산 (grt_step_r_i8)
//   루프FSM 6/(I·J·K) ~ 0.012 (대조군 — 발행이 절대 지배하지 않아야 함)
//
// 발행 바운드라면  T = m · 타일연산 · cpto · c_issue / f  이므로
//   c_issue = T·f / (m · 타일연산 · cpto)
// 로 역산된다. 여러 점에서 같은 값이 나오면 그것이 c_issue다.
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
#define MAXN 256
static int SZ;
#define FREQ 50.0e6
static int8_t A[3][MAXN*MAXN], B[3][MAXN*MAXN], C[3][MAXN*MAXN] __attribute__((aligned(64)));
static int8_t REF[MAXN*MAXN];
static int NACC;
static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }
static grt_work W(int a){ grt_work w={ .c=&AC[a], .A=A[a], .B=B[a], .C=C[a],
                                       .M=SZ,.N=SZ,.K=SZ }; return w; }
/* 순진 스테퍼 */
static void naive(void){
  grt_work w1=W(0),w2=W(1),w3=W(2);
  if(NACC==1) grt_matmul(&AC[0],A[0],B[0],C[0],SZ,SZ,SZ);
  else if(NACC==2) grt_mm2_i8i8(&w1,&w2);
  else grt_mm3_i8(&w1,&w2,&w3); }
/* 재사용 스테퍼 */
static void reuse(void){
  grt_work w1=W(0),w2=W(1),w3=W(2);
  if(NACC==1) grt_matmul_r_i8(&w1);
  else if(NACC==2) grt_mm2_r_i8i8(&w1,&w2);
  else grt_mm3_r_i8(&w1,&w2,&w3); }
/* 루프 FSM (대조군) */
static void fsm(void){
  int T=SZ/16;
  for(int a=0;a<NACC;a++) grt_loop_ws_config(&AC[a],(uint64_t)SZ,(uint64_t)SZ,(uint64_t)SZ);
  for(int a=0;a<NACC;a++) grt_loop_ws(&AC[a],4,4,T,A[a],B[a],C[a],SZ,SZ,SZ);
  /* 4x4 블록 1개로는 128³을 못 덮으므로 전체 순회 */
  grt_fence(); }
static void fsm_full(void){
  int T=SZ/16;
  for(int a=0;a<NACC;a++) grt_loop_ws_config(&AC[a],(uint64_t)SZ,(uint64_t)SZ,(uint64_t)SZ);
  for(int j0=0;j0<T;j0+=4) for(int i0=0;i0<T;i0+=4)
    for(int a=0;a<NACC;a++)
      grt_loop_ws(&AC[a],4,4,T, A[a]+(size_t)i0*16*SZ, B[a]+(size_t)j0*16,
                  C[a]+(size_t)i0*16*SZ+(size_t)j0*16, SZ,SZ,SZ);
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
static double meas(void (*fn)(void), int *worst){
  double best=1e30; *worst=0;
  for(int t=0;t<5;t++){ for(int a=0;a<3;a++) memset(C[a],0,(size_t)SZ*SZ);
    double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt;
    int b=chk(); if(b>*worst)*worst=b; }
  return best; }
int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/issue_c.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);

  /* E220: 발행이 지배하는 점을 크기별로 늘린다. 작업명령은 순진 4, 재사용 3
     (config는 발행 대역폭을 안 먹는다 — E206). */
  mark("=== 발행 지배 점 늘리기 (w256, 가속기 1~3, best-of-5) ===");
  mark("%5s %8s %5s | %9s | %11s | %11s | %s",
       "크기","스테퍼","개수","시간ms","연산바닥ms","발행예측ms","정확성");
  int Ns[]={128,192,256};
  for(unsigned z=0;z<3;z++){
    SZ=Ns[z]; fill();
    double tops=(double)(SZ/16)*(SZ/16)*(SZ/16);
    double fl = tops*16.0/FREQ*1e3;
    const char *nm[2]={"순진","재사용"};
    double wc[2]={4.0,3.0};
    void (*fn[2])(void)={naive,reuse};
    for(int q=0;q<2;q++){
      for(NACC=1; NACC<=3; NACC++){
        fn[q]();
        int w; double t=meas(fn[q],&w);
        double ti = NACC*tops*wc[q]*17.0/FREQ*1e3;
        mark("%5d %8s %5d | %9.3f | %11.3f | %11.3f | %s",
             SZ, nm[q], NACC, t*1e3, fl, ti, w?"FAIL":"PASS");
      } }
    mark(""); }
  mark("=== ISSUE_C_DONE ===");
  return 0; }
