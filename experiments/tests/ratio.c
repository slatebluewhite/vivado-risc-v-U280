// ratio.c — 유형 규칙의 경계 찾기 (E276). N/K 비를 0.5~4로 훑는다.
// 개발에 쓰지 않은 워크로드에서 단계별 9조합 훑기가 같은 이득을 주는가?
// hidden=1024, FFN=4096:  [1024x1024] [1024x4096] [4096x1024]
// N=1024 -> Ntil=64, J=4면 nj=16 (3의 배수 아님 -> m=3 불균형 1.125)
// N=4096 -> Ntil=256, J=4면 nj=64 (불균형 1.031)  — 실제 워크로드의 불균형이 그대로 들어간다
// E272의 처방(청크 4)을 적용하려는데 (4,6)은 BERT의 N에서 불균형이 생긴다
// (N=768 -> Ntil=48 -> nj=8 -> m=3에서 3/3/2 -> 1.125). (8,4)는 nj=12로 완벽히 균형.
// 청크 이득과 불균형 손실 중 무엇이 큰지는 재 봐야 안다.
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
#define MMAX 128
#define KMAX 4096
#define NMAX 4096
#define FREQ 50.0e6
static int BI=8, BJ=4;
static int KCv=16;
static int8_t A[MMAX*KMAX] __attribute__((aligned(64)));
#define BMAX (4096*1024)
static int8_t B[BMAX]      __attribute__((aligned(64)));
static int8_t C[MMAX*NMAX] __attribute__((aligned(64)));
static int8_t REF[MMAX*NMAX];
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
  mark("[%4dx%4d] (%d,%d) Kc=%2d 청크%3d nj=%2d 타일%4d | %9.3f %9.3f | %5.2f %5.2f | %s",
       KK,NN,BI,BJ,KCv,(KK/16+KCv-1)/KCv,(NN/16+BJ-1)/BJ,KCv*(BI+BJ),
       t[0]*1e3,t[2]*1e3, t[0]/roof,t[2]/(roof/3), bad?"FAIL":"PASS"); }
int main(int argc,char**argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/blk84.log","w");
  if(!g){perror("로그");exit(1);}
  if(mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof(cs),&cs); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  mark("=== N/K 비 훑기 (권장 구성, M=128, best-of-5) ===");
  mark("nj가 3의 배수가 아니면 m=3에 불균형이 붙는다 — 그 비용까지 포함된 실측이다");
  mark("%5s %11s %4s %7s %9s | %9s %9s %9s | %11s | %s",
       "Kc","형상","청크","루프라인","","m=1","m=2","m=3","η(2) η(3)","정확성");
  MM=128;
  struct { int K,N; } SH[]={{1024,512},{1024,1024},{1024,2048},{1024,4096},
                            {512,1024},{2048,1024},{4096,1024}};
  int BLI[]={8,4,4}, BLJ[]={4,4,8};
  int CH[]={2,4,8};
  for(unsigned h=0;h<7;h++){ KK=SH[h].K; NN=SH[h].N;
    int Ktil=KK/16;
    for(int b=0;b<3;b++){ BI=BLI[b]; BJ=BLJ[b];
      if ((NN/16) % BJ) continue;
      for(int c=0;c<3;c++){
        if (Ktil % CH[c]) continue;
        KCv = Ktil / CH[c];
        if (KCv*(BI+BJ) > 512) continue;
        one(); } } }
  mark("=== BLK84_DONE ===");
  return 0; }
