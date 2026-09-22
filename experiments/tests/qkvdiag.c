// qkvdiag.c — 비정방 형상에서 loop_ws가 왜 틀리는가 (E195 진단).
// M=128(8타일), N=K=768(48타일). 블록 (I,J)를 바꿔가며 불일치 수를 본다.
// 후보: (a) 누산기 I*J 한계  (b) 스크래치패드 I*K + K*J 한계  (c) K가 큰 것 자체
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
#define M 128
#define NN 768
static int8_t X[M*NN], W[NN*NN], O[M*NN] __attribute__((aligned(64)));
static int8_t REF[M*NN];
static int KK;                       // 시험할 K (원소 단위)
static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static void fill(void){
  memset(X,0,sizeof(X));
  for(int r=0;r<M;r++){ X[r*KK + r]=1; if(r+1<KK) X[r*KK + r+1]=1; }
  for(int r=0;r<KK;r++) for(int c=0;c<NN;c++) W[r*NN+c]=(int8_t)((r*3+c*5)%7-3);
  for(int r=0;r<M;r++) for(int c=0;c<NN;c++)
    REF[r*NN+c]=(int8_t)(W[r*NN+c] + ((r+1<KK)?W[(r+1)*NN+c]:0)); }
static int run(int BI,int BJ){
  int TI=M/16, TJ=NN/16, TK=KK/16;
  memset(O,0,sizeof(O));
  grt_loop_ws_config(&AC1, KK, NN, NN);
  for(int i0=0;i0<TI;i0+=BI) for(int j0=0;j0<TJ;j0+=BJ){
    int I=(i0+BI<=TI)?BI:(TI-i0), J=(j0+BJ<=TJ)?BJ:(TJ-j0);
    grt_loop_ws(&AC1, I,J,TK, X+(size_t)i0*16*KK, W+(size_t)j0*16,
                O+(size_t)i0*16*NN+(size_t)j0*16, KK, NN, NN); }
  grt_fence();
  int b=0; for(int r=0;r<M;r++) for(int c=0;c<NN;c++) if(O[r*NN+c]!=REF[r*NN+c]) b++;
  return b; }
int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/qkvdiag.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  grt_flush_ctx(&AC1);
  mark("=== 비정방 loop_ws 진단: M=128(8타일), N=768(48타일) ===");
  mark("%5s %3s %3s %5s %8s %8s | %8s", "K","I","J","I*J","A타일","B타일","불일치");
  int Ks[]={128, 256, 512, 768};
  int pairs[][2]={{8,4},{4,4},{8,2},{4,2},{2,2},{8,8}};
  for(unsigned k=0;k<sizeof(Ks)/sizeof(Ks[0]);k++){
    KK=Ks[k]; fill(); int TK=KK/16;
    for(unsigned q=0;q<sizeof(pairs)/sizeof(pairs[0]);q++){
      int BI=pairs[q][0], BJ=pairs[q][1];
      if(BI*BJ>64) continue;
      int bad=run(BI,BJ);
      mark("%5d %3d %3d %5d %8d %8d | %8d %s", KK,BI,BJ,BI*BJ, BI*TK, TK*BJ, bad,
           bad?"FAIL":"PASS");
    }
    mark(""); }
  mark("=== QKVDIAG_DONE ===");
  return 0; }
