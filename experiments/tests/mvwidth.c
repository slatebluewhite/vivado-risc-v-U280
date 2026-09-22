// mvwidth.c — 요청 폭이 달성 대역폭을 바꾸는가 (E226).
//
// E225에서 우연히 가속기 2개의 온칩 천장이 14.0 B/cycle로 나왔는데, E194의 같은 조건은
// 16.44였다. 차이는 접근 모양뿐이다 — E225는 16x16 타일(256B)을 하나씩 mvin 했고
// E194는 루프 FSM의 블록 적재였다.
//
// 하드웨어는 MAX_BLOCK_LEN = MAX_BYTES/DIM = 4 이므로 **한 mvin이 최대 4타일 폭**을 싣는다.
// 총 바이트를 고정하고 폭만 16/32/48/64 열로 바꿔 달성 대역폭을 잰다.
// 폭이 늘면 명령 수는 줄고 요청은 커진다 — 둘 다 유리한 방향이므로,
// 차이가 나면 "타일 단위로 쪼개 싣지 말라"는 실용 결론이 나온다.
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
#define FREQ 50.0e6
#define ROWS 2048
#define W    256                 /* 소스 행 폭(바이트). 작업셋 512KB -> L2 안 */
#define TOTB (2048*256)          /* 가속기당 옮길 총 바이트 = 512KB */
static int8_t SRC[3][ROWS*W] __attribute__((aligned(64)));
static int8_t DST[3][ROWS*W] __attribute__((aligned(64)));
static int NACC, COLS;
static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }
static int verify(int a){
  grt_config_ld(&AC[a], W); grt_config_st(&AC[a], W);
  grt_mvin (&AC[a], SRC[a], 0, COLS, 16);
  grt_mvout(&AC[a], DST[a], 0, COLS, 16);
  grt_fence();
  int bad=0;
  for(int r=0;r<16;r++) for(int c=0;c<COLS;c++)
    if(DST[a][r*W+c]!=SRC[a][r*W+c]) bad++;
  return bad; }
static void loop(void){
  /* 총 바이트 고정: COLS 폭 x 16행 = COLS*16 바이트를 한 명령이 옮긴다 */
  int per = COLS*16;
  int n = TOTB/per;
  for(int a=0;a<NACC;a++) grt_config_ld(&AC[a], W);
  for(int t=0;t<n;t++)
    for(int a=0;a<NACC;a++)
      grt_mvin(&AC[a], SRC[a] + (size_t)((t*16)%(ROWS-16))*W, (t%8)*16, COLS, 16);
  grt_fence(); }
int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/mvwidth.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  for(int a=0;a<3;a++) for(int i=0;i<ROWS*W;i++) SRC[a][i]=(int8_t)(i*7+a);
  mark("=== mvin 폭과 달성 대역폭 (총 바이트 고정 %d KB/가속기) ===", TOTB/1024);
  mark("하드웨어 MAX_BLOCK_LEN=4 -> 한 mvin이 최대 4타일(64열) 폭");
  COLS=16; for(int a=0;a<3;a++) mark("가속기%d 왕복 검증(16열): 불일치 %d", a, verify(a));
  mark("%5s %6s %8s %5s | %9s | %9s", "열","타일","명령수","개수","시간ms","B/cycle");
  int CS[]={16,32,48,64};
  for(unsigned q=0;q<4;q++){
    COLS=CS[q];
    for(NACC=1; NACC<=3; NACC++){
      loop();
      double best=1e30;
      for(int t=0;t<5;t++){ double t0=now(); loop(); double dt=now()-t0; if(dt<best)best=dt; }
      double by=(double)TOTB*NACC;
      mark("%5d %6d %8d %5d | %9.3f | %9.2f", COLS, COLS/16, TOTB/(COLS*16), NACC,
           best*1e3, by/(best*FREQ));
    } }
  mark("=== MVWIDTH_DONE ===");
  return 0; }
