// mid6.c — E410: h1536 층 블록 전수를 **새 Kc 규칙**(E406/E408)으로 다시 훑는다.
// measured.py의 표는 옛 Kc로 잰 것이라 도구가 권하는 조합을 채점할 수 없었다(E407 주석).
// 규칙이 이 워크로드에 주는 (단계, 블록)별 Kc:
//   QKV·출력 Kc=48   FFN1 Kc=24   FFN2 (4,4)Kc=96 나머지 Kc=64
// 블록 (4,4)(8,4)(4,8)(8,8) x m, 5회 재실행 칸별 최소.
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
#define SEQ 128
#define H   1536
#define FF  6144
#define FREQ 62.5e6
static int BI=8, BJ=4;
static int KCv=32;

static int8_t Xin[SEQ*FF] __attribute__((aligned(64)));
static int8_t Wq[3][H*H]  __attribute__((aligned(64)));   /* Q,K,V */
static int8_t Wo[H*H]     __attribute__((aligned(64)));   /* 출력 */
static int8_t W1[H*FF]    __attribute__((aligned(64)));
static int8_t W2[FF*H]    __attribute__((aligned(64)));
static int8_t Out[3][SEQ*FF] __attribute__((aligned(64)));
static int8_t REF[SEQ*FF];

typedef struct { const int8_t *A,*B; int8_t *C; int K,N,jb,i0,step,done; } iter;

static void it_step(int a, iter *s){
  if(s->done) return;
  int TI=SEQ/16, Ntil=s->N/16;
  int j0=s->jb*BJ; int J=(j0+BJ<=Ntil)?BJ:(Ntil-j0);
  int I=(s->i0+BI<=TI)?BI:(TI-s->i0);
  grt_block_ksplit(&AC[a], I,J,s->K/16,KCv,
                   s->A+(size_t)s->i0*16*s->K, s->B+(size_t)j0*16,
                   s->C+(size_t)s->i0*16*s->N+(size_t)j0*16,
                   s->K, s->N, s->N, 1);
  s->i0 += BI;
  if(s->i0>=TI){ s->i0=0; s->jb+=s->step; if(s->jb*BJ>=Ntil) s->done=1; } }

// matmul 하나를 가속기 m개로 J 분할, 블록 단위 교대 발행
static void mm_split(const int8_t *A,const int8_t *B,int8_t *C,int K,int N,int m){
  iter s[3];
  for(int a=0;a<m;a++){ grt_loop_ws_config(&AC[a], K, N, N);
    s[a]=(iter){A,B,C,K,N,a,0,m,0};
    if(a*BJ >= N/16) s[a].done=1; }
  int left; do{ left=0;
    for(int a=0;a<m;a++){ it_step(a,&s[a]); if(!s[a].done) left++; } }while(left);
  grt_fence(); }

// 독립 matmul 세 개를 가속기 하나씩 (E237 판)
static void qkv_each(int m){
  /* Q,K,V는 **항상 세 개**다. 가속기가 m<3이면 여러 라운드로 돈다.
     (이전 판은 for(a<m)으로 m개만 계산했다 — E237에서 stage_qkv를 고쳤는데
      이 함수에 같은 버그가 남아 있었다. m=2 측정이 무효였다.) */
  for(int base=0; base<3; base+=m){
    int n = (3-base < m) ? (3-base) : m;
    iter s[3];
    for(int a=0;a<n;a++){ grt_loop_ws_config(&AC[a], H,H,H);
      s[a]=(iter){Xin,Wq[base+a],Out[base+a],H,H,0,0,1,0}; }
    int left; do{ left=0;
      for(int a=0;a<n;a++){ it_step(a,&s[a]); if(!s[a].done) left++; } }while(left);
  }
  grt_fence(); }

static void setblk(int i,int j,int kc){ BI=i; BJ=j; KCv=kc; }
static FILE *g;
static void mark(const char *fmt, ...){ if(!g) return;
  va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }

static void set_in(int cols){ memset(Xin,0,(size_t)SEQ*cols);
  for(int r=0;r<SEQ;r++){ Xin[(size_t)r*cols+r]=1; Xin[(size_t)r*cols+r+1]=1; } }
static void set_ref(const int8_t *B,int N){
  for(int r=0;r<SEQ;r++) for(int c=0;c<N;c++)
    REF[(size_t)r*N+c]=(int8_t)(B[(size_t)r*N+c]+B[(size_t)(r+1)*N+c]); }
static int chk(const int8_t *C,int N){ int b=0;
  for(int r=0;r<SEQ;r++) for(int c=0;c<N;c++)
    if(C[(size_t)r*N+c]!=REF[(size_t)r*N+c]) b++; return b; }

