// chunkrule.c — E302: E300 청크 목표 규칙의 `K >= 1536` 조항 표본 밖 검정.
// 사전 등록: experiments/model/PREREG_E302.txt
//
// E300은 42칸에 맞춰 "K >= 1536이면 목표 청크 4"를 넣었다. E278의 N/K 조항과 달리
// 이건 표본 밖에서 검정된 적이 없다. 두 조항은 `K >= 1536 && N/K <= 2`에서만 갈리고,
// 스크래치패드 때문에 그 영역에서 청크 2가 합법인 블록은 (4,4)뿐이다(K <= 2048).
//
// A군 [1792x896] [1920x1920] [2048x2048] [1536x3072] — 전부 미측정, N/K <= 2.
//   H1(E300): 넷 다 청크 4가 이긴다.   H0(E278): 넷 다 청크 2가 이긴다.
// B군 [1280x1280] — K < 1536이므로 두 규칙 모두 청크 2. 대조군.
//
// 청크 3·6도 잰다: E300의 "최적은 2 아니면 4뿐"이 표본 밖에서 버티는지 본다.
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
#define KMAX 2560
#define NMAX 6144
#define FREQ 50.0e6
static int BI=8, BJ=4;
static int KCv=16;
static int8_t A[MMAX*KMAX] __attribute__((aligned(64)));
#define BMAX (1536*6144)
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
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/chunkrule.log","w");
  if(!g){perror("로그");exit(1);}
  if(mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof(cs),&cs); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  mark("=== E302 청크 목표 규칙의 K>=1536 조항 표본 밖 검정 (권장 구성, M=128, best-of-5) ===");
  mark("H1(E300): A군 넷 다 청크4 승리 | H0(E278): 넷 다 청크2 승리 | 2% 미만은 동률");
  mark("%5s %11s %4s %7s %9s | %9s %9s %9s | %11s | %s",
       "Kc","형상","청크","루프라인","","m=1","m=2","m=3","η(2) η(3)","정확성");
  MM=128;
  struct { int K,N; const char*grp; } SH[]={
    {1792, 896,"A"},{1920,1920,"A"},{2048,2048,"A"},{1536,3072,"A"},{1280,1280,"B"}};
  int BLI[]={4,4}, BLJ[]={4,8};
  int CH[]={2,3,4,6,8};
  for(unsigned h=0;h<5;h++){ KK=SH[h].K; NN=SH[h].N;
    int Ktil=KK/16;
    mark("--- %s군 [%dx%d] Ktil=%d N/K=%.2f ---",SH[h].grp,KK,NN,Ktil,(double)NN/KK);
    for(int b=0;b<2;b++){ BI=BLI[b]; BJ=BLJ[b];
      if ((NN/16) % BJ) continue;
      for(int c=0;c<5;c++){
        if (Ktil % CH[c]) continue;
        KCv = Ktil / CH[c];
        if (KCv*(BI+BJ) > 512) continue;
        one(); } } }
  mark("=== CHUNKRULE_DONE ===");
  return 0; }
