// align.c — 양봉의 원인은 **주소 배치**인가 (E187).
//
// E186: (3,2)·(2,4)·(3,4) 세 모양이 실행마다 느린 모드(790~900us)와 빠른 모드(660~710us)를
// 오간다. 한 실행 안에서는 best-of-5가 전부 같은 무리에 든다.
// **"프로세스마다 다르고 실행 안에서는 일정하다"**는 성질은 물리 페이지 배치를 가리킨다 —
// Gemmini DMA는 자기 TLB로 가상주소를 훑으므로 물리 페이지 단편화가 전송 효율을 바꿀 수 있고,
// 그 배치는 프로세스 수명 동안 고정이다.
//
// [측정 전 예측]
//   한 프로세스 안에서 버퍼 오프셋만 바꿔 모드가 바뀌면 원인은 주소 배치다.
//   전부 같은 값이면 주소 배치가 아니라 프로세스 단위의 다른 무엇(예: TLB 워밍 상태)이다.
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

static const grt_ctx AC1 = { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_INT8 };
#define SZ 192
#define FREQ 50.0e6
#define PAD 65536
static int8_t Abuf[SZ*SZ+PAD], Bbuf[SZ*SZ+PAD], Cbuf[SZ*SZ+PAD] __attribute__((aligned(4096)));
static int8_t REF[SZ*SZ];
static int8_t *A, *B, *C;
static int BI, BJ;

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }

static void run(void){
  int T=SZ/16;
  grt_loop_ws_config(&AC1,(uint64_t)SZ,(uint64_t)SZ,(uint64_t)SZ);
  for(int i0=0;i0<T;i0+=BI) for(int j0=0;j0<T;j0+=BJ){
    int I=(i0+BI<=T)?BI:(T-i0), J=(j0+BJ<=T)?BJ:(T-j0);
    grt_loop_ws(&AC1, I,J,T, A+(size_t)i0*16*SZ, B+(size_t)j0*16,
                C+(size_t)i0*16*SZ+(size_t)j0*16, SZ,SZ,SZ); }
  grt_fence(); }
static void fill(void){
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++){
    A[r*SZ+c]=(c==r)||(c==(r+1)%SZ); B[r*SZ+c]=(int8_t)((r*3+c*5)%7-3); }
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++)
    REF[r*SZ+c]=(int8_t)(B[r*SZ+c]+B[((r+1)%SZ)*SZ+c]); }
static int chk(void){ int b=0;
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++) if(C[r*SZ+c]!=REF[r*SZ+c]) b++; return b; }

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/align.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  grt_flush_ctx(&AC1);
  double fl=(double)SZ*SZ*SZ/256.0;

  mark("=== 양봉의 원인은 주소 배치인가 (192³, 한 프로세스 안에서 오프셋만 변경) ===");
  mark("[예측] 오프셋에 따라 모드가 바뀌면 주소 배치가 원인. 전부 같으면 아니다.");
  mark("느린 모드 790~900us / 빠른 모드 660~710us (E186)");
  mark("%3s %3s %8s | %9s %7s | %s", "I","J","오프셋","시간us","k","정확성");
  int pairs[][2]={{3,2},{2,4},{3,4},{4,3}};   // 앞 셋은 양봉, (4,3)은 안정 대조군
  int offs[]={0,64,256,1024,4096,8192,16384,32768};
  for(unsigned p=0;p<sizeof(pairs)/sizeof(pairs[0]);p++){
    BI=pairs[p][0]; BJ=pairs[p][1];
    for(unsigned q=0;q<sizeof(offs)/sizeof(offs[0]);q++){
      A=Abuf+offs[q]; B=Bbuf+offs[q]; C=Cbuf+offs[q];
      fill();
      run(); run();
      double best=1e30; int worst=0;
      for(int t=0;t<5;t++){ memset(C,0,(size_t)SZ*SZ);
        double t0=now(); run(); double dt=now()-t0; if(dt<best)best=dt;
        int b=chk(); if(b>worst)worst=b; }
      mark("%3d %3d %8d | %9.2f %7.3f | %s",
           BI,BJ,offs[q], best*1e6, best*FREQ/fl, worst?"FAIL":"PASS");
    }
    mark("");
  }
  mark("=== ALIGN_DONE ===");
  return 0; }
