// llamaffn.c — E316: Llama 계열 FFN 전체 + 블록 규칙의 빈 구간.
// 사전 등록: experiments/model/PREREG_E316.txt
//   A부: down [2816x1024] (N/K=0.36 — 규칙의 두 분기 어디에도 안 걸림) 블록 훑기
//   B부: gate+up+down 전체, 배분은 E315의 불균형 규칙대로
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
#define MM 128
#define H  1024
#define FF 2816
#define FREQ 50.0e6
static int8_t X[MM*H]   __attribute__((aligned(64)));   // 입력 [128 x 1024]
static int8_t Wg[H*FF]  __attribute__((aligned(64)));
static int8_t Wu[H*FF]  __attribute__((aligned(64)));
static int8_t Wd[FF*H]  __attribute__((aligned(64)));
static int8_t Hg[MM*FF] __attribute__((aligned(64)));   // gate 출력 = down 입력
static int8_t Hu[MM*FF] __attribute__((aligned(64)));
static int8_t Y[MM*H]   __attribute__((aligned(64)));
static int8_t Rg[MM*FF], Ry[MM*H];
static int BI,BJ,KCv;
static FILE *g;
static void mark(const char *f,...){ if(!g)return; va_list ap; va_start(ap,f);
  vfprintf(g,f,ap); va_end(ap); fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }

// 하나의 matmul [MM x K] x [K x N] 을 m대로 블록 라운드로빈 J분할
static void mm_jsplit(int m,const int8_t*A,const int8_t*W,int8_t*C,int K,int N){
  int TI=MM/16, Ntil=N/16, NB=(Ntil+BJ-1)/BJ;
  for(int a=0;a<m;a++) grt_loop_ws_config(&AC[a], K,N,N);
  int i0[3]={0,0,0}, jb[3], done[3];
  for(int a=0;a<m;a++){ jb[a]=a; done[a]=(a>=NB); }
  int left; do{ left=0;
    for(int a=0;a<m;a++){ if(done[a]) continue;
      int j0=jb[a]*BJ; int J=(j0+BJ<=Ntil)?BJ:(Ntil-j0);
      int I=(i0[a]+BI<=TI)?BI:(TI-i0[a]);
      grt_block_ksplit(&AC[a], I,J,K/16,KCv,
                       A+(size_t)i0[a]*16*K, W+(size_t)j0*16,
                       C+(size_t)i0[a]*16*N+(size_t)j0*16, K,N,N, 1);
      i0[a]+=BI;
      if(i0[a]>=TI){ i0[a]=0; jb[a]+=m; if(jb[a]>=NB) done[a]=1; }
      if(!done[a]) left++; }
  }while(left);
}
static void fill(void){
  memset(X,0,sizeof X);
  for(int r=0;r<MM;r++){ int p=r%(H-1); X[(size_t)r*H+p]=1; X[(size_t)r*H+p+1]=1; }
  for(int r=0;r<H;r++) for(int c=0;c<FF;c++){
    Wg[(size_t)r*FF+c]=(int8_t)((r*3+c*5)%7-3);
    Wu[(size_t)r*FF+c]=(int8_t)((r*5+c*3)%7-3); }
  for(int r=0;r<FF;r++) for(int c=0;c<H;c++) Wd[(size_t)r*H+c]=(int8_t)((r*3+c*5)%7-3);
  // 참조: Hg[r][c] = Wg[p][c]+Wg[p+1][c]
  for(int r=0;r<MM;r++){ int p=r%(H-1);
    for(int c=0;c<FF;c++) Rg[(size_t)r*FF+c]=(int8_t)(Wg[(size_t)p*FF+c]+Wg[(size_t)(p+1)*FF+c]); }
}
static int chk_g(void){ int b=0;
  for(size_t i=0;i<(size_t)MM*FF;i++) if(Hg[i]!=Rg[i]) b++; return b; }
// down의 참조는 Hg를 입력으로 하는 완전 행렬곱이라 CPU로 재계산하면 너무 느리다.
// 대신 m=1 결과를 기준으로 삼아 m=2·3이 같은지 본다 (분할 정확성 검사).
static int chk_y(void){ int b=0;
  for(size_t i=0;i<(size_t)MM*H;i++) if(Y[i]!=Ry[i]) b++; return b; }

