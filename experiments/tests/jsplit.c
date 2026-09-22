// jsplit.c — J 분할은 왜 E235 곡선보다 20~30% 낮은가 (E238).
//
// E237: 층의 각 단계 속도향상이 가속기당 워킹셋과 단조로 맞물렸지만,
// **독립 복제**인 QKV만 곡선과 2% 안에 맞고 **J 분할**한 세 단계는 20~30% 낮았다.
// 두 가지 설명이 가능하다:
//   (가) J 분할 자체의 손해 — 모든 가속기가 A 전체를 다시 스트리밍한다
//   (나) 형상 의존성 — 곡선은 M=128,N=768 고정에 K만 훑어 얻었고, FFN1은 N=3072다
//        (E235에서 단일항 단순화를 기각한 이유가 바로 형상 의존성이었다)
//
// 가르는 법: **같은 형상**에서 두 방식을 모두 재고 스케일링을 비교한다.
//   모드 A(독립 복제): 가속기마다 자기 데이터로 전체 matmul  -> 처리량 향상 = m*t1/tm
//   모드 B(J 분할)   : matmul 하나를 J로 m분할              -> 향상 = t1/tm
// A가 곡선과 맞고 B만 낮으면 (가). 둘 다 낮으면 (나).
//
// 발행은 반드시 **블록 단위 교대**다(E156, E237). 통째 발행은 겹침이 0에 가깝다.
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
#define M    128
#define KMAX 3072
#define NMAX 3072
#define FREQ 50.0e6
#define BI 4
#define BJ 6
#define KC 16

static int8_t Amat[3][M*KMAX]  __attribute__((aligned(64)));
static int8_t Bmat[3][768*3072] __attribute__((aligned(64)));   /* 최대 K*N */
static int8_t Cmat[3][M*NMAX]  __attribute__((aligned(64)));
static int8_t REF[M*NMAX];
static int NACC, KK, NN;

static FILE *g;
static void mark(const char *fmt, ...){ if(!g) return;
  va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }

typedef struct { const int8_t *A,*B; int8_t *C; int jb,i0,jb_step,done; } iter;

static void it_step(int a, iter *s){
  if(s->done) return;
  int TI=M/16, Ntil=NN/16;
  int j0=s->jb*BJ; int J=(j0+BJ<=Ntil)?BJ:(Ntil-j0);
  int I=(s->i0+BI<=TI)?BI:(TI-s->i0);
  grt_block_ksplit(&AC[a], I,J,KK/16,KC,
                   s->A+(size_t)s->i0*16*KK, s->B+(size_t)j0*16,
                   s->C+(size_t)s->i0*16*NN+(size_t)j0*16,
                   KK, NN, NN, 1);
  s->i0 += BI;                                     /* i 내곽 (E196) */
  if(s->i0>=TI){ s->i0=0; s->jb+=s->jb_step;
                 if(s->jb*BJ>=Ntil) s->done=1; } }

// mode 0 = 독립 복제, 1 = J 분할
static void run(int mode){
  iter s[3];
  for(int a=0;a<NACC;a++){
    grt_loop_ws_config(&AC[a], KK, NN, NN);
    s[a].A = mode? Amat[0] : Amat[a];
    s[a].B = mode? Bmat[0] : Bmat[a];
    s[a].C = mode? Cmat[0] : Cmat[a];
    s[a].jb = mode? a : 0;
    s[a].jb_step = mode? NACC : 1;
    s[a].i0 = 0;
    s[a].done = (s[a].jb*BJ >= NN/16);
  }
  int left; do { left=0;
    for(int a=0;a<NACC;a++){ it_step(a,&s[a]); if(!s[a].done) left++; }
  } while(left);
  grt_fence(); }

static void fill(void){
  for(int k=0;k<3;k++){
    memset(Amat[k],0,(size_t)M*KK);
    for(int r=0;r<M;r++){ Amat[k][(size_t)r*KK+r]=1; Amat[k][(size_t)r*KK+r+1]=1; }
    for(int r=0;r<KK;r++) for(int c=0;c<NN;c++)
      Bmat[k][(size_t)r*NN+c]=(int8_t)((r*3+c*5)%7-3); }
  for(int r=0;r<M;r++) for(int c=0;c<NN;c++)
    REF[(size_t)r*NN+c]=(int8_t)(Bmat[0][(size_t)r*NN+c]+Bmat[0][(size_t)(r+1)*NN+c]); }

static int chk(int mode){ int b=0;
  int n = mode?1:NACC;
  for(int a=0;a<n;a++) for(int r=0;r<M;r++) for(int c=0;c<NN;c++)
    if(Cmat[a][(size_t)r*NN+c]!=REF[(size_t)r*NN+c]) b++;
  return b; }

static double best3(int mode,int *bad){
  double b=1e30; *bad=0;
  run(mode);                                        /* 웜업 */
  for(int s=0;s<3;s++){
    for(int a=0;a<3;a++) memset(Cmat[a],0,(size_t)M*NN);
    double t0=now(); run(mode); double dt=now()-t0;
    if(dt<b) b=dt; *bad += chk(mode); }
  return b; }

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/jsplit.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof(cs),&cs); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);

  mark("=== J 분할 대 독립 복제, 같은 형상에서 (3i8f50, 50MHz, best-of-3) ===");
  mark("A가 곡선과 맞고 B만 낮으면 'J 분할의 손해', 둘 다 낮으면 '형상 의존성'");
  mark("%-18s %4s | %9s %9s | %8s %8s | %s","형상","m",
       "독립ms","J분할ms","독립향상","J향상","정확성");

  struct { int K,N; const char *nm; } sh[] = {
    { 768,  768, "QKV/출력 [768x768]" },
    { 768, 3072, "FFN1     [768x3072]" },
    {3072,  768, "FFN2     [3072x768]" },
  };
  for(unsigned i=0;i<sizeof(sh)/sizeof(sh[0]);i++){
    KK=sh[i].K; NN=sh[i].N; fill();
    double a1=0,b1=0;
    for(NACC=1;NACC<=3;NACC++){
      int ba,bb;
      double ta=best3(0,&ba);           /* 독립 복제 */
      double tb=best3(1,&bb);           /* J 분할 */
      if(NACC==1){a1=ta;b1=tb;}
      /* 워킹셋: 독립은 A+B+C 전부, J분할은 A + (B+C)/m */
      double wsA=(double)M*KK+(double)KK*NN+(double)M*NN;
      double wsB=(double)M*KK+((double)KK*NN+(double)M*NN)/NACC;
      mark("%-18s %4d | %9.3f %9.3f | %7.2fx %7.2fx | %s   ws 독립%.2fMB J분할%.2fMB",
           sh[i].nm, NACC, ta*1e3, tb*1e3,
           NACC*a1/ta, b1/tb, (ba||bb)?"FAIL":"PASS", wsA/1e6, wsB/1e6);
      if(ba||bb) mark("    불일치 독립=%d J분할=%d", ba, bb);
    }
  }
  mark("=== JSPLIT_DONE ===");
  return 0; }
