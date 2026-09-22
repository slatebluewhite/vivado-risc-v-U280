// layer.c — BERT-base 인코더 한 층의 matmul 전체를 세 가속기로 (E202).
//
// 이 트랙에서 확립한 규칙을 전부 적용한다:
//   K 분할 Kc=16 (E199)          — FFN2의 K=192타일을 다룰 수 있게
//   블록 (4,6)  (E191~E193)       — I·J=24<=32, 강도 최소 쪽, 안정 모양
//   i 내곽 순회 (E196)            — 훑히는 W를 고정
//   타일 단위 교대 발행 (E156)    — 세 가속기를 한 코어가 먹임
//
// 층 구성(seq=128, hidden=768, FFN=3072):
//   Q,K,V   [128x768]x[768x768] x3   — 독립 matmul 3개 -> 가속기 하나씩 (E195)
//   출력    [128x768]x[768x768]      — 단일 matmul -> J로 3분할
//   FFN1    [128x768]x[768x3072]     — 단일 matmul -> J로 3분할
//   FFN2    [128x3072]x[3072x768]    — 단일 matmul -> J로 3분할
// 어텐션(전체 연산의 2.7%)과 softmax/LayerNorm은 뺐다 — 이 비트스트림에 정규화 유닛이 없다.
//
// 정확성: 각 단계의 입력을 대각+한칸옆 패턴으로 재설정해 단계마다 독립 검증한다.
// 수치적으로 진짜 BERT는 아니지만 **형상·크기·메모리 트래픽은 정확히 같으므로
// 시간을 결정하는 요소는 동일하다.** (검증 없는 타이밍은 E176에서 데였다.)
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
#define H   768
#define FF  3072
#define FREQ 50.0e6
#define BI 4
#define BJ 6
#define KC 16
static int8_t Xin[SEQ*FF]  __attribute__((aligned(64)));   // 단계 입력 (FFN2용으로 크게)
static int8_t Wp[4][H*H]   __attribute__((aligned(64)));   // Wq,Wk,Wv,Wo
static int8_t W1[H*FF]     __attribute__((aligned(64)));
static int8_t W2[FF*H]     __attribute__((aligned(64)));
static int8_t Out[3][SEQ*FF] __attribute__((aligned(64)));
static int8_t REF[SEQ*FF];
static int NACC;
static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }

// 하나의 (i0, j0블록범위) 작업을 가속기 a가 수행
// --- 블록 단위 교대 발행용 반복자 ---------------------------------------
// E156: matmul을 통째로 교대하면 겹침 0%다(예약 스테이션이 감당 못 해 코어가
// 한쪽을 먹이다 멈춘다). 반드시 **블록 단위**로 번갈아 발행해야 한다.
// (원래 판은 for(a) do_range(a,..)로 가속기별 작업을 통째 발행했다 — 주석은
//  "타일 단위 교대"라 써 있었지만 코드가 아니었다.)
typedef struct { const int8_t *A,*B; int8_t *C;
                 int Ktil,Ntil,Kstride,Nstride,jb_first,jb_step; } item;
typedef struct { item it[3]; int nit,cur,jb,i0,done; } iter;

static void it_fix(iter *s){                 /* 빈 항목을 건너뛴다 */
  while(s->cur < s->nit && s->jb*BJ >= s->it[s->cur].Ntil){
    s->cur++; if(s->cur < s->nit) s->jb = s->it[s->cur].jb_first; }
  if(s->cur >= s->nit) s->done = 1; }
static void it_reset(iter *s){ s->cur=0; s->i0=0; s->done=(s->nit==0);
  if(s->nit){ s->jb = s->it[0].jb_first; it_fix(s); } }

static void it_step(int a, iter *s){
  if(s->done) return;
  item *w = &s->it[s->cur];
  int TI=SEQ/16;
  int j0=s->jb*BJ; int J=(j0+BJ<=w->Ntil)?BJ:(w->Ntil-j0);
  int I=(s->i0+BI<=TI)?BI:(TI-s->i0);
  grt_block_ksplit(&AC[a], I,J,w->Ktil,KC,
                   w->A+(size_t)s->i0*16*w->Kstride, w->B+(size_t)j0*16,
                   w->C+(size_t)s->i0*16*w->Nstride+(size_t)j0*16,
                   w->Kstride, w->Nstride, w->Nstride, 1);
  s->i0 += BI;                                  /* i 내곽 (E196) */
  if(s->i0 >= TI){ s->i0=0; s->jb += w->jb_step; it_fix(s); } }

static int INTERLEAVE = 1;
static void run_iters(iter *s){
  if(INTERLEAVE){                               /* 블록 단위 교대 */
    int left; do { left=0;
      for(int a=0;a<NACC;a++){ it_step(a,&s[a]); if(!s[a].done) left++; }
    } while(left);
  } else {                                      /* 가속기별 통째 발행(원래 판) */
    for(int a=0;a<NACC;a++) while(!s[a].done) it_step(a,&s[a]);
  }
  grt_fence(); }

// 세 개의 독립 matmul(Q,K,V) — 가속기 수와 무관하게 **항상 세 개**여야
// 총 작업량이 같아 1/2/3 비교가 성립한다. 부족하면 라운드로빈으로 겹쳐 맡는다.
static void stage_qkv(void){
  for(int a=0;a<NACC;a++) grt_loop_ws_config(&AC[a], H, H, H);
  iter s[3]; for(int a=0;a<NACC;a++) s[a].nit=0;
  for(int p=0;p<3;p++){ int a=p%NACC; item *w=&s[a].it[s[a].nit++];
    w->A=Xin; w->B=Wp[p]; w->C=Out[p]; w->Ktil=H/16; w->Ntil=H/16;
    w->Kstride=H; w->Nstride=H; w->jb_first=0; w->jb_step=1; }
  for(int a=0;a<NACC;a++) it_reset(&s[a]);
  run_iters(s); }

