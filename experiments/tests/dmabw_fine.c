// dmabw.c — 15 B/cycle 천장이 정말 시스템 버스인가, 독립 검증 (E188).
//
// E180~E184의 근거는 "matmul 트래픽의 집계 바이트율이 15.0에서 멈추고 SystemBus가
// 128비트 = 16 B/cycle이다"로 **정황**이다. matmul 특유의 한계일 가능성이 남아 있다.
//
// 연산을 빼고 **DMA만** 돌려 같은 천장이 나오는지 본다. 나오면 버스가 맞고,
// 더 높이 가면 15.0은 matmul 쪽 한계였던 것이므로 E180의 처방(버스를 넓혀라)이 흔들린다.
//
// mvin은 스크래치패드 주소를 16칸에 돌려 써서 WAR 정지를 피한다.
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
#define ROWS 16384                 // 4096행 x 192B = 786KB 소스 (L2 512KB보다 큼)
#define W    192
#define NTILE 2048                // 가속기당 옮길 타일 수
#define SLOTS 16
static int8_t SRC[3][ROWS*W] __attribute__((aligned(64)));
static int8_t DST[3][ROWS*W] __attribute__((aligned(64)));
static int NACC, FOOT;   // FOOT = 순회할 행 수(작업셋 크기)

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }

// 정확성: 타일 하나를 넣었다 빼서 비교 (전송이 실제로 일어나는지 확인)
static int verify(int a){
  grt_config_ld(&AC[a], W); grt_config_st(&AC[a], W);
  grt_mvin (&AC[a], SRC[a], 0, 16, 16);
  grt_mvout(&AC[a], DST[a], 0, 16, 16);
  grt_fence();
  int bad=0;
  for(int r=0;r<16;r++) for(int c=0;c<16;c++)
    if(DST[a][r*W+c]!=SRC[a][r*W+c]) bad++;
  return bad; }

static void mvin_only(void){
  for(int a=0;a<NACC;a++) grt_config_ld(&AC[a], W);
  for(int t=0;t<NTILE;t++)
    for(int a=0;a<NACC;a++)
      grt_mvin(&AC[a], SRC[a] + (size_t)((t*16)%(FOOT-16))*W, (t%SLOTS)*16, 16, 16);
  grt_fence(); }
static void mvout_only(void){
  for(int a=0;a<NACC;a++) grt_config_st(&AC[a], W);
  for(int t=0;t<NTILE;t++)
    for(int a=0;a<NACC;a++)
      grt_mvout(&AC[a], DST[a] + (size_t)((t*16)%(FOOT-16))*W, (t%SLOTS)*16, 16, 16);
  grt_fence(); }
static void both(void){
  for(int a=0;a<NACC;a++){ grt_config_ld(&AC[a], W); grt_config_st(&AC[a], W); }
  for(int t=0;t<NTILE;t++)
    for(int a=0;a<NACC;a++){
      grt_mvin (&AC[a], SRC[a] + (size_t)((t*16)%(FOOT-16))*W, (t%SLOTS)*16, 16, 16);
      grt_mvout(&AC[a], DST[a] + (size_t)((t*16)%(FOOT-16))*W, (t%SLOTS)*16, 16, 16); }
  grt_fence(); }

static double meas(void (*fn)(void)){
  double best=1e30;
  for(int t=0;t<5;t++){ double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt; }
  return best; }

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/dmabw_fine.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  for(int a=0;a<2;a++) grt_flush_ctx(&AC[a]);
  for(int a=0;a<2;a++) for(int i=0;i<ROWS*W;i++) SRC[a][i]=(int8_t)(i*7+a);

  mark("=== [2가속기 62.5MHz 256비트] 순수 DMA 절벽 정밀 측정 (E339) ===");
  mark("matmul 트래픽의 천장은 15.0 B/cycle이었다(E180). 연산 없이도 같은가?");
  for(int a=0;a<2;a++) mark("가속기%d 왕복 검증: 불일치 %d", a, verify(a));
  mark("가속기당 %d 타일(%d KB)을 옮기되, **훑는 작업셋 크기**를 바꾼다.", NTILE, NTILE*256/1024);
  mark("E207: projection(가중치 576KB)은 4.98 B/cyc, FFN(2.36MB)은 3.07을 달성한다.");
  mark("순수 DMA에서도 같은 하락이 보이면 B_dram을 작업셋의 함수로 쓸 수 있다.");
  mark("L2는 512 KB이므로 작업셋이 그 아래면 온칩, 위면 DRAM 경로다.");
  mark("matmul 실험(E180~)은 전부 L2 안이었으므로 비교 대상은 작은 작업셋이다.");
  mark("");
  mark("%8s %6s %5s | %9s %9s | %9s %9s | %9s", 
       "작업셋KB","가속기당","개수","mvin ms","B/cyc","mvout ms","B/cyc","둘다B/cyc");
  // E339: 절벽(가속기당 768KB -> 1536KB)을 촘촘히. 총 작업셋이 같은 m=1/m=2 짝을
  // 절벽 위아래로 여러 개 만든다. FOOT*192B = 가속기당 작업셋.
  int foots[]={2048, 4096, 5120, 6144, 8192, 10240, 12288, 14336, 16384};
  for(unsigned f=0; f<sizeof(foots)/sizeof(foots[0]); f++){
    FOOT=foots[f];
    for(NACC=1; NACC<=2; NACC++)   /* custom1 없음 */{
      mvin_only(); mvout_only(); both();          /* 웜업 */
      double ti=meas(mvin_only), to=meas(mvout_only), tb=meas(both);
      double by = (double)NTILE*256.0*NACC;
      mark("%8d %6d %5d | %9.3f %9.2f | %9.3f %9.2f | %9.2f",
           FOOT*W*NACC/1024, FOOT*W/1024, NACC,
           ti*1e3, by/(ti*FREQ), to*1e3, by/(to*FREQ), 2*by/(tb*FREQ));
    }
  }
  mark("=== DMAFINE_DONE ===");
  return 0; }
