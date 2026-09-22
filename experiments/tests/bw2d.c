// bwext.c — B_dram(ws) 곡선을 16 MB까지 확장 (E229).
//
// 모델의 DRAM 항은 5점 보간(576KB~4.6MB)이고 9 MB는 추측값이다.
// BERT 워크로드가 7.8~8.3 MB에 있어 **거기서 외삽**하고 있다 — 없애야 한다.
//
// mvin만 돈다(mvout 없이). 총 바이트를 고정하고 훑는 작업셋만 키운다.
// E226에 따라 mvin은 4타일(64열) 폭으로 — 좁게 실으면 명령률에 묶여 대역폭을 못 잰다.
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
#define W    256
#define ROWS 32768               /* 8 MB/가속기 */
static int COLS;
#define TOTB (4*1024*1024)       /* 가속기당 4 MB를 옮긴다 */
static int8_t SRC[3][ROWS*W] __attribute__((aligned(64)));
static int8_t DST[3][64*W] __attribute__((aligned(64)));
static int NACC, FOOT;
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
  int per = COLS*16, n = TOTB/per;
  for(int a=0;a<NACC;a++) grt_config_ld(&AC[a], W);
  for(int t=0;t<n;t++)
    for(int a=0;a<NACC;a++)
      grt_mvin(&AC[a], SRC[a] + (size_t)((t*16)%(FOOT-16))*W, (t%8)*16, COLS, 16);
  grt_fence(); }
int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/bwext.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  for(int a=0;a<3;a++) for(size_t i=0;i<(size_t)ROWS*W;i++) SRC[a][i]=(int8_t)(i*7+a);
  mark("=== 유효 대역폭 = f(런 길이, 작업셋) 2차원 (E230) ===");
  mark("E229: 곡선이 런 길이의 함수임이 드러났다. 2차원으로 재서 법칙으로 만든다.");
  COLS=64; for(int a=0;a<3;a++) mark("가속기%d 왕복 검증(64열): 불일치 %d", a, verify(a));
  mark("%6s %9s %5s | %9s | %9s", "런B","총작업셋KB","개수","시간ms","B/cycle");
  int CS[]={16,32,64};
  int F[]={2048, 8192, 32768};      /* 가속기당 512KB / 2MB / 8MB */
  for(unsigned c=0;c<3;c++){
    COLS=CS[c];
    for(unsigned q=0;q<3;q++){
      FOOT=F[q];
      for(NACC=1; NACC<=3; NACC+=2){
        loop();
        double best=1e30;
        for(int t=0;t<3;t++){ double t0=now(); loop(); double dt=now()-t0; if(dt<best)best=dt; }
        double by=(double)TOTB*NACC;
        mark("%6d %9d %5d | %9.3f | %9.2f",
             COLS, FOOT*W*NACC/1024, NACC, best*1e3, by/(best*FREQ));
      } }
    mark(""); }
  mark("=== BWEXT_DONE ===");
  return 0; }
