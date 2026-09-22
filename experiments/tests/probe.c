// probe.c — E251의 부팅 간 흔들림을 싸게 표집한다 (E252).
// 두 점만 잰다: [768x576] (E247에서 η3=1.22, 지금 부팅 1.55 — 흔들리는 점)
//              [3072x576] (E247 2.73, 지금 2.67 — 안정한 점, 대조군)
// 실행 10초 안이라 프로그래밍 주기를 여러 번 돌릴 수 있다.
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
static int8_t A[M*KMAX]  __attribute__((aligned(64)));
static int8_t B[KMAX*NN] __attribute__((aligned(64)));
static int8_t C[M*NN]    __attribute__((aligned(64)));
static int8_t REF[M*NN];
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
int main(int argc,char**argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/probe.log","w");
  if(!g){perror("로그");exit(1);}
  if(mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof(cs),&cs); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  mark("=== E251 흔들림 표집 (3i8f50, M=128, N=576, (4,6), best-of-5) ===");
  mark("[768x576] η3: E247=1.22  E251 부팅=1.53~1.55   [3072x576] η3: 2.73 / 2.67 (안정)");
  int Ks[]={768,3072};
  for(int ki=0;ki<2;ki++){
    KK=Ks[ki];
    memset(A,0,(size_t)M*KK);
    for(int r=0;r<M;r++){ A[(size_t)r*KK+r]=1; A[(size_t)r*KK+r+1]=1; }
    for(int r=0;r<KK;r++) for(int c=0;c<NN;c++) B[(size_t)r*NN+c]=(int8_t)((r*3+c*5)%7-3);
    for(int r=0;r<M;r++) for(int c=0;c<NN;c++)
      REF[(size_t)r*NN+c]=(int8_t)(B[(size_t)r*NN+c]+B[(size_t)(r+1)*NN+c]);
    double roof=(double)M*KK*NN/256.0/FREQ, t[3]; int bad=0;
    for(int m=1;m<=3;m++){
      run(m); double b=1e30;
      for(int r=0;r<5;r++){ memset(C,0,(size_t)M*NN);
        double t0=now(); run(m); double d=now()-t0; if(d<b)b=d;
        for(int i=0;i<M*NN;i++) if(C[i]!=REF[i]){ bad++; break; } }
      t[m-1]=b; }
    mark("[%4dx%3d] | %8.3f %8.3f %8.3f ms | η %5.2f %5.2f %5.2f | %s",
         KK,NN,t[0]*1e3,t[1]*1e3,t[2]*1e3,
         t[0]/roof, t[1]/(roof/2), t[2]/(roof/3), bad?"FAIL":"PASS");
  }
  mark("=== PROBE_DONE ===");
  return 0; }
