// mem2test.c — E355: 온칩 메모리를 두 배로 하면 큰 K의 청크 강제가 풀리는가.
//
// 스크래치패드 Kc*(I+J) <= 512 타일이 Kc <= 64를 만들고, 그게
//   최소 청크 = ceil((K/16)/64) = K/1024
// 를 만든다. 따라서 K > 4096이면 어떤 블록으로도 청크 목표 4에 못 간다.
// 누산기 64타일의 절반 규칙이 I*J <= 32를 만들어 (8,8)(강도 4.0)을 막는다.
//
// `Rocket64b1gem2i8w256su20mem2f62`는 스크래치패드 256->512KB, 누산기 64->128KB다.
// 한계가 각각 Kc*(I+J) <= 1024, I*J <= 64로 두 배가 된다.
//
// **같은 바이너리를 두 비트스트림에 돌린다.** 옛 판에서 새 동작점은 FAIL이어야 한다
// (한계를 넘긴 Kc는 더 빠르게 돌면서 틀린 답을 내고, I*J 초과도 틀린 답을 낸다 —
//  행이 아니다). 그게 하드웨어 변경이 실제로 먹혔다는 검증이기도 하다.
//
// 가속기 2개 판이므로 문맥 3은 건드리지 않는다 (E337: 없는 opcode는 행을 만든다).
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

#define NACC 2
static const grt_ctx AC[NACC] = {
  { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_INT8 },
  { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_FP32 },
};
#define MMAX 128
#define KMAX 8192
#define NMAX 2048
#define FREQ 62.5e6
static int BI=4, BJ=4;
static int KCv=64;
static int8_t A[MMAX*KMAX]        __attribute__((aligned(64)));
static int8_t B[(size_t)KMAX*NMAX] __attribute__((aligned(64)));
static int8_t C[MMAX*NMAX]        __attribute__((aligned(64)));
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
  iter s[NACC];
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

// A는 행마다 1이 두 개뿐이라 참조가 B[p]+B[p+1]로 정확히 int8에 들어간다 (chunkrule.c와 동일).
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

// fill()은 형상마다 한 번만 부른다 — B는 K*N 바이트라 62.5 MHz 코어에서 셀마다 채우면
// 측정보다 채우기가 더 오래 걸린다.
static void one(const char *tag){
  double roof=(double)MM*KK*NN/256.0/FREQ, t[NACC]; int bad=0;
  for(int m=1;m<=NACC;m++){
    run(m); double b=1e30;
    for(int r=0;r<5;r++){ memset(C,0,(size_t)MM*NN);
      double t0=now(); run(m); double d=now()-t0; if(d<b)b=d; bad+=chk(); }
    t[m-1]=b; }
  int ch=(KK/16+KCv-1)/KCv;
  mark("[%5dx%5d] (%d,%d) Kc=%3d 청크%3d 강도%5.2f sp%5d acc%3d | %9.3f %9.3f | %5.2f %5.2f | %-8s %s",
       KK,NN,BI,BJ,KCv,ch, 16.0*(1.0/BI+1.0/BJ), KCv*(BI+BJ), BI*BJ,
       t[0]*1e3,t[1]*1e3, t[0]/roof, t[1]/(roof/2), tag, bad?"FAIL":"PASS"); }

int main(int argc,char**argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/mem2test.log","w");
  if(!g){perror("로그");exit(1);}
  if(mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof(cs),&cs); sched_yield();
  for(int a=0;a<NACC;a++) grt_flush_ctx(&AC[a]);

  mark("=== E355: 온칩 메모리 2배가 큰 K의 청크 강제를 푸는가 (M=128, 가속기 2개, best-of-5) ===");
  mark("옛 한계: Kc*(I+J) <= 512, I*J <= 32   |   새 한계: <= 1024, <= 64");
  mark("같은 바이너리를 두 비트스트림에 돌린다. 옛 판에서 '새' 표시 행은 FAIL이어야 정상이다.");
  mark("%13s %5s %6s %5s %6s %8s %4s | %9s %9s | %11s | %s",
       "형상","블록","Kc","청크","강도","sp타일","acc","m=1","m=2","η(1) η(2)","분류 정확성");
  MM=128;
  struct { int K,N; } SH[]={ {4096,1024}, {6144,1536}, {8192,2048} };
  struct { int I,J; } BL[]={ {4,4}, {8,4}, {4,8}, {8,8} };
  int KCL[]={32,40,48,64,85,96,128};

  for(unsigned h=0; h<sizeof(SH)/sizeof(SH[0]); h++){
    KK=SH[h].K; NN=SH[h].N; int Ktil=KK/16;
    mark("--- [%dx%d] Ktil=%d  옛 하드웨어 최소청크=%d ---",KK,NN,Ktil,(Ktil+63)/64);
    fill();
    for(unsigned b=0;b<sizeof(BL)/sizeof(BL[0]);b++){
      BI=BL[b].I; BJ=BL[b].J;
      if ((MM/16) % BI) continue;
      if ((NN/16) % BJ) continue;
      for(unsigned c=0;c<sizeof(KCL)/sizeof(KCL[0]);c++){
        KCv=KCL[c];
        if (KCv > Ktil) continue;
        int sp=KCv*(BI+BJ), acc=BI*BJ;
        if (sp > 1024 || acc > 64) continue;              // 새 하드웨어에서도 불법
        const char *tag = (sp<=512 && acc<=32) ? "옛" : "새";
        one(tag); } } }
  mark("=== MEM2TEST_DONE ===");
  return 0; }
