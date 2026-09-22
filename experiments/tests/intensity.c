// intensity.c — 산술 강도를 낮추면 다중 가속기가 그만큼 빨라지는가 (E191).
//
// E190: 가속기 하나의 수요가 8 B/연산사이클로 고정이라 둘이면 공급 15를 넘어 포화한다.
// 그런데 8은 (4,4) 블록의 값이다. 일반식을 세우면
//     바이트 = 256·(T³/J + T³/I + T²),  연산 = 16·T³ cycle
//     강도  = 16·(1/I + 1/J + 1/T)  B/연산사이클
// 이므로 블록을 키우면 수요가 준다. 누산기 절반 규칙(I·J <= 32, E179) 안에서:
//     (4,4) 8.00   (8,4)/(4,8) 6.00   (8,2)/(2,8) 10.00
//
// [측정 전 예측]
//   대역폭에 묶인 영역(N>=2)에서는 시간이 바이트에 비례하므로
//   (8,4)가 (4,4)보다 약 25% 빨라야 하고 (8,2)는 25% 느려야 한다.
//   가속기 하나에서는 대역폭에 안 묶이므로 차이가 작아야 한다(E183의 역전과 같은 구조).
//
// 주의: (8,4)는 I*J=32로 누산기 절반 규칙의 **경계**다. E179는 25 통과 / 36 실패까지만
// 확인했으므로 32는 미검증이다. 정확성 검사가 이번 실험의 절반이다.
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
#define SZ 512
#define FREQ 50.0e6
static int8_t A[3][SZ*SZ], B[3][SZ*SZ], C[3][SZ*SZ] __attribute__((aligned(64)));
static int8_t REF[SZ*SZ];
static int BI, BJ, NACC;

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }
static double bytes_one(void){
  int T=SZ/16; double b=0;
  for(int i0=0;i0<T;i0+=BI) for(int j0=0;j0<T;j0+=BJ)
    b += (double)(BI*T + T*BJ + BI*BJ)*256.0;
  return b; }
typedef struct { int i0,j0,done; } blk_cur;
static void blk_step(int a, blk_cur *s){
  if (s->done) return;
  int T=SZ/16;
  grt_loop_ws(&AC[a], BI,BJ,T, A[a]+(size_t)s->i0*16*SZ, B[a]+(size_t)s->j0*16,
              C[a]+(size_t)s->i0*16*SZ+(size_t)s->j0*16, SZ,SZ,SZ);
  s->j0+=BJ; if(s->j0>=T){ s->j0=0; s->i0+=BI; if(s->i0>=T) s->done=1; } }
static void run(void){
  for(int a=0;a<NACC;a++) grt_loop_ws_config(&AC[a],(uint64_t)SZ,(uint64_t)SZ,(uint64_t)SZ);
  blk_cur s[3]={{0,0,0},{0,0,0},{0,0,0}}; int left=NACC;
  while(left>0){ left=0; for(int a=0;a<NACC;a++){ blk_step(a,&s[a]); if(!s[a].done) left++; } }
  grt_fence(); }
static void fill(void){
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++){
    int8_t a=(c==r)||(c==(r+1)%SZ), b=(int8_t)((r*3+c*5)%7-3);
    for(int k=0;k<3;k++){ A[k][r*SZ+c]=a; B[k][r*SZ+c]=b; } }
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++)
    REF[r*SZ+c]=(int8_t)(B[0][r*SZ+c]+B[0][((r+1)%SZ)*SZ+c]); }
static int chk(void){ int b=0;
  for(int a=0;a<NACC;a++) for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++)
    if(C[a][r*SZ+c]!=REF[r*SZ+c]) b++; return b; }

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/intensity.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  fill();

  mark("=== 산술 강도를 낮추면 다중 가속기가 빨라지는가 (512³, T=32, best-of-3) ===");
  mark("강도 = 16(1/I + 1/J + 1/T) B/연산사이클.  공급 15 B/cycle.");
  mark("[예측] N>=2에서 (8,4)가 (4,4)보다 약 25%% 빠르고 (8,2)는 약 25%% 느리다.");
  mark("[주의] (8,4)는 I*J=32로 누산기 절반 규칙의 경계 — 정확성 미검증 구간이다.");
  mark("%3s %3s %6s %6s %5s | %10s %10s | %7s | %8s | %s",
       "I","J","I*J","강도","개수","시간ms","matmul당","처리량","B/cyc","정확성");
  int pairs[][2]={{4,4},{8,4},{4,8},{8,2},{2,8}};
  double b44[4]={0,0,0,0};
  for(unsigned q=0;q<sizeof(pairs)/sizeof(pairs[0]);q++){
    BI=pairs[q][0]; BJ=pairs[q][1];
    double b1=bytes_one(), inten=16.0*(1.0/BI+1.0/BJ+1.0/(SZ/16));
    for(NACC=1; NACC<=3; NACC++){
      run();
      double best=1e30; int worst=0;
      for(int t=0;t<3;t++){ for(int a=0;a<3;a++) memset(C[a],0,(size_t)SZ*SZ);
        double t0=now(); run(); double dt=now()-t0; if(dt<best)best=dt;
        int b=chk(); if(b>worst)worst=b; }
      double per=best/NACC;
      if(q==0) b44[NACC]=per;
      mark("%3d %3d %6d %6.2f %5d | %10.3f %10.3f | %6.2fx | %8.2f | %s",
           BI,BJ,BI*BJ,inten,NACC, best*1e3, per*1e3,
           b44[NACC]/per, b1*NACC/(best*FREQ), worst?"FAIL":"PASS");
    }
    mark("");
  }
  mark("(처리량 열은 같은 가속기 수에서 (4,4) 대비 배수)");
  mark("=== INTENSITY_DONE ===");
  return 0; }
