// sync_scale.c — 동기화 비용이 문제되는 **작업 크기 범위**를 정량화한다 (E169).
//
// E168: 128³에서 조건변수 1.09배 / 스핀 1.88배 / 단일코어 교차 1.90배.
//   조건변수 오버헤드 약 1.2 ms는 **고정 비용**이므로 작업이 커지면 묻힌다.
//   교차점을 찾아야 "세밀한 작업에서만 단일 코어가 유리하다"는 주장이 정확해진다.
//
// 크기를 훑으며 세 방식을 비교한다. 단일 코어 교차 발행은 이 비트스트림(d9)에서
// 불가능하므로(가속기가 코어당 하나) 여기서는 코어별 두 방식만 재고,
// 단일 코어 교차 발행 1.90배는 2i8f50에서 잰 값을 대조로 쓴다.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <sys/mman.h>
#include "include/gemmini_rt.h"

static const grt_ctx I8 = { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_INT8 };
#define MAXN 192
static int8_t A1[MAXN*MAXN], B1[MAXN*MAXN], C1[MAXN*MAXN] __attribute__((aligned(64)));
static int8_t A2[MAXN*MAXN], B2[MAXN*MAXN], C2[MAXN*MAXN] __attribute__((aligned(64)));
static int SZ;

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }
static void pin(int c){ cpu_set_t s; CPU_ZERO(&s); CPU_SET(c,&s);
  pthread_setaffinity_np(pthread_self(),sizeof(s),&s); sched_yield(); }

// --- 조건변수 워커 ---
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cg = PTHREAD_COND_INITIALIZER, cd = PTHREAD_COND_INITIALIZER;
static int gf=0, df=0, qf=0;
static void *cv_worker(void *_){
  pin(1);
  for(;;){ pthread_mutex_lock(&mu);
    while(!gf && !qf) pthread_cond_wait(&cg,&mu);
    if(qf){ pthread_mutex_unlock(&mu); return NULL; }
    gf=0; pthread_mutex_unlock(&mu);
    grt_matmul(&I8,A2,B2,C2,SZ,SZ,SZ);
    pthread_mutex_lock(&mu); df=1; pthread_cond_signal(&cd); pthread_mutex_unlock(&mu); }
}
static void par_cv(void){
  pthread_mutex_lock(&mu); gf=1; df=0; pthread_cond_signal(&cg); pthread_mutex_unlock(&mu);
  grt_matmul(&I8,A1,B1,C1,SZ,SZ,SZ);
  pthread_mutex_lock(&mu); while(!df) pthread_cond_wait(&cd,&mu); pthread_mutex_unlock(&mu);
}
// --- 스핀 워커 ---
static volatile int sg=0, sd=0, sq=0;
static void *sp_worker(void *_){
  pin(1);
  for(;;){ while(!__atomic_load_n(&sg,__ATOMIC_ACQUIRE)){ if(sq) return NULL; }
    __atomic_store_n(&sg,0,__ATOMIC_RELAXED);
    grt_matmul(&I8,A2,B2,C2,SZ,SZ,SZ);
    __atomic_store_n(&sd,1,__ATOMIC_RELEASE); }
}
static void par_sp(void){
  __atomic_store_n(&sd,0,__ATOMIC_RELAXED); __atomic_store_n(&sg,1,__ATOMIC_RELEASE);
  grt_matmul(&I8,A1,B1,C1,SZ,SZ,SZ);
  while(!__atomic_load_n(&sd,__ATOMIC_ACQUIRE)){}
}
static void one(void){ grt_matmul(&I8,A1,B1,C1,SZ,SZ,SZ); }
static void seq(void){ grt_matmul(&I8,A1,B1,C1,SZ,SZ,SZ); grt_matmul(&I8,A2,B2,C2,SZ,SZ,SZ); }
static int chk(void){ int b=0;
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++){
    if(C1[r*SZ+c]!=B1[r*SZ+c])b++; if(C2[r*SZ+c]!=B2[r*SZ+c])b++; } return b; }
static double timeit(void (*fn)(void)){
  double best=1e30;
  for(int s=0;s<5;s++){ memset(C1,0,(size_t)SZ*SZ); memset(C2,0,(size_t)SZ*SZ);
    double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt; }
  return best;
}
int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/syncscale.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  pin(0); grt_flush_ctx(&I8);
  mark("=== 동기화 비용 vs 작업 크기 (d9, best-of-5) ===");
  mark("대조: 단일 코어 두 가속기 교차 발행 = 1.90배, 동기화 없음 (2i8f50)");
  mark("%6s %9s %9s %9s %9s %8s %8s", "크기","단독ms","순차ms","조건변수","스핀","조건변수","스핀");

  pthread_t tcv; pthread_create(&tcv,NULL,cv_worker,NULL); usleep(200000);
  int Ns[]={32,64,128,192};
  double cvres[4], spres[4], seqres[4];
  for(unsigned i=0;i<4;i++){
    SZ=Ns[i];
    for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++){
      A1[r*SZ+c]=(r==c)?1:0; B1[r*SZ+c]=(int8_t)((r+c)%7-3);
      A2[r*SZ+c]=(r==c)?1:0; B2[r*SZ+c]=(int8_t)((r+c)%5-2); }
    double t1=timeit(one); seqres[i]=timeit(seq); cvres[i]=timeit(par_cv);
    mark("  %4d³ %9.3f %9.3f %9.3f      —   %7.2f배      —",
         SZ, t1*1e3, seqres[i]*1e3, cvres[i]*1e3, seqres[i]/cvres[i]);
  }
  pthread_mutex_lock(&mu); qf=1; pthread_cond_signal(&cg); pthread_mutex_unlock(&mu);
  pthread_join(tcv,NULL);

  pthread_t tsp; pthread_create(&tsp,NULL,sp_worker,NULL); usleep(200000);
  for(unsigned i=0;i<4;i++){
    SZ=Ns[i];
    for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++){
      A1[r*SZ+c]=(r==c)?1:0; B1[r*SZ+c]=(int8_t)((r+c)%7-3);
      A2[r*SZ+c]=(r==c)?1:0; B2[r*SZ+c]=(int8_t)((r+c)%5-2); }
    spres[i]=timeit(par_sp); int b=chk();
    mark("  %4d³ 스핀 %8.3f ms  속도 %.2f배  정확성 %s",
         SZ, spres[i]*1e3, seqres[i]/spres[i], b?"FAIL":"PASS");
  }
  __atomic_store_n(&sq,1,__ATOMIC_RELEASE); __atomic_store_n(&sg,1,__ATOMIC_RELEASE);
  pthread_join(tsp,NULL);
  mark("");
  mark("조건변수 오버헤드 추정 = 조건변수시간 - 스핀시간:");
  for(unsigned i=0;i<4;i++) mark("  %4d³: %+.3f ms", Ns[i], (cvres[i]-spres[i])*1e3);
  mark("=== SYNCSCALE_DONE ===");
  return 0;
}
