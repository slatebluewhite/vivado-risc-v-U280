// twocore.c — **코어별 가속기 병렬** vs 한 코어가 둘을 교차 발행 (E168).
//
// 지금까지 잰 1.9배는 "같은 코어에서 순차 vs 교차"였다.
// 사용자 지적: 그것이 "코어1-가속기1 / 코어2-가속기2"보다 빠르다는 뜻은 아니다. 맞다.
//
// d9(rocket64b2gem16wf40)는 **두 타일 각각에 INT8 16x16**을 갖고,
// E127에서 양쪽 코어 모두 RoCC 응답이 확인됐다. 그래서 이 비교가 가능하다.
//
//   (a) 한 코어에서 순차 두 개   — 기준선
//   (b) 두 코어 병렬(스레드 2개) — 코어별 가속기
//
// 이론상 (b)가 유리해야 한다: 명령 발행 엔진이 둘이다.
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

// d9는 두 타일 모두 custom3 + INT8 16x16
static const grt_ctx I8 = { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_INT8 };

#define SZ 128
static int8_t A1[SZ*SZ], B1[SZ*SZ], C1[SZ*SZ] __attribute__((aligned(64)));
static int8_t A2[SZ*SZ], B2[SZ*SZ], C2[SZ*SZ] __attribute__((aligned(64)));

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }
static void pin(int c){ cpu_set_t s; CPU_ZERO(&s); CPU_SET(c,&s);
  pthread_setaffinity_np(pthread_self(),sizeof(s),&s); sched_yield(); }

// 스레드를 **측정 구간 밖에서** 한 번만 만든다.
// 앞선 판은 pthread_create/join이 타이밍 안에 있어 40MHz에서 수 ms를 먹었다 —
// 가속기 병렬성이 아니라 스레드 생성 비용을 잰 셈이었다(E168 1차, 무효).
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv_go = PTHREAD_COND_INITIALIZER, cv_done = PTHREAD_COND_INITIALIZER;
static int go_flag = 0, done_flag = 0, quit_flag = 0;

// 스핀 기반 — 조건변수(futex+컨텍스트 스위치)는 동기화 중 가장 비싼 축이다.
// 공정한 비교를 위해 최선의 동기화도 함께 잰다.
static volatile int spin_go = 0, spin_done = 0, spin_quit = 0;
static void *spin_worker(void *_){
  pin(1);
  for(;;){
    while (!__atomic_load_n(&spin_go, __ATOMIC_ACQUIRE)) { if (spin_quit) return NULL; }
    __atomic_store_n(&spin_go, 0, __ATOMIC_RELAXED);
    grt_matmul(&I8, A2, B2, C2, SZ,SZ,SZ);
    __atomic_store_n(&spin_done, 1, __ATOMIC_RELEASE);
  }
}
static void par_spin(void){
  __atomic_store_n(&spin_done, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&spin_go, 1, __ATOMIC_RELEASE);
  grt_matmul(&I8, A1, B1, C1, SZ,SZ,SZ);
  while (!__atomic_load_n(&spin_done, __ATOMIC_ACQUIRE)) { }
}
static void spin_sync_only(void){
  __atomic_store_n(&spin_done, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&spin_go, 1, __ATOMIC_RELEASE);
  while (!__atomic_load_n(&spin_done, __ATOMIC_ACQUIRE)) { }
}

static void *worker(void *_){
  pin(1);
  for (;;) {
    pthread_mutex_lock(&mu);
    while (!go_flag && !quit_flag) pthread_cond_wait(&cv_go, &mu);
    if (quit_flag) { pthread_mutex_unlock(&mu); return NULL; }
    go_flag = 0;
    pthread_mutex_unlock(&mu);

    grt_matmul(&I8, A2, B2, C2, SZ,SZ,SZ);

    pthread_mutex_lock(&mu); done_flag = 1;
    pthread_cond_signal(&cv_done); pthread_mutex_unlock(&mu);
  }
}

