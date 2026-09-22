// mtest.c — M=128에서 적합한 a·b 곡선이 M=256·512로 옮겨가는가 (E259).
// 사전 등록: experiments/model/PREREG_E259.txt (η는 M과 무관해야 한다).
// i 블록 수가 M/64로 2->4->8로 늘어나므로 변할 이유는 있다.
//
// E246: [2048x512]만 모델의 진짜 오답으로 남았고, 적합이 b(512)=-0.05를 주는데
// 실측은 +0.16을 요구했다. E243의 격자는 K와 N을 함께 키우는 방향만 촘촘했으므로
// 모서리가 미측정이다.
//
// 설계상 중요한 점: **N을 nj가 6의 배수가 되게 고른다**(Ntil=36/72/144 -> nj=6/12/24).
// 그러면 m=2와 m=3 모두 부하 불균형이 정확히 1.0이라, 잔차에 남는 것은 분리성 파탄뿐이다.
// 꼬리 블록도 없다(전부 6으로 나뉜다). E246에서 꼬리가 m=3에서 2~8%임을 알았으므로
// 이것을 제거해 두는 것이 중요하다.
//
//   K = 256 / 768 / 2048 / 3072  x  N = 576 / 1152 / 2304   (4x3)
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
static int MM;
#define MMAX 512
#define KMAX 2048
#define NMAX 2304
#define FREQ 50.0e6
#define BI 4
#define BJ 6
#define KC 16
static int8_t A[MMAX*KMAX] __attribute__((aligned(64)));
static int8_t B[KMAX*NMAX] __attribute__((aligned(64)));
static int8_t C[MMAX*NMAX] __attribute__((aligned(64)));
static int8_t REF[MMAX*NMAX];
static int KK,NN;
typedef struct { int jb,i0,step,done; } iter;
static void it_step(int a, iter *s){
  if(s->done) return;
  int TI=MM/16, Ntil=NN/16;
  int j0=s->jb*BJ; int J=(j0+BJ<=Ntil)?BJ:(Ntil-j0);
  int I=(s->i0+BI<=TI)?BI:(TI-s->i0);
  grt_block_ksplit(&AC[a], I,J,KK/16,KC,
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
int main(int argc,char**argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/corner.log","w");
  if(!g){perror("로그");exit(1);}
  if(mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof(cs),&cs); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  mark("=== M 축 검정 (3i8f50 현재 상태, (4,6), Kc=16, best-of-5) ===");
  mark("사전 등록: η는 M과 무관해야 한다");
  mark("%4s %11s %4s %9s | %9s %9s %9s | %11s | %s","M","형상","nj","루프라인","m=1","m=2","m=3","η(2) η(3)","정확성");
  mark("%11s %4s %8s | %9s %9s %9s | %11s | %s",
       "형상","nj","루프라인","m=1","m=2","m=3","η(2) η(3)","정확성");
  struct { int K,N; } S[]={{1024,1152},{2048,1152},{1024,2304}};
  int Ms[]={128,256,512};
  for(unsigned si=0; si<3; si++) for(int mi=0; mi<3; mi++){
    KK=S[si].K; NN=S[si].N; MM=Ms[mi]; fill();
    double roof=(double)MM*KK*NN/256.0/FREQ;
    double t[3]; int bad=0;
    for(int m=1;m<=3;m++){
      run(m); double bt=1e30;
      for(int r=0;r<5;r++){ memset(C,0,(size_t)MM*NN);
        double t0=now(); run(m); double d=now()-t0; if(d<bt)bt=d; bad+=chk(); }
      t[m-1]=bt; }
    mark("[%4dx%4d] %4d %8.3f | %9.3f %9.3f %9.3f | %5.2f %5.2f | %s",
         KK,NN,(NN/16)/BJ,roof*1e3,t[0]*1e3,t[1]*1e3,t[2]*1e3,
         t[1]/(roof/2),t[2]/(roof/3), bad?"FAIL":"PASS");
  }
  mark("=== CORNER_DONE ===");
  return 0; }
