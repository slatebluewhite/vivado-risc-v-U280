// dist.c — E251/E252 열화가 "상태 이동"인가 "모드 비율 변화"인가 (E254).
//
// E253: 동일 연산(출력 projection, J분할 m=3)이 한 프로세스 안에서 20% 흩어졌다.
// E239에서는 같은 자리가 1.4%였다. 평균만이 아니라 **분산**이 커졌다.
//
// 그렇다면 두 가지가 구별된다:
//   (가) 빠른 모드가 사라졌다        -> 100회 최솟값이 여전히 느린 값
//   (나) 빠른 모드의 비율이 줄었다   -> 100회 중 몇 번은 E247의 1.80ms가 나온다
// best-of-3/5로는 (나)를 놓칠 수 있다. 100회를 전부 보고 히스토그램을 찍는다.
// [3072x576]은 안정 구간이라 대조군이다.
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
#define M 128
#define NN 576
#define KMAX 3072
#define FREQ 50.0e6
#define BI 4
#define BJ 6
#define KC 16
#define REP 100
static int8_t A[M*KMAX]  __attribute__((aligned(64)));
static int8_t B[KMAX*NN] __attribute__((aligned(64)));
static int8_t C[M*NN]    __attribute__((aligned(64)));
static int8_t REF[M*NN];
static double T[REP];
static int KK;
typedef struct { int jb,i0,step,done; } iter;
static void it_step(int a, iter *s){
  if(s->done) return;
  int TI=M/16, Ntil=NN/16;
  int j0=s->jb*BJ; int J=(j0+BJ<=Ntil)?BJ:(Ntil-j0);
  int I=(s->i0+BI<=TI)?BI:(TI-s->i0);
  grt_block_ksplit(&AC[a], I,J,KK/16,KC,
                   A+(size_t)s->i0*16*KK, B+(size_t)j0*16,
                   C+(size_t)s->i0*16*NN+(size_t)j0*16, KK,NN,NN, 1);
  s->i0 += BI;
  if(s->i0>=TI){ s->i0=0; s->jb+=s->step; if(s->jb*BJ>=Ntil) s->done=1; } }
static void run(int m){
  iter s[3];
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
static int cmp(const void*a,const void*b){ double d=*(const double*)a-*(const double*)b;
  return d<0?-1:(d>0?1:0); }
int main(int argc,char**argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/dist.log","w");
  if(!g){perror("로그");exit(1);}
  if(mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof(cs),&cs); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  mark("=== 분포 (3i8f50, M=128, N=576, (4,6), 각 %d회) ===",REP);
  mark("E247 상태의 [768x576] m=3 = 1.804 ms. 그 값이 100회 중 나타나는가?");
  int Ks[]={768,3072};
  for(int ki=0;ki<2;ki++){
    KK=Ks[ki];
    memset(A,0,(size_t)M*KK);
    for(int r=0;r<M;r++){ A[(size_t)r*KK+r]=1; A[(size_t)r*KK+r+1]=1; }
    for(int r=0;r<KK;r++) for(int c=0;c<NN;c++) B[(size_t)r*NN+c]=(int8_t)((r*3+c*5)%7-3);
    for(int r=0;r<M;r++) for(int c=0;c<NN;c++)
      REF[(size_t)r*NN+c]=(int8_t)(B[(size_t)r*NN+c]+B[(size_t)(r+1)*NN+c]);
    for(int m=1;m<=3;m++){
      run(m); int bad=0;
      for(int r=0;r<REP;r++){ memset(C,0,(size_t)M*NN);
        double t0=now(); run(m); T[r]=now()-t0;
        for(int i=0;i<M*NN;i++) if(C[i]!=REF[i]){ bad++; break; } }
      double raw[REP]; memcpy(raw,T,sizeof(T));
      qsort(T,REP,sizeof(double),cmp);
      double mn=T[0], md=T[REP/2], mx=T[REP-1];
      double sum=0; for(int r=0;r<REP;r++) sum+=T[r];
      mark("[%4dx%3d] m=%d | 최소 %7.3f  5%% %7.3f  중앙 %7.3f  95%% %7.3f  최대 %7.3f ms"
           " | 평균 %7.3f  최대/최소 %5.2f  %s",
           KK,NN,m, mn*1e3, T[REP/20]*1e3, md*1e3, T[REP*19/20]*1e3, mx*1e3,
           sum/REP*1e3, mx/mn, bad?"FAIL":"PASS");
      /* 최소값 기준 5% 폭 히스토그램 */
      if(m==3){ int h[12]={0};
        for(int r=0;r<REP;r++){ int b=(int)((T[r]/mn-1.0)/0.05); if(b>11)b=11; h[b]++; }
        char buf[256]; int p=0;
        for(int b=0;b<12;b++) p+=snprintf(buf+p,sizeof(buf)-p,"%d ",h[b]);
        mark("           히스토그램(최소값의 +0%%,+5%%,...,+55%% 이상): %s",buf);
        /* 실행 순서대로 앞 20개 — 표류인지 무작위인지 */
        p=0; for(int r=0;r<20;r++) p+=snprintf(buf+p,sizeof(buf)-p,"%.2f ",raw[r]*1e3);
        mark("           실행 순서 앞 20개: %s",buf); }
    }
  }
  mark("=== DIST_DONE ===");
  return 0; }
