// blk2.c — 최적 블록 모양이 K에 따라 뒤집히는가 (E244).
//
// E243: 라이브 = m·(BI+BJ)·K·16 이 L2(512KB)를 넘으면 η가 뛴다 (기제 + 문턱).
// E191: I·J<=32 안에서 1/I+1/J 최소화, 즉 **크게** 잡으라.
// 두 항이 반대로 움직이므로 K에 따라 최적이 달라져야 한다. 사전 등록은
// experiments/model/PREREG_E244.txt 이고 반증 조건까지 적어 두었다.
//
// N=960 고정(모든 BJ가 60을 나눔), K=512와 2048, m=1과 3, 여덟 모양.
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
#define KMAX 2048
#define NN 960
#define FREQ 50.0e6
#define KC 16
static int8_t A[M*KMAX]   __attribute__((aligned(64)));
static int8_t B[KMAX*NN]  __attribute__((aligned(64)));
static int8_t C[M*NN]     __attribute__((aligned(64)));
static int8_t REF[M*NN];
static int KK, gBI, gBJ;

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
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/blk2.log","w");
  if(!g){perror("로그");exit(1);}
  if(mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof(cs),&cs); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  mark("=== 블록 모양 x K (3i8f50, M=128, N=960, Kc=16, best-of-3) ===");
  mark("사전 등록: K=512는 큰 블록이 최적, K=2048·m=3은 (2,2)가 크게 올라야 한다");
  int SI[]={2,2,4,4,2,4,8,8}, SJ[]={2,4,2,4,6,6,2,4};
  int Ks[]={512,2048};
  for(int ki=0;ki<2;ki++){
    KK=Ks[ki]; fill();
    double roof=(double)M*KK*NN/256.0/FREQ;
    mark("--- K=%d  루프라인 %.3f ms ---", KK, roof*1e3);
    mark("%7s %4s %9s | %9s %9s | %6s %6s | %s",
         "(I,J)","I·J","라이브m3","m=1 ms","m=3 ms","η(1)","η(3)","정확성");
    for(int i=0;i<8;i++){
      gBI=SI[i]; gBJ=SJ[i];
      double t1,t3; int bad=0;
      for(int m=1;m<=3;m+=2){
        run(m); double b=1e30;
        for(int r=0;r<3;r++){ memset(C,0,(size_t)M*NN);
          double t0=now(); run(m); double d=now()-t0; if(d<b)b=d; bad+=chk(); }
        if(m==1) t1=b; else t3=b; }
      mark("  (%d,%d) %4d %7dKB | %9.3f %9.3f | %6.2f %6.2f | %s",
           gBI,gBJ,gBI*gBJ, 3*(gBI+gBJ)*KK*16/1024,
           t1*1e3,t3*1e3, t1/roof, t3/(roof/3), bad?"FAIL":"PASS");
    }
  }
  mark("=== BLK2_DONE ===");
  return 0; }