static void only1(void){ grt_matmul(&I8, A1, B1, C1, SZ,SZ,SZ); }
static void seq(void){ grt_matmul(&I8,A1,B1,C1,SZ,SZ,SZ); grt_matmul(&I8,A2,B2,C2,SZ,SZ,SZ); }
static void par(void){
  pthread_mutex_lock(&mu); go_flag = 1; done_flag = 0;
  pthread_cond_signal(&cv_go); pthread_mutex_unlock(&mu);

  grt_matmul(&I8, A1, B1, C1, SZ,SZ,SZ);      // 이 스레드는 코어0

  pthread_mutex_lock(&mu);
  while (!done_flag) pthread_cond_wait(&cv_done, &mu);
  pthread_mutex_unlock(&mu);
}
// 동기화 자체의 비용 — 병렬 측정에서 빼서 봐야 한다
static void sync_only(void){
  pthread_mutex_lock(&mu); go_flag = 1; done_flag = 0;
  pthread_cond_signal(&cv_go); pthread_mutex_unlock(&mu);
  pthread_mutex_lock(&mu);
  while (!done_flag) pthread_cond_wait(&cv_done, &mu);
  pthread_mutex_unlock(&mu);
}
static int chk(void){
  int b=0; for(int i=0;i<SZ*SZ;i++){ if(C1[i]!=B1[i]) b++; if(C2[i]!=B2[i]) b++; } return b;
}
static double timeit(void (*fn)(void)){
  double best=1e30;
  for(int s=0;s<5;s++){ memset(C1,0,sizeof(C1)); memset(C2,0,sizeof(C2));
    double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt; }
  return best;
}
int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/twocore.log","w");
  if(!g){perror("로그");exit(1);}
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++){
    A1[r*SZ+c]=(r==c)?1:0; B1[r*SZ+c]=(int8_t)((r+c)%7-3);
    A2[r*SZ+c]=(r==c)?1:0; B2[r*SZ+c]=(int8_t)((r+c)%5-2); }
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  pin(0);
  grt_flush_ctx(&I8);
  pthread_t th; 
  if (pthread_create(&th,NULL,worker,NULL)!=0){ mark("스레드 생성 실패"); exit(1); }
  usleep(200000);   // 워커가 코어1에 자리잡을 시간
  mark("=== 코어별 가속기 병렬 (d9: 두 타일 각각 INT8 16x16, %d³, best-of-5) ===", SZ);
  double t1=timeit(only1);
  double ts=timeit(seq); int b1=chk();
  double tp=timeit(par); int b2=chk();
  double tsync=timeit(sync_only);
  mark("단독 한 개        %8.3f ms", t1*1e3);
  mark("한 코어 순차 두 개 %8.3f ms   정확성 %s", ts*1e3, b1?"FAIL":"PASS");
  mark("두 코어 병렬      %8.3f ms   정확성 %s   속도 %.2f배", tp*1e3, b2?"FAIL":"PASS", ts/tp);
  mark("   (완전 병렬이면 단독과 같아야 한다: %.3f ms)", t1*1e3);
  mark("  조건변수 동기화만 %8.3f ms   ← 병렬 시간의 %.0f%%", tsync*1e3, tsync/tp*100.0);
  // 조건변수 워커 종료 후 스핀 워커로 재측정
  pthread_mutex_lock(&mu); quit_flag=1; pthread_cond_signal(&cv_go); pthread_mutex_unlock(&mu);
  pthread_join(th,NULL);

  pthread_t th2;
  if (pthread_create(&th2,NULL,spin_worker,NULL)!=0){ mark("스핀 워커 생성 실패"); exit(1); }
  usleep(200000);
  double tps=timeit(par_spin); int b3=chk();
  double tss=timeit(spin_sync_only);
  mark("");
  mark("두 코어 병렬(스핀) %8.3f ms   정확성 %s   속도 %.2f배", tps*1e3, b3?"FAIL":"PASS", ts/tps);
  mark("  스핀 동기화만   %8.3f ms   ← 병렬 시간의 %.0f%%", tss*1e3, tss/tps*100.0);
  mark("");
  mark("대조: 한 코어 두 가속기 교차 발행은 1.9배 (동기화 없음)");
  __atomic_store_n(&spin_quit, 1, __ATOMIC_RELEASE);
  __atomic_store_n(&spin_go, 1, __ATOMIC_RELEASE);
  pthread_join(th2,NULL);
  mark("=== TWOCORE_DONE ===");
  return 0;
}
