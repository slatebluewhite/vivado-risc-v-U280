// twostream.c — 스트림 두 개를 번갈아 읽으면 작업셋 의존성이 생기는가 (E233).
//
// E232: 총 트래픽 기준 달성 대역폭이 작업셋에 따라 23.6(정방 0.32MB) -> 14.6(proj 2.1MB)
// -> 7.07(FFN2 7.8MB)로 떨어진다. 그런데 E230의 순수 DMA는 2~24MB에서 12로 평탄했다.
//
// 차이는 matmul이 **A와 B를 서로 다른 스트라이드로 번갈아** 읽는다는 것이다.
// 스트림 1개와 2개를 같은 총 바이트로 비교한다. 2개에서만 하락하면 그것이 기제다.
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
#define ROWS 16384              /* 4 MB per stream per accel */
#define COLS 64                 /* 64 B 런 */
#define TOTB (2*1024*1024)      /* 가속기당 2 MB (스트림당 1 MB씩) */
static int8_t S1[3][ROWS*W] __attribute__((aligned(64)));
static int8_t S2[3][ROWS*W] __attribute__((aligned(64)));
static int8_t DST[3][64*W]  __attribute__((aligned(64)));
static int NACC, FOOT, TWO, WR;
static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }
static int verify(int a){
  grt_config_ld(&AC[a], W); grt_config_st(&AC[a], W);
  grt_mvin (&AC[a], S1[a], 0, COLS, 16);
  grt_mvout(&AC[a], DST[a], 0, COLS, 16);
  grt_fence();
  int bad=0;
  for(int r=0;r<16;r++) for(int c=0;c<COLS;c++)
    if(DST[a][r*W+c]!=S1[a][r*W+c]) bad++;
  return bad; }
static void loop(void){
  int per=COLS*16, n=TOTB/per;
  for(int a=0;a<NACC;a++){ grt_config_ld(&AC[a], W); grt_config_st(&AC[a], W); }
  for(int t=0;t<n;t++)
    for(int a=0;a<NACC;a++){
      /* TWO=0: 한 배열만.  TWO=1: 두 배열을 번갈아 (matmul의 A/B처럼) */
      int8_t *src = (TWO && (t&1)) ? S2[a] : S1[a];
      grt_mvin(&AC[a], src + (size_t)((t*16)%(FOOT-16))*W, (t%8)*16, COLS, 16);
      /* WR: matmul의 mvout처럼 쓰기를 섞는다. 비율은 WR분의 1 */
      if (WR && (t % WR)==0)
        grt_mvout(&AC[a], S2[a] + (size_t)((t*16)%(FOOT-16))*W, (t%8)*16, COLS, 16);
    }
  grt_fence(); }
int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/twostream.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  for(int a=0;a<3;a++) for(size_t i=0;i<(size_t)ROWS*W;i++){
    S1[a][i]=(int8_t)(i*7+a); S2[a][i]=(int8_t)(i*5+a); }
  mark("=== 스트림 1개 대 2개, 작업셋 훑기 (64 B 런, 가속기당 %d MB 이동) ===", TOTB/1048576);
  for(int a=0;a<3;a++) mark("가속기%d 왕복 검증: 불일치 %d", a, verify(a));
  mark("E233 2부: 쓰기를 섞으면 matmul의 7.07 B/cycle이 재현되는가");
  mark("%6s %5s %9s %5s | %9s | %9s", "스트림","쓰기","총작업셋KB","개수","시간ms","B/cycle");
  int F[]={1024, 4096, 16384};
  int WRS[]={0, 16, 4};               /* 쓰기 없음 / 16회당 1 / 4회당 1 */
  for(int tw=1; tw<2; tw++){
    TWO=tw;
    for(unsigned wq=0; wq<3; wq++){
    WR=WRS[wq];
    for(unsigned q=0;q<3;q++){
      FOOT=F[q];
      for(NACC=1; NACC<=3; NACC+=2){
        loop();
        double best=1e30;
        for(int t=0;t<3;t++){ double t0=now(); loop(); double dt=now()-t0; if(dt<best)best=dt; }
        double by=(double)TOTB*NACC;
        /* 작업셋: 1스트림이면 FOOT행, 2스트림이면 2배 */
        long ws = (long)FOOT*W*NACC*(TWO?2:1);
        double wb = WR ? by/WR : 0.0;         /* 쓰기 바이트 */
        mark("%6d %5s %9ld %5d | %9.3f | %9.2f", TWO+1,
             WR? (WR==16?"1/16":"1/4") : "없음", ws/1024, NACC,
             best*1e3, (by+wb)/(best*FREQ));
      } }
    mark(""); } }
  mark("=== TWOSTREAM_DONE ===");
  return 0; }
