// tail.c — 부분(꼬리) 블록이 E245의 실패 둘을 설명하는가 (E246).
// 사전 등록은 JOURNAL E245 말미. [512x2048]과 [2048x512]를 BJ=6(꼬리 J=2)과
// BJ=4(정확)로 각각 재고, 모델 예측과 대조한다.
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
#define M 128
#define FREQ 50.0e6
#define KC 16
#define NMAX 2112
static int8_t A[M*2048]     __attribute__((aligned(64)));
static int8_t B[2048*NMAX]  __attribute__((aligned(64)));
static int8_t C[M*NMAX]     __attribute__((aligned(64)));
static int8_t REF[M*NMAX];
static int KK,NN,gBI,gBJ;
typedef struct { int jb,i0,step,done; } iter;
static void it_step(int a, iter *s){
  if(s->done) return;
  int TI=M/16, Ntil=NN/16;
  int j0=s->jb*gBJ; int J=(j0+gBJ<=Ntil)?gBJ:(Ntil-j0);
  int I=(s->i0+gBI<=TI)?gBI:(TI-s->i0);
  grt_block_ksplit(&AC[a], I,J,KK/16,KC,
                   A+(size_t)s->i0*16*KK, B+(size_t)j0*16,
                   C+(size_t)s->i0*16*NN+(size_t)j0*16, KK,NN,NN, 1);
  s->i0 += gBI;
  if(s->i0>=TI){ s->i0=0; s->jb+=s->step; if(s->jb*gBJ>=Ntil) s->done=1; } }
static void run(int m){
  iter s[3];
  for(int a=0;a<m;a++){ grt_loop_ws_config(&AC[a], KK,NN,NN);
    s[a]=(iter){a,0,m,0}; if(a*gBJ>=NN/16) s[a].done=1; }
  int left; do{ left=0;
    for(int a=0;a<m;a++){ it_step(a,&s[a]); if(!s[a].done) left++; } }while(left);
  grt_fence(); }
static FILE *g;
static void mark(const char *f,...){ if(!g)return; va_list ap; va_start(ap,f);
  vfprintf(g,f,ap); va_end(ap); fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }
static void fill(void){
  memset(A,0,(size_t)M*KK);
  for(int r=0;r<M;r++){ A[(size_t)r*KK+r]=1; A[(size_t)r*KK+r+1]=1; }
  for(int r=0;r<KK;r++) for(int c=0;c<NN;c++) B[(size_t)r*NN+c]=(int8_t)((r*3+c*5)%7-3);
  for(int r=0;r<M;r++) for(int c=0;c<NN;c++)
    REF[(size_t)r*NN+c]=(int8_t)(B[(size_t)r*NN+c]+B[(size_t)(r+1)*NN+c]); }
static int chk(void){ int b=0;
  for(int r=0;r<M;r++) for(int c=0;c<NN;c++)
    if(C[(size_t)r*NN+c]!=REF[(size_t)r*NN+c]) b++; return b; }
int main(int argc,char**argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/tail.log","w");
  if(!g){perror("로그");exit(1);}
  if(mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof(cs),&cs); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  mark("=== 꼬리 블록 검정 (3i8f50, M=128, Kc=16, best-of-3) ===");
  mark("BJ를 바꾸면 강도도 바뀌어 교란된다. **패딩**으로 가른다:");
  mark("  N을 96의 배수까지 늘리면 블록이 정확해지고 (I,J)는 그대로다.");
  mark("  일은 늘어나므로, 늘어난 비율보다 덜 느려지면 꼬리 비용이 실재한다.");
  mark("%11s %6s %6s %5s | %8s %8s %8s | %s","형상","(I,J)","블록수","꼬리","m=1","m=2","m=3","정확성");
  /* (K,N) 쌍: 원본(꼬리 있음)과 96의 배수로 패딩한 것(정확), 블록은 (4,6) 고정 */
  struct { int K,N; } S[]={{512,2048},{512,2112},{2048,512},{2048,576}};
  for(int si=0;si<4;si++){
    KK=S[si].K; NN=S[si].N; fill();
    {
      gBI=4; gBJ=6;
      int TN=NN/16, nb=(TN+gBJ-1)/gBJ, last=TN-(nb-1)*gBJ;
      double t[3]; int bad=0;
      for(int m=1;m<=3;m++){
        run(m); double b=1e30;
        for(int r=0;r<3;r++){ memset(C,0,(size_t)M*NN);
          double t0=now(); run(m); double d=now()-t0; if(d<b)b=d; bad+=chk(); }
        t[m-1]=b; }
      mark("[%4dx%4d]  (%d,%d) %6d %5d | %8.3f %8.3f %8.3f | %s",
           KK,NN,gBI,gBJ,nb,last,t[0]*1e3,t[1]*1e3,t[2]*1e3,bad?"FAIL":"PASS");
    }
  }
  mark("=== TAIL_DONE ===");
  return 0; }