static void setblk(int i,int j,int kc){ BI=i; BJ=j; KCv=kc; }
static double best_of(int m,void(*fn)(int),int reps){
  double b=1e30; fn(m); grt_fence();
  for(int r=0;r<reps;r++){ double t0=now(); fn(m); grt_fence(); double d=now()-t0; if(d<b)b=d; }
  return b; }
static void run_down(int m){ mm_jsplit(m,Hg,Wd,Y,FF,H); }
static void run_gate(int m){ mm_jsplit(m,X,Wg,Hg,H,FF); }

int main(int argc,char**argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/llamaffn.log","w");
  if(!g){perror("로그");exit(1);}
  if(mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof(cs),&cs); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  mark("=== E316 Llama FFN (hidden %d, inter %d), M=%d, best-of-5, 50MHz ===",H,FF,MM);
  fill();
  // gate를 규칙대로 한 번 돌려 Hg를 채운다 (down의 입력이자 정확성 기준)
  setblk(8,4,16); mm_jsplit(1,X,Wg,Hg,H,FF); grt_fence();
  mark("gate 규칙 설정 (8,4) Kc=16: 정확성 %s", chk_g()?"FAIL":"PASS");

  mark("");
  mark("--- A부: down [%dx%d] 블록 훑기 (N/K=%.2f — 규칙의 빈 구간) ---",FF,H,(double)H/FF);
  mark("%-18s %6s %5s | %9s %9s %9s | %s","블록","Kc","청크","m=1","m=2","m=3","정확성");
  struct { int I,J,kc; } CAND[]={{8,4,22},{8,4,16},{8,4,11},{4,4,44},{4,4,22},{4,4,16},{4,8,22},{4,8,16}};
  int first=1;
  for(unsigned i=0;i<sizeof(CAND)/sizeof(CAND[0]);i++){
    setblk(CAND[i].I,CAND[i].J,CAND[i].kc);
    if((H/16)%BJ) continue;
    if(KCv*(BI+BJ)>512) continue;
    double t[3]; int bad=0;
    for(int m=1;m<=3;m++){
      memset(Y,0,sizeof Y);
      t[m-1]=best_of(m,run_down,5);
      if(first&&m==1) memcpy(Ry,Y,sizeof Ry);
      else bad+=chk_y(); }
    first=0;
    mark("(%d,%d)              %6d %5d | %9.3f %9.3f %9.3f | %s",
         BI,BJ,KCv,(FF/16+KCv-1)/KCv,t[0]*1e3,t[1]*1e3,t[2]*1e3,bad?"FAIL":"PASS");
  }
  mark("");
  mark("--- B부: FFN 전체 (gate+up+down) ---");
  mark("gate/up: (8,4) Kc=16.  down: A부 최선을 손으로 넣지 않고 (4,4) Kc=44 사용");
  mark("%-24s | %9s %9s %9s | %9s","배분","gate","up","down","합계ms");
  for(int m=1;m<=3;m++){
    double bg=1e30,bu=1e30,bd=1e30;
    for(int r=0;r<5;r++){
      setblk(8,4,16);
      double t0=now(); mm_jsplit(m,X,Wg,Hg,H,FF); grt_fence(); double a=now()-t0;
      t0=now(); mm_jsplit(m,X,Wu,Hu,H,FF); grt_fence(); double b=now()-t0;
      setblk(4,4,44);
      t0=now(); mm_jsplit(m,Hg,Wd,Y,FF,H); grt_fence(); double c=now()-t0;
      if(a<bg)bg=a; if(b<bu)bu=b; if(c<bd)bd=c; }
    mark("m=%d 전 단계 J분할        | %9.3f %9.3f %9.3f | %9.3f",
         m,bg*1e3,bu*1e3,bd*1e3,(bg+bu+bd)*1e3);
  }
  mark("=== LLAMAFFN_DONE ===");
  return 0; }
