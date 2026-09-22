// mbdiag.c — 다중 블록 경합의 원인 좁히기 (E179).
//
// E178: 192³에서 블록을 여러 개 발행하면 틀리고, 블록마다 fence를 넣으면 맞는다.
// 스톡 라이브러리는 같은 크기를 fence 없이 맞게 계산한다. 무엇이 다른가.
//
// 후보 두 개를 가른다:
//   (a) 누산기 용량 — 내 블록은 I*J=64로 누산기를 **통째로** 쓴다. 연속 블록이
//       같은 영역을 덮으므로 이전 mvout을 앞지를 수 있다. 블록을 줄이면 나아야 한다.
//   (b) spad_id — 라이브러리는 재사용 시 1/2를 넘긴다. 0만 쓰는 내 판과 다르다.
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
#define MAXN 192
static int8_t A[MAXN*MAXN], B[MAXN*MAXN], C[MAXN*MAXN] __attribute__((aligned(64)));
static int8_t REF[MAXN*MAXN];
static int SZ, BLK, USE_ID;

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }

static void run(void){
  int T = SZ/16, nb = 0;
  grt_loop_ws_config(&AC1, (uint64_t)SZ, (uint64_t)SZ, (uint64_t)SZ);
  for (int i0=0; i0<T; i0+=BLK) for (int j0=0; j0<T; j0+=BLK, nb++) {
    int I = (i0+BLK<=T)?BLK:(T-i0), J = (j0+BLK<=T)?BLK:(T-j0);
    const void *a = A + (size_t)i0*16*SZ, *b = B + (size_t)j0*16;
    void *c = C + (size_t)i0*16*SZ + (size_t)j0*16;
    if (USE_ID) grt_loop_ws_id(&AC1, I,J,T, a,b,c, SZ,SZ,SZ, nb?2:1, nb?2:1);
    else        grt_loop_ws   (&AC1, I,J,T, a,b,c, SZ,SZ,SZ);
  }
  grt_fence();
}
static void fill(void){
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++){
    A[r*SZ+c]=(c==r)||(c==(r+1)%SZ); B[r*SZ+c]=(int8_t)((r*3+c*5)%7-3); }
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++)
    REF[r*SZ+c]=(int8_t)(B[r*SZ+c]+B[((r+1)%SZ)*SZ+c]); }
static int chk(void){ int b=0;
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++) if(C[r*SZ+c]!=REF[r*SZ+c]) b++; return b; }

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/mbdiag.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  grt_flush_ctx(&AC1);
  mark("=== 다중 블록 경합 원인 좁히기 (3i8f50) ===");
  mark("%5s %4s %5s %6s | %8s | %s", "크기","블록","개수","spad","시간ms","불일치/회차");
  SZ=192; fill();
  int blks[]={4,6,8};
  for(unsigned i=0;i<3;i++) for(USE_ID=0; USE_ID<2; USE_ID++){
    BLK=blks[i]; int T=SZ/16; int nb=((T+BLK-1)/BLK)*((T+BLK-1)/BLK);
    double best=1e30; int bad[3];
    for(int t=0;t<3;t++){ memset(C,0,(size_t)SZ*SZ);
      double t0=now(); run(); double dt=now()-t0; if(dt<best)best=dt; bad[t]=chk(); }
    mark("%4d³ %4d %5d %6s | %8.3f | %d %d %d %s", SZ, BLK, nb,
         USE_ID?"1/2":"0", best*1e3, bad[0],bad[1],bad[2],
         (bad[0]||bad[1]||bad[2])?"FAIL":"PASS");
  }
  mark("=== MBDIAG_DONE ===");
  return 0; }
