// sched_xl.c — 본 적 없는 워크로드(hidden 1536 / FFN 6144)에 규칙+모델 전체 적용 (E284).
// 사전 등록: experiments/model/PREREG_E284.txt   예측 161.1 ms, 1.86배.
// K=6144는 모델 유효 범위(4096)를 넘는 외삽이다.
// E275는 단계 측정을 합산했을 뿐 조립 층을 재지 않았고, E276 정정으로 FFN2 우승자가 바뀌었다.
// 최종 규칙(E278)의 단계별 선택:
//   [1024x1024] N/K=1   -> (8,4) 청크2 Kc=32   (전치 (4,8)은 4.514로 열세)
//   [1024x4096] N/K=4   -> (8,4) 청크4 Kc=16   (전치 (4,8)은 28.316로 열세)
//   [4096x1024] K/N=4   -> 예외: (4,4)·(4,8) 둘 다 재고 (4,8) 청크8 Kc=32 채택
// 단계 합산 예측: 4*4.170 + 20.304 + 31.378 = 68.36 ms
// E273 훑기의 단계별 우승자:
//   [768x768]  (QKV·출력) : (8,4) Kc=32  청크2  -> 2.226 ms
//   [768x3072] (FFN1)     : (8,4) Kc=12  청크4  -> 9.280 ms
//   [3072x768] (FFN2)     : (4,4) Kc=48  청크4  -> 14.556 ms
// 예측 합계 3*2.226 + 2.226 + 9.280 + 14.556 = 32.74 ms
// E260/E261: (8,4)가 (4,6)보다 m=3에서 항상 빠르고(최대 23%), 그 이유는 N 의존성을
// 없애기 때문이다. E239의 헤드라인 1.43배는 (4,6)으로 잰 것이므로 여유가 있다.
// 스케줄 결론(FFN은 가속기 2개)이 더 나은 블록에서도 성립하는지도 함께 본다.
// BERT 형상의 N은 768과 3072 -> Ntil 48, 192 -> BJ=4면 nj=12, 48로 전부 6의 배수.
//
// E237: 세 가속기로 층을 1.47배 (블록 단위 교대 발행, 모든 단계 m=3).
// E238이 두 가지를 바꾸라고 한다:
//   (1) 독립 matmul 세 개(QKV)를 하나씩 맡기지 말고, 하나씩 전 가속기로 쪼개 차례로 (27% 빠름)
//   (2) FFN1은 m=3이 m=2보다 느리다 (16.4 -> 17.6 ms). FFN2는 변화 없음.
// 세 스케줄을 같은 바이너리·같은 회차에서 비교한다:
//   S1: 가속기 1개                    (기준)
//   S2: E237 판 — QKV 하나씩, 나머지 J분할 m=3
//   S3: E238 판 — QKV 순차 J분할 m=3, 출력 m=3, FFN1/FFN2 m=2
//   S4: 전부 순차 J분할 m=3           (FFN의 m=2 결정만 분리해 보려고)
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
  const int QJ = blkset==1 ? 8 : 4, F1J = blkset==1 ? 8 : 4;
  const int F2J = blkset==0 ? 4 : 8, F2KC = blkset==0 ? 32 : 64;
  int bad=0; double t0;
  setblk(8,QJ,24);
  set_in(H); set_ref(Wq[0],H);
  for(int a=0;a<3;a++) memset(Out[a],0,(size_t)SEQ*H);
  t0=now();
  if(qkv_mode==0) qkv_each(mq);
  else for(int p=0;p<3;p++) mm_split(Xin,Wq[p],Out[p],H,H,mq);
  t[0]=now()-t0;
  for(int p=0;p<3;p++) bad+=chk(Out[p],H);   /* 항상 셋 다 검사 */

  setblk(8,QJ,24);
  set_in(H); set_ref(Wo,H); memset(Out[0],0,(size_t)SEQ*H);
  t0=now(); mm_split(Xin,Wo,Out[0],H,H,mo); t[1]=now()-t0; bad+=chk(Out[0],H);

  setblk(8,F1J,24);
  set_in(H); set_ref(W1,FF); memset(Out[0],0,(size_t)SEQ*FF);
  t0=now(); mm_split(Xin,W1,Out[0],H,FF,m1); t[2]=now()-t0; bad+=chk(Out[0],FF);

  setblk(8,F2J,F2KC);
  set_in(FF); set_ref(W2,H); memset(Out[0],0,(size_t)SEQ*H);
  t0=now(); mm_split(Xin,W2,Out[0],FF,H,m2); t[3]=now()-t0; bad+=chk(Out[0],H);
  return bad; }

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/sched.log","w");
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
  mark("=== E361: mem2 판 hidden1536 층 — 옛 규칙 대 새 규칙, 62.5MHz, best-of-3 ===");
  mark("A=(8,4) 전부 | B=전부 (8,8) | C=FFN2만 (8,8)Kc64.  FFN2=[6144x1536], 합성에서 +34.2%%였다");
  mark("총 %.0f M MAC = 한 가속기 이론 %.2f ms", macs/1e6, macs/256.0/FREQ*1e3);
  mark("%-34s | %8s %8s %8s %8s | %9s | %s",
       "스케줄","QKV","출력","FFN1","FFN2","합계ms","향상");

  struct { const char *nm; int bs,qm,mq,mo,m1,m2; } S[] = {
    { "A 옛규칙  m=1",          0,1,1,1,1,1 },
    { "C FFN2만(8,8) m=1",      2,1,1,1,1,1 },
    { "A 옛규칙  m=2",          0,1,2,2,2,2 },
    { "B 새규칙(8,8) m=2",      1,1,2,2,2,2 },
    { "C FFN2만(8,8) m=2",      2,1,2,2,2,2 },
    { "A 옛규칙 m=3", 0,1,3,3,3,3 },
    { "B 새규칙(8,8) m=3", 1,1,3,3,3,3 },
    { "C FFN2만(8,8) m=3", 2,1,3,3,3,3 },
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
  mark("=== MEM2MID_DONE ===");
  return 0; }
