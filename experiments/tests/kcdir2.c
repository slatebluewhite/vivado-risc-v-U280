// kcdir2.c — E404: E403의 대칭 실험. **세로로 긴** FFN2 형상(K/N=4)의 Kc 방향.
//
// E403은 가로로 긴 FFN1 세 형상(N/K=4)에서 "4청크"가 옳고 E400의 "합법 최대 Kc" 조항이
// 8~10% 손해임을 보였다. 그런데 E401의 FFN2 [3072x768]은 Kc 48(4청크) -> 64(3청크)가
// **+14.2%**로 정반대였다. 두 무리의 차이는 종횡비뿐이다.
// 여기서 FFN2 쪽 세 형상을 같은 방식으로 훑어 방향이 정말 종횡비로 갈리는지 본다.
//
//   [3072x 768] Ktil=192 : Kc 16 24 32 48 64  (청크 12 8 6 4 3)
//   [4096x1024] Ktil=256 : Kc 16 32 48 64     (청크 16 8 6 4)
//   [6144x1536] Ktil=384 : Kc 24 32 48 64     (청크 16 12 8 6)
// (8,8)에서 sp = Kc*16 <= 1024 이므로 Kc는 64가 상한이다 — 뒤 두 형상은 4청크에
//  도달조차 못 한다. 그 자체가 결과의 일부다.
//
// E400은 BERT-base FFN1에서 Kc 12->24 (4청크->2청크)가 좋다고 보고 규칙에 조항을 넣었고
// E401이 층에서 +6.2%로 확인했다. 그런데 E402가 BERT-large FFN1에서 정반대를 봤다:
//     [1024x4096] (8,8) m=3 : Kc16(4청크) 13.950 < Kc32(2청크) 15.069 < Kc64(1청크) 16.877
// 청크가 줄수록 단조로 나빠진다.
//
// E401의 비교는 블록과 Kc를 **같이** 바꿨고(=(8,4)Kc12 -> (8,8)Kc24) 두 수치는 서로 다른
// 세션에서 나왔다. 즉 "형상에 따라 방향이 다르다"와 "E401이 교차 세션 착시였다"가 아직
// 구분되지 않는다. 여기서는 **한 바이너리 한 회차 안에서** 세 FFN1 형상의 Kc를 훑어
// 그 둘을 가른다. 블록은 (8,8)로 고정한다.
//
//   [ 768x3072] Ktil=48 : Kc 6 8 12 16 24 48  (청크 8 6 4 3 2 1)
//   [1024x4096] Ktil=64 : Kc 8 16 32 64       (청크 8 4 2 1)
//   [1536x6144] Ktil=96 : Kc 12 16 24 32 48 96(청크 8 6 4 3 2 1)
//
// m=1도 같이 잰다 — E301에 따르면 청크 효과는 공유할 때만 나타나야 한다.
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

#define NACC 3
static const grt_ctx AC[NACC] = {
  { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_INT8 },
  { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_FP32 },
  { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_C1   },
};
#define MM   128
#define KMAX 6144
#define NMAX 1536
#define BMAX ((size_t)KMAX*NMAX)
#define FREQ 62.5e6
static int BI=8, BJ=8, KCv=16, KK, NN;
static int8_t A[MM*KMAX]  __attribute__((aligned(64)));
static int8_t B[BMAX]     __attribute__((aligned(64)));
static int8_t C[MM*NMAX]  __attribute__((aligned(64)));
static int8_t REF[MM*NMAX];

typedef struct { int jb,i0,step,done; } iter;
static void it_step(int a, iter *s){
  if(s->done) return;
  int TI=MM/16, Ntil=NN/16;
  int j0=s->jb*BJ; int J=(j0+BJ<=Ntil)?BJ:(Ntil-j0);
  int I=(s->i0+BI<=TI)?BI:(TI-s->i0);
  grt_block_ksplit(&AC[a], I,J,KK/16,KCv,
                   A+(size_t)s->i0*16*KK, B+(size_t)j0*16,
                   C+(size_t)s->i0*16*NN+(size_t)j0*16, KK,NN,NN, 1);
  s->i0 += BI;
  if(s->i0>=TI){ s->i0=0; s->jb+=s->step; if(s->jb*BJ>=Ntil) s->done=1; } }
