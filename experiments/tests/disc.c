// disc.c — 넓은 버스가 어떤 작업에는 듣고 어떤 작업에는 안 듣는가, 그 축은 무엇인가 (E250).
//
// E249: 모서리 격자(M=128, J 분할)는 버스를 두 배로 넓혀도 36점 전부 1.5% 안에서 같은데,
// 같은 회차의 정방 192³(M=N=K=192, 독립 복제)은 1.62배 빨라진다.
// E249는 "L2 상주 여부"를 후보로 적었으나 **기존 데이터가 그것을 배제한다** —
// 정방 512³는 워킹셋 2.36MB로 L2(512KB)를 한참 넘는데 256/128 = 0.76으로 여전히 이득이다.
//
// 남은 후보 둘을 2x2로 가른다. K=512, N=576 고정:
//     M    = 128 (i 블록 2개)  대  512 (i 블록 8개)
//     방식 = J 분할(A·B 공유)   대  독립 복제(각자 자기 데이터)
// 같은 바이너리를 256비트에서 한 번, 128비트에서 한 번 돌려 시간비를 본다.
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
#define KK   512
#define NN   576
#define FREQ 50.0e6
#define BI 4
#define BJ 6
#define KC 16
static int8_t A[3][MMAX*KK] __attribute__((aligned(64)));
static int8_t B[3][KK*NN]   __attribute__((aligned(64)));
static int8_t C[3][MMAX*NN] __attribute__((aligned(64)));
static int8_t REF[MMAX*NN];
static int MM;
typedef struct { const int8_t *a,*b; int8_t *c; int jb,i0,step,done; } iter;
static void it_step(int x, iter *s){
  if(s->done) return;
  int TI=MM/16, Ntil=NN/16;
  int j0=s->jb*BJ; int J=(j0+BJ<=Ntil)?BJ:(Ntil-j0);
  int I=(s->i0+BI<=TI)?BI:(TI-s->i0);
  grt_block_ksplit(&AC[x], I,J,KK/16,KC,
                   s->a+(size_t)s->i0*16*KK, s->b+(size_t)j0*16,
                   s->c+(size_t)s->i0*16*NN+(size_t)j0*16, KK,NN,NN, 1);
  s->i0 += BI;
  if(s->i0>=TI){ s->i0=0; s->jb+=s->step; if(s->jb*BJ>=Ntil) s->done=1; } }
// split=1: matmul 하나를 J로 m분할 / split=0: 가속기마다 독립 matmul
static void run(int m,int split){
  iter s[3];
  for(int x=0;x<m;x++){ grt_loop_ws_config(&AC[x], KK,NN,NN);
    s[x]=(iter){ split?A[0]:A[x], split?B[0]:B[x], split?C[0]:C[x],
                 split?x:0, 0, split?m:1, 0 };
    if(split && x*BJ>=NN/16) s[x].done=1; }
  int left; do{ left=0;
    for(int x=0;x<m;x++){ it_step(x,&s[x]); if(!s[x].done) left++; } }while(left);
  grt_fence(); }
static FILE *g;
static void mark(const char *f,...){ if(!g)return; va_list ap; va_start(ap,f);
  vfprintf(g,f,ap); va_end(ap); fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }
static void fill(void){
  for(int k=0;k<3;k++){
    memset(A[k],0,(size_t)MM*KK);
    for(int r=0;r<MM;r++){ A[k][(size_t)r*KK+(r%(KK-1))]=1; A[k][(size_t)r*KK+(r%(KK-1))+1]=1; }
    for(int r=0;r<KK;r++) for(int c=0;c<NN;c++) B[k][(size_t)r*NN+c]=(int8_t)((r*3+c*5)%7-3); }
  for(int r=0;r<MM;r++){ int p=r%(KK-1);
    for(int c=0;c<NN;c++)
      REF[(size_t)r*NN+c]=(int8_t)(B[0][(size_t)p*NN+c]+B[0][(size_t)(p+1)*NN+c]); } }
static int chk(int m,int split){ int b=0; int n=split?1:m;
  for(int x=0;x<n;x++) for(int r=0;r<MM;r++) for(int c=0;c<NN;c++)
    if(C[x][(size_t)r*NN+c]!=REF[(size_t)r*NN+c]) { b++; }
  return b; }
int main(int argc,char**argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/disc.log","w");
  if(!g){perror("로그");exit(1);}
  if(mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof(cs),&cs); sched_yield();
  for(int x=0;x<3;x++) grt_flush_ctx(&AC[x]);
  mark("=== 넓은 버스가 듣는 축 찾기: M x 분할방식 (K=%d, N=%d, (4,6), best-of-3) ===",KK,NN);
  mark("%5s %10s %9s | %9s %9s %9s | %s","M","방식","루프라인","m=1","m=2","m=3","정확성");
  int Ms[]={128,512};
  for(int mi=0;mi<2;mi++){
    MM=Ms[mi]; fill();
    double roof=(double)MM*KK*NN/256.0/FREQ;
    for(int sp=1; sp>=0; sp--){
      double t[3]; int bad=0;
      for(int m=1;m<=3;m++){
        run(m,sp); double b=1e30;
        for(int r=0;r<3;r++){ for(int x=0;x<3;x++) memset(C[x],0,(size_t)MM*NN);
          double t0=now(); run(m,sp); double d=now()-t0; if(d<b)b=d; bad+=chk(m,sp); }
        t[m-1]=b; }
      mark("%5d %10s %9.3f | %9.3f %9.3f %9.3f | %s",
           MM, sp?"J분할":"독립복제", roof*1e3, t[0]*1e3,t[1]*1e3,t[2]*1e3, bad?"FAIL":"PASS");
    }
  }
  mark("=== DISC_DONE ===");
  return 0; }