// 스케줄 하나를 돌리고 단계별 시간을 t[]에 남긴다.  qkv_mode: 0=하나씩, 1=순차 J분할
static int run_sched(int blkset,int qkv_mode,int mq,int mo,int m1,int m2,double *t){
  /* E361: 0=A(옛 한계의 규칙) 1=B(전부 (8,8)) 2=C(FFN2만 (8,8)) */
  /* E383: blkset 0..3 = (4,4)(8,4)(4,8)(8,8) 균일 적용, Kc는 블록별 규칙값 */
  static const int BI4[4]={4,8,4,8}, BJ4[4]={4,4,8,8};
  static const int F2KCr[4]={96,64,64,64};   /* E410: FFN2는 블록마다 다름 */
  static const int F2KC4[4]={96,64,64,64};
  const int QI=BI4[blkset], QJ=BJ4[blkset];
  const int F1I=BI4[blkset], F1J=BJ4[blkset];
  const int F2I=BI4[blkset], F2J=BJ4[blkset], F2KC=F2KC4[blkset];
  int bad=0; double t0;
  setblk(QI,QJ,48);
  set_in(H); set_ref(Wq[0],H);
  for(int a=0;a<3;a++) memset(Out[a],0,(size_t)SEQ*H);
  t0=now();
  if(qkv_mode==0) qkv_each(mq);
  else for(int p=0;p<3;p++) mm_split(Xin,Wq[p],Out[p],H,H,mq);
  t[0]=now()-t0;
  for(int p=0;p<3;p++) bad+=chk(Out[p],H);   /* 항상 셋 다 검사 */

  setblk(QI,QJ,48);
  set_in(H); set_ref(Wo,H); memset(Out[0],0,(size_t)SEQ*H);
  t0=now(); mm_split(Xin,Wo,Out[0],H,H,mo); t[1]=now()-t0; bad+=chk(Out[0],H);

  setblk(F1I,F1J,24);
  set_in(H); set_ref(W1,FF); memset(Out[0],0,(size_t)SEQ*FF);
  t0=now(); mm_split(Xin,W1,Out[0],H,FF,m1); t[2]=now()-t0; bad+=chk(Out[0],FF);

  setblk(F2I,F2J,F2KCr[blkset]);
  set_in(FF); set_ref(W2,H); memset(Out[0],0,(size_t)SEQ*H);
  t0=now(); mm_split(Xin,W2,Out[0],FF,H,m2); t[3]=now()-t0; bad+=chk(Out[0],H);
  return bad; }

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/mid6.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof(cs),&cs); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);  /* E374: 3가속기 판 — 문맥 셋 다 플러시 */
  for(int w=0;w<3;w++) for(int r=0;r<H;r++) for(int c=0;c<H;c++)
    Wq[w][(size_t)r*H+c]=(int8_t)((r*3+c*5)%7-3);
  for(int r=0;r<H;r++) for(int c=0;c<H;c++)  Wo[(size_t)r*H+c]=(int8_t)((r*3+c*5)%7-3);
  for(int r=0;r<H;r++) for(int c=0;c<FF;c++) W1[(size_t)r*FF+c]=(int8_t)((r*3+c*5)%7-3);
  for(int r=0;r<FF;r++) for(int c=0;c<H;c++) W2[(size_t)r*H+c]=(int8_t)((r*3+c*5)%7-3);

  double macs=3.0*SEQ*H*H+(double)SEQ*H*H+(double)SEQ*H*FF+(double)SEQ*FF*H;
  mark("=== E410: h1536 블록 전수 — 새 Kc 규칙 (3가속기+mem2) ===");
  mark("총 %.0f M MAC = 한 가속기 이론 %.2f ms", macs/1e6, macs/256.0/FREQ*1e3);
  mark("%-34s | %8s %8s %8s %8s | %9s | %s",
       "스케줄","QKV","출력","FFN1","FFN2","합계ms","향상");

  struct { const char *nm; int bs,qm,mq,mo,m1,m2; } S[] = {
    { "(4,4) m=2", 0,1,2,2,2,2 },
    { "(4,4) m=3", 0,1,3,3,3,3 },
    { "(8,4) m=2", 1,1,2,2,2,2 },
    { "(8,4) m=3", 1,1,3,3,3,3 },
    { "(4,8) m=2", 2,1,2,2,2,2 },
    { "(4,8) m=3", 2,1,3,3,3,3 },
    { "(8,8) m=2", 3,1,2,2,2,2 },
    { "(8,8) m=3", 3,1,3,3,3,3 },
  };
  double base=0;
  for(unsigned i=0;i<sizeof(S)/sizeof(S[0]);i++){
    double bt[4]={1e30,1e30,1e30,1e30}, bs=1e30; int bad=0;
    run_sched(S[i].bs,S[i].qm,S[i].mq,S[i].mo,S[i].m1,S[i].m2,bt);   /* 웜업 */
    for(int r=0;r<3;r++){ double t[4];
      bad+=run_sched(S[i].bs,S[i].qm,S[i].mq,S[i].mo,S[i].m1,S[i].m2,t);
      double s=t[0]+t[1]+t[2]+t[3];
      if(s<bs){ bs=s; for(int k=0;k<4;k++) bt[k]=t[k]; } }
    if(i==0) base=bs;
    mark("%-34s | %8.3f %8.3f %8.3f %8.3f | %9.3f | %5.2f배 %s",
         S[i].nm, bt[0]*1e3,bt[1]*1e3,bt[2]*1e3,bt[3]*1e3, bs*1e3,
         base/bs, bad?"FAIL":"PASS");
    if(bad) mark("    불일치 %d", bad);
  }
  mark("=== E410_DONE ===");
  return 0; }