static void run(int m){
  iter s[NACC];
  for(int a=0;a<m;a++){ grt_loop_ws_config(&AC[a], KK,NN,NN);
    s[a]=(iter){a,0,m,0}; if(a*BJ>=NN/16) s[a].done=1; }
  int left; do{ left=0;
    for(int a=0;a<m;a++){ it_step(a,&s[a]); if(!s[a].done) left++; } }while(left);
  grt_fence(); }

static FILE *g;
static void mark(const char *f,...){ if(!g)return; va_list ap; va_start(ap,f);
  vfprintf(g,f,ap); va_end(ap); fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }
static void fill(void){
  memset(A,0,(size_t)MM*KK);
  for(int r=0;r<MM;r++){ int p=r%(KK-1); A[(size_t)r*KK+p]=1; A[(size_t)r*KK+p+1]=1; }
  for(int r=0;r<KK;r++) for(int c=0;c<NN;c++) B[(size_t)r*NN+c]=(int8_t)((r*3+c*5)%7-3);
  for(int r=0;r<MM;r++){ int p=r%(KK-1);
    for(int c=0;c<NN;c++)
      REF[(size_t)r*NN+c]=(int8_t)(B[(size_t)p*NN+c]+B[(size_t)(p+1)*NN+c]); } }
static int chk(void){ int b=0;
  for(int r=0;r<MM;r++) for(int c=0;c<NN;c++)
    if(C[(size_t)r*NN+c]!=REF[(size_t)r*NN+c]) b++;
  return b; }

static void one(void){
  double t[NACC]; int bad=0;
  for(int mi=0;mi<2;mi++){
    int m = mi ? 3 : 1;
    run(m); double b=1e30;
    for(int r=0;r<3;r++){ memset(C,0,(size_t)MM*NN);
      double t0=now(); run(m); double d=now()-t0; if(d<b)b=d; bad+=chk(); }
    t[mi]=b; }
  int ch=(KK/16+KCv-1)/KCv;
  mark("[%5dx%5d] (%d,%d) Kc=%3d 청크%2d sp%5d | m=1 %9.3f  m=3 %9.3f | %5.2f배 | %s",
       KK,NN,BI,BJ,KCv,ch,KCv*(BI+BJ), t[0]*1e3,t[1]*1e3, t[0]/t[1],
       bad?"FAIL":"PASS"); }

int main(int argc,char**argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/kcdir2.log","w");
  if(!g){perror("로그");exit(1);}
  if(mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof(cs),&cs); sched_yield();
  for(int a=0;a<NACC;a++) grt_flush_ctx(&AC[a]);
  mark("=== E404: FFN2(세로로 긴) 세 형상의 Kc 방향 (블록 (8,8) 고정, best-of-3) ===");

  static const int KCs1[]={16,24,32,48,64,0};
  static const int KCs2[]={16,32,48,64,0};
  static const int KCs3[]={24,32,48,64,0};
  struct { int K,N; const int *kcs; const char *nm; } S[] = {
    { 3072, 768, KCs1, "BERT-base FFN2" },
    { 4096,1024, KCs2, "BERT-large FFN2" },
    { 6144,1536, KCs3, "h1536 FFN2" },
  };
  for(unsigned i=0;i<sizeof(S)/sizeof(S[0]);i++){
    KK=S[i].K; NN=S[i].N; fill();
    mark("--- %s [%dx%d] Ktil=%d ---", S[i].nm, KK, NN, KK/16);
    for(const int *p=S[i].kcs; *p; p++){ KCv=*p; one(); } }
  mark("=== E404_DONE ===");
  return 0; }
