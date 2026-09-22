// ceiling.c — 천장이 DRAM인가 FSM 상호방해인가 (E294).
// 사전 등록: experiments/model/PREREG_E294.txt
// 연산 604 M MAC 고정, 가중치만 1.18/2.36/4.72 MB로 바꾼다.
// 사전 등록: experiments/model/PREREG_E272.txt  블록 (4,4) 고정.
// 사전 등록: experiments/model/PREREG_E271.txt
// 사전 등록: experiments/model/PREREG_E269.txt
//
// E260: 곡선은 블록 모양을 넘어 옮겨가지 않는다(−44%~+57%). 그리고 (8,4)가
// (4,6)보다 m=3에서 항상 빠른데(최대 23%), 나는 E241~E259를 (4,6)에서 세웠다.
// 여기서 확인할 것 두 가지:
//   (1) 결합 형태가 여전히 p≈2인가 (max도 가법도 아닌가)
//   (2) η가 여전히 M에 무관한가
// 구조가 블록에 무관하면 모델의 일반성이 한 단계 올라간다.
//
// N은 384의 배수 -> Ntil이 24의 배수 -> nj가 6의 배수 -> m=2,3 모두 불균형 1.0, 꼬리 없음.
// K는 256의 배수 -> Ktil이 16의 배수 -> K 분할도 딱 떨어짐.
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
#define MMAX 512
#define KMAX 3072
#define NMAX 1536
#define FREQ 50.0e6
static int BI=8, BJ=4;
static int KCv=16;
static int8_t A[512*3072]  __attribute__((aligned(64)));
static int8_t B[KMAX*NMAX] __attribute__((aligned(64)));
static int8_t C[512*1536]  __attribute__((aligned(64)));
static int8_t REF[512*1536];
static int MM,KK,NN;
typedef struct { int jb,i0,step,done; } iter;
static void it_step(int a, iter *s){
  if(s->done) return;
  int TI=MM/16, Ntil=NN/16;
  int j0=s->jb*BJ; int J=(j0+BJ<=Ntil)?BJ:(Ntil-j0);
  int I=(s->i0+BI<=TI)?BI:(TI-s->i0);
  grt_block_ksplit(&AC[a], I,J,KK/16,KCv,
                   A+(size_t)s->i0*16*KK, B+(size_t)j0*16,
                   C+(size_t)s->i0*16*NN+(size_t)j0*16, KK,NN,NN, 1);
  s->i0 += BI;
  if(s->i0>=TI){ s->i0=0; s->jb+=s->step; if(s->jb*BJ>=Ntil) s->done=1; } }
static void run(int m){
  iter s[3];
  for(int a=0;a<m;a++){ grt_loop_ws_config(&AC[a], KK,NN,NN);
    s[a]=(iter){a,0,m,0}; if(a*BJ>=NN/16) s[a].done=1; }
  int left; do{ left=0;
    for(int a=0;a<m;a++){ it_step(a,&s[a]); if(!s[a].done) left++; } }while(left);
  grt_fence(); }
static FILE *g;
static void mark(const char *f,...){ if(!g)return; va_list ap; va_start(ap,f);
  vfprintf(g,f,ap); va_end(ap); fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }
static void fill(void){
  memset(A,0,(size_t)MM*KK);
  for(int r=0;r<MM;r++){ int p=r%(KK-1); A[(size_t)r*KK+p]=1; A[(size_t)r*KK+p+1]=1; }
  for(int r=0;r<KK;r++) for(int c=0;c<NN;c++) B[(size_t)r*NN+c]=(int8_t)((r*3+c*5)%7-3);
  for(int r=0;r<MM;r++){ int p=r%(KK-1);
    for(int c=0;c<NN;c++)
      REF[(size_t)r*NN+c]=(int8_t)(B[(size_t)p*NN+c]+B[(size_t)(p+1)*NN+c]); } }
static int chk(void){ int b=0;
  for(int r=0;r<MM;r++) for(int c=0;c<NN;c++)
    if(C[(size_t)r*NN+c]!=REF[(size_t)r*NN+c]) b++; return b; }
static void one(void){
  fill();
  double roof=(double)MM*KK*NN/256.0/FREQ, t[3]; int bad=0;
  for(int m=1;m<=3;m++){
    run(m); double b=1e30;
    for(int r=0;r<5;r++){ memset(C,0,(size_t)MM*NN);
      double t0=now(); run(m); double d=now()-t0; if(d<b)b=d; bad+=chk(); }
    t[m-1]=b; }
  mark("M=%4d N=%4d 가중치%5.2fMB nbi=%d | %9.3f %9.3f %9.3f | 향상 %5.2f %5.2f | %s",
       MM,NN,(double)KK*NN/1e6,(MM/16+BI-1)/BI,t[0]*1e3,t[1]*1e3,t[2]*1e3,
       t[0]/t[1],t[0]/t[2], bad?"FAIL":"PASS"); }
int main(int argc,char**argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/blk84.log","w");
  if(!g){perror("로그");exit(1);}
  if(mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof(cs),&cs); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  mark("=== 천장의 정체: 연산 고정, 가중치만 4배 (권장 구성, K=3072, (8,4) 청크8) ===");
  mark("A(DRAM): 가중치가 작을수록 m=3이 좋다 / B(FSM): 셋이 같다");
  mark("%5s %11s %4s %7s %9s | %9s %9s %9s | %11s | %s",
       "Kc","형상","청크","루프라인","","m=1","m=2","m=3","η(2) η(3)","정확성");
  KK=3072; KCv=24;
  struct { int M,N; } S[]={{512,384},{256,768},{128,1536}};
  for(unsigned i=0;i<3;i++){ MM=S[i].M; NN=S[i].N; one(); }
  mark("=== BLK84_DONE ===");
  return 0; }