// 단일 matmul을 J로 나눠 여러 가속기에
static void stage_split(const int8_t *B,int Ktil,int Ntil,int Kstride,int Nstride){
  for(int a=0;a<NACC;a++) grt_loop_ws_config(&AC[a], Kstride, Nstride, Nstride);
  iter s[3];
  for(int a=0;a<NACC;a++){ s[a].nit=1; item *w=&s[a].it[0];
    w->A=Xin; w->B=B; w->C=Out[0]; w->Ktil=Ktil; w->Ntil=Ntil;
    w->Kstride=Kstride; w->Nstride=Nstride; w->jb_first=a; w->jb_step=NACC;
    it_reset(&s[a]); }
  run_iters(s); }

static void set_input(int cols){          // 대각 + 한 칸 옆
  memset(Xin,0,(size_t)SEQ*cols);
  for(int r=0;r<SEQ;r++){ Xin[(size_t)r*cols + r]=1; Xin[(size_t)r*cols + r+1]=1; } }
static void set_ref(const int8_t *B, int Nstride){
  for(int r=0;r<SEQ;r++) for(int c=0;c<Nstride;c++)
    REF[(size_t)r*Nstride+c]=(int8_t)(B[(size_t)r*Nstride+c]+B[(size_t)(r+1)*Nstride+c]); }
static int chk1(const int8_t *C, int Nstride){ int b=0;
  for(int r=0;r<SEQ;r++) for(int c=0;c<Nstride;c++)
    if(C[(size_t)r*Nstride+c]!=REF[(size_t)r*Nstride+c]) b++; return b; }
static int chk3(int Nstride){ int b=0;
  for(int a=0;a<NACC;a++) b+=chk1(Out[a],Nstride); return b; }

static void fillW(void){
  for(int w=0;w<4;w++) for(int r=0;r<H;r++) for(int c=0;c<H;c++)
    Wp[w][(size_t)r*H+c]=(int8_t)((r*3+c*5)%7-3);
  for(int r=0;r<H;r++) for(int c=0;c<FF;c++) W1[(size_t)r*FF+c]=(int8_t)((r*3+c*5)%7-3);
  for(int r=0;r<FF;r++) for(int c=0;c<H;c++)  W2[(size_t)r*H+c]=(int8_t)((r*3+c*5)%7-3); }

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/layer.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  fillW();
  double macs = 3.0*SEQ*H*H + (double)SEQ*H*H + (double)SEQ*H*FF + (double)SEQ*FF*H;
  mark("=== BERT-base 인코더 한 층의 matmul (seq=%d, hidden=%d, FFN=%d) ===", SEQ,H,FF);
  mark("규칙 적용: K분할 Kc=%d, 블록 (%d,%d), i 내곽, 타일 교대 발행", KC,BI,BJ);
  mark("총 %.0f M MAC = 이론 %.2f ms (어텐션·정규화 제외)", macs/1e6, macs/256.0/FREQ*1e3);
  mark("%5s | %9s %9s %9s %9s | %10s | %7s | %s",
       "개수","QKV","출력","FFN1","FFN2","합계ms","활용률","정확성");
  double base=0;
  for(int mode=1; mode>=0; mode--){
   INTERLEAVE=mode; base=0;
   mark("--- %s ---", mode? "블록 단위 교대 발행 (E156 준수)":"가속기별 통째 발행 (원래 판)");
   for(NACC=1; NACC<=3; NACC++){
    double t[4]; int bad=0;
    for(int rep=0; rep<2; rep++){        /* 1회 웜업 + 1회 측정 */
      /* QKV */
      set_input(H); set_ref(Wp[0],H);
      for(int a=0;a<3;a++) memset(Out[a],0,(size_t)SEQ*H);
      double t0=now(); stage_qkv(); t[0]=now()-t0;
      if(rep) { for(int p=0;p<3;p++) bad+=chk1(Out[p],H); }   /* 셋 다 검사 */
      /* 출력 projection */
      set_input(H); set_ref(Wp[3],H); memset(Out[0],0,(size_t)SEQ*H);
      t0=now(); stage_split(Wp[3], H/16, H/16, H, H); t[1]=now()-t0;
      if(rep) bad+=chk1(Out[0],H);
      /* FFN1 */
      set_input(H); set_ref(W1,FF); memset(Out[0],0,(size_t)SEQ*FF);
      t0=now(); stage_split(W1, H/16, FF/16, H, FF); t[2]=now()-t0;
      if(rep) bad+=chk1(Out[0],FF);
      /* FFN2 */
      set_input(FF); set_ref(W2,H); memset(Out[0],0,(size_t)SEQ*H);
      t0=now(); stage_split(W2, FF/16, H/16, FF, H); t[3]=now()-t0;
      if(rep) bad+=chk1(Out[0],H);
    }
    double tot=t[0]+t[1]+t[2]+t[3];
    if(NACC==1) base=tot;
    mark("%5d | %9.3f %9.3f %9.3f %9.3f | %10.3f | %6.1f%% | %s  (%.2f배)",
         NACC, t[0]*1e3,t[1]*1e3,t[2]*1e3,t[3]*1e3, tot*1e3,
         100.0*macs/256.0/(tot*FREQ), bad?"FAIL":"PASS", base/tot);
    if(bad) mark("    불일치 %d", bad);
   }
  }
  mark("=== LAYER_DONE ===");
  return 0; }
