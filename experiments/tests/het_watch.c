// het_watch.c — "하트 하나만 멈춘 것"인지 "시스템 전체가 막힌 것"인지 가른다 (E128).
//
// 왜 필요한가:
//   RocketCore를 읽어보니 RoCC 명령은 !io.rocc.cmd.ready면 WB에서 영원히 replay된다
//   (RocketCore.scala:767). 그런데 replay 중에도 인터럽트는 받는다(take_pc_wb에
//   wb_xcpt가 포함). 즉 하트 하나가 멈춰도 **리눅스는 살아 있어야 하고 sshd도 떠야 한다.**
//   실제로는 sshd 배너 교환도 실패했다. 모델과 관측이 어긋난다.
//
//   가능성 둘:
//     (A) cpu1 하트만 멈췄고, 시스템은 멀쩡한데 내가 관측을 잘못했다
//     (B) 명령 발행 자체는 됐고, 그 뒤 가속기가 TileLink에 이상 트랜잭션을 내
//         **시스템 버스가 막혔다** (그러면 NFS·네트워크·sshd가 전부 죽는다)
//
// 감시자 스레드를 cpu0에 고정해 0.5초마다 마크를 남긴다. cpu1 스레드가 RoCC 명령을
// 낸 뒤에도 감시자 마크가 계속 찍히면 (A), 함께 멎으면 (B)다.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <time.h>
#include "include/gemmini_rt.h"

static FILE *g;      // 주 스레드용
static FILE *gw;     // 감시자 전용 (파일을 나눠 인터리브·버퍼 문제를 배제)
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static void wr(FILE *f, const char *s, long n){
  if (!f) return;
  if (n >= 0) fprintf(f, "%s %ld  (cpu=%d)\n", s, n, sched_getcpu());
  else        fprintf(f, "%s  (cpu=%d)\n", s, sched_getcpu());
  fflush(f); fsync(fileno(f));
}
static void mark(const char *s, long n){ wr(g, s, n); }
static void pin(int c){ cpu_set_t s; CPU_ZERO(&s); CPU_SET(c,&s);
  pthread_setaffinity_np(pthread_self(), sizeof(s), &s); sched_yield(); }

static const grt_ctx C1 = { .dim=8, .elem_bytes=4, .acc_bytes=4 };

static void *watchdog(void *_){
  pin(0);
  for (long i = 0; i < 60; i++) {           // 30초간 감시
    wr(gw, "감시자 tick", i);
    struct timespec t = {0, 500*1000*1000}; nanosleep(&t, NULL);
  }
  wr(gw, "감시자 정상 종료", -1);
  return NULL;
}

int main(int argc, char **argv){
  g = fopen(argc>1?argv[1]:"/mnt2/tmp/het_watch.log","w");
  if(!g){ perror("로그"); exit(1); }
  gw = fopen("/mnt2/tmp/het_watch_wd.log","w");
  if(!gw){ perror("감시자 로그"); exit(1); }
  mark("시작 — 감시자를 cpu0에 띄운다", -1);

  // 스레드를 mlockall **이전에** 만든다. mlockall(MCL_FUTURE)를 먼저 걸면
  // 새 스레드 스택 매핑까지 잠그려 해서 RLIMIT_MEMLOCK에 걸려 생성이 실패한다
  // (앞선 실행에서 감시자 tick이 하나도 안 찍힌 원인).
  pthread_t th;
  int rc = pthread_create(&th, NULL, watchdog, NULL);
  mark(rc==0 ? "감시자 스레드 생성 성공" : "감시자 스레드 생성 실패", rc);
  if (rc != 0) { fprintf(g, "  → 감시 불가, 중단\n"); fflush(g); exit(2); }

  sleep(3);                                  // 감시자가 몇 틱 찍게 둔다
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(무시하고 진행)", -1);

  pin(1);
  mark(">>> cpu1에서 config_ex 발행 직전 <<<", -1);
  grt_config_ex(&C1, GRT_WS);
  mark(">>> cpu1 config_ex 통과 <<<", -1);

  pthread_join(th, NULL);
  mark("=== 전부 종료 ===", -1);
  return 0;
}
