// gemmini_rt.h — Gemmini를 **런타임 파라미터**로 구동하는 최소 라이브러리
//
// 왜 필요한가:
//   Gemmini의 기본 헤더 `gemmini_params.h`는 DIM·elem_t·MAX_BYTES를 **컴파일 타임 상수**로
//   고정한다. 그래서 이종 구성(코어마다 다른 Gemmini)에서는 한 바이너리가 두 가속기를
//   구동할 수 없고, 코어별로 따로 빌드해야 했다(E121·E123).
//
//   그러나 RoCC 명령 자체는 cols·rows·stride를 **런타임 값**으로 받는다.
//   DIM이 컴파일 타임이어야 할 이유가 없다 — 소프트웨어가 주소와 타일링을 계산할 때만 쓰인다.
//   이 헤더는 그 값들을 구조체로 받아, **하나의 바이너리가 여러 Gemmini를 구동**할 수 있게 한다.
//
// 제한: 기본 WS matmul만 지원한다(활성화·정규화·전치 없음). 검증이 목적이다.

#ifndef GEMMINI_RT_H
#define GEMMINI_RT_H

#include <stdint.h>
#include <stddef.h>
#include "rocc-software/src/xcustom.h"

// opcode는 이제 **런타임 선택**이다. big.LITTLE 구성(Rocket64b2gembl)에서는
// 코어0에 가속기가 둘 붙어 있고 opcode로 갈린다: INT8=custom3, FP32=custom2.
//
// 주의: xcustom.h의 ROCC_INSTRUCTION_0_R_R은 `CAT(CUSTOM_, x)`로 **토큰 붙이기**를 하므로
// x가 컴파일 타임 리터럴이어야 한다. 따라서 런타임 선택은 매크로 인자로는 불가능하고,
// 두 변형을 각각 만들어 놓고 분기하는 수밖에 없다.
#define GRT_OP_INT8  3      // custom3 (0x7b)
#define GRT_OP_FP32  2      // custom2 (0x5b)
#define GRT_OP_C1    1      // custom1 (0x2b) — 세 번째 가속기(E167)
#define GRT_ADDR_LEN 32

#define GRT_k_CONFIG            0
#define GRT_k_MVIN              2
#define GRT_k_MVOUT             3
#define GRT_k_COMPUTE_PRELOADED 4
#define GRT_k_PRELOAD           6
#define GRT_k_FLUSH             7

#define GRT_CONFIG_EX 0
#define GRT_CONFIG_LD 1
#define GRT_CONFIG_ST 2

// GARBAGE_ADDR는 0xFFFFFFFF다. `>> 1`을 붙이면 bit31이 0이 되어 하드웨어가
// 이를 "실재하는 거대한 스크래치패드 주소"로 해석하고 컨트롤러가 멎는다(E125에서 실측).
#define GRT_GARBAGE ((uint32_t)(-1))
#define GRT_ACC(addr)     ((uint32_t)(addr) | (1u << 31))            // 누산기, 덮어쓰기
#define GRT_ACC_ACC(addr) ((uint32_t)(addr) | (3u << 30))            // 누산기, 누적

// 가속기 한 대를 기술한다. 코어마다 다른 값을 채워 쓴다.
typedef struct {
  int dim;          // mesh 한 변 (16 또는 8)
  int elem_bytes;   // elem_t 크기 (INT8=1, FP32=4)
  int acc_bytes;    // acc_t 크기 (보통 4)
  int opcode;       // GRT_OP_INT8(3) 또는 GRT_OP_FP32(2)
} grt_ctx;

// gemmini.h가 정의하는 별칭을 여기서 직접 만든다 (기본 헤더에 의존하지 않기 위해).
// opcode마다 변형을 하나씩 만들고, 런타임에 분기한다.
#define GRT_INSN_OP3(rs1, rs2, funct) ROCC_INSTRUCTION_0_R_R(3, (rs1), (rs2), (funct))
#define GRT_INSN_OP2(rs1, rs2, funct) ROCC_INSTRUCTION_0_R_R(2, (rs1), (rs2), (funct))
#define GRT_INSN_OP1(rs1, rs2, funct) ROCC_INSTRUCTION_0_R_R(1, (rs1), (rs2), (funct))

// funct도 리터럴이어야 하므로 funct별로 분기한다. 인자가 많아 보이지만
// 컴파일러가 상수 전파로 전부 접는다.
#define GRT_DISPATCH(op, rs1, rs2, funct)                 \
  do {                                                    \
    /* ROCC_INSTRUCTION_0_R_R은 `{ asm ...; }` 블록으로 확장되므로            */ \
    /* 뒤에 `;`를 붙이면 else가 끊긴다. 각 분기를 중괄호로 감싼다.            */ \
    if      ((op) == GRT_OP_INT8) { GRT_INSN_OP3(rs1, rs2, funct); }          \
    else if ((op) == GRT_OP_FP32) { GRT_INSN_OP2(rs1, rs2, funct); }          \
    else                          { GRT_INSN_OP1(rs1, rs2, funct); }          \
  } while (0)

// 문맥을 받는 함수들이 쓰는 형태
#define GRT_INSN_C(c, rs1, rs2, funct) GRT_DISPATCH((c)->opcode, rs1, rs2, funct)

// skip=0이면 TLB까지 비운다(라이브러리 기본). skip=1이면 TLB 플러시를 건너뛴다.
// 이 구분이 중요하다 — TLB 경로를 원인에서 배제할 수 있는 유일한 손잡이다.
static inline void grt_flush_skip(const grt_ctx *c, int skip) {
  GRT_INSN_C(c, (uint64_t)skip, 0, GRT_k_FLUSH);
}
static inline void grt_flush_ctx(const grt_ctx *c) { grt_flush_skip(c, 0); }
static inline void grt_fence(void) { asm volatile("fence" ::: "memory"); }

// 스케일 항등값: 두 구성(INT8/FP32) 모두 scale_t·acc_scale_t가 float이므로
// 1.0f의 비트패턴 0x3f800000이 공통이다 — 구성에 의존하지 않는다.
#define GRT_SCALE_ONE 0x3f800000u

// rs1 = (scale<<32) | (block_mvin_stride<<16) | (pixel_repeats<<8) | (id<<3) | (shrunk<<2) | CONFIG_LD
// block_mvin_stride는 DIM이다 — 여기가 바로 런타임 파라미터가 필요한 지점.
static inline void grt_config_ld(const grt_ctx *c, uint64_t stride_bytes) {
  GRT_INSN_C(c, ((uint64_t)GRT_SCALE_ONE << 32) | ((uint64_t)c->dim << 16) | (1ull << 8)
             | (0ull << 3) | (0ull << 2) | GRT_CONFIG_LD,
           stride_bytes, GRT_k_CONFIG);
}

// rs1 = (pool/pad 전부 0) | (acc_act<<2) | CONFIG_ST
// rs2 = (acc_scale<<32) | (uint32_t)stride
static inline void grt_config_st(const grt_ctx *c, uint64_t stride_bytes) {
  GRT_INSN_C(c, (0ull << 2) | GRT_CONFIG_ST,
           ((uint64_t)GRT_SCALE_ONE << 32) | (uint32_t)stride_bytes, GRT_k_CONFIG);
}

// rs1 = (acc_scale<<32) | (A_stride<<16) | (B_tr<<9) | (A_tr<<8) | (set_only<<7)
//       | (sys_act<<3) | (dataflow<<2) | CONFIG_EX
// rs2 = (C_stride<<48) | sys_shift
#define GRT_WS 1
#define GRT_OS 0
static inline void grt_config_ex(const grt_ctx *c, int dataflow) {
  GRT_INSN_C(c, ((uint64_t)GRT_SCALE_ONE << 32) | (1ull << 16) | ((uint64_t)dataflow << 2)
             | GRT_CONFIG_EX,
           (1ull << 48) | 0ull, GRT_k_CONFIG);
}

// config_ld의 id 필드를 지정하는 판. loop_ws는 A/B/D를 각각 id 0/1/2로 설정한다
// (gemmini.h의 tiled_matmul_outer가 그렇게 한다).
static inline void grt_config_ld_id(const grt_ctx *c, uint64_t stride_bytes, int id) {
  GRT_INSN_C(c, ((uint64_t)GRT_SCALE_ONE << 32) | ((uint64_t)c->dim << 16) | (1ull << 8)
             | ((uint64_t)id << 3) | (0ull << 2) | GRT_CONFIG_LD,
           stride_bytes, GRT_k_CONFIG);
}

// ---------------------------------------------------------------------------
// 루프 FSM (E176) — 명령 6개로 I×J×K 타일 루프 전체를 돌린다.
//
// E173이 잰 병목은 "타일 연산당 명령 3개를 코어가 발행해야 한다"는 것이었다.
// loop_ws는 그 전제를 없앤다: 128³이면 512 타일 연산이 명령 6개다. 발행 비용이
// 타일당에서 matmul당으로 바뀌므로, 단일 코어가 가속기 여러 개를 먹일 수 있는지가
// 다시 열린다.
//
// 인코딩은 gemmini.h의 gemmini_loop_ws 매크로와 tiled_matmul_outer의 config 순서를
// 그대로 옮겼다 (기억에 의존하지 말 것 — 여기서 네 번 데였다).
#define GRT_k_LOOP_WS                 8
#define GRT_k_LOOP_WS_CFG_BOUNDS      9
#define GRT_k_LOOP_WS_CFG_ADDRS_AB   10
#define GRT_k_LOOP_WS_CFG_ADDRS_DC   11
#define GRT_k_LOOP_WS_CFG_STRIDES_AB 12
#define GRT_k_LOOP_WS_CFG_STRIDES_DC 13

// 스케일·데이터플로우 설정. loop_ws 앞에 한 번만 하면 된다.
// stride 인자는 **바이트**(config_ld/st가 바이트를 받는다).
static inline void grt_loop_ws_config(const grt_ctx *c,
                                      uint64_t A_stride_b, uint64_t B_stride_b,
                                      uint64_t C_stride_b) {
  grt_config_ex(c, GRT_WS);
  grt_config_st(c, C_stride_b);
  grt_config_ld_id(c, A_stride_b, 0);
  grt_config_ld_id(c, B_stride_b, 1);
  grt_config_ld_id(c, 0, 2);              // D 없음
}

// I·J·K는 **타일 개수**. stride는 **원소 단위 행 스트라이드**(loop_ws가 그렇게 받는다).
// 편향 없음(D=NULL), full_C=0, act=0, 전치 없음.
// 용량 제약: I*J <= ACC_ROWS/dim,  I*K + K*J <= (BANK_NUM*BANK_ROWS)/dim.
// spad_id 판: 라이브러리는 피연산자를 재사용할 때 1(적재)/2(재사용)를 넘긴다.
// 0은 기본(매번 적재). E178에서 다중 블록 경합의 후보로 지목됐다.
static inline void grt_loop_ws_id(const grt_ctx *c, int I, int J, int K,
                                  const void *A, const void *B, void *C,
                                  uint64_t A_stride, uint64_t B_stride, uint64_t C_stride,
                                  int a_spad_id, int b_spad_id) {
  GRT_INSN_C(c, 0,
           ((uint64_t)K << 32) | ((uint64_t)J << 16) | (uint64_t)I,
           GRT_k_LOOP_WS_CFG_BOUNDS);
  GRT_INSN_C(c, (uint64_t)A, (uint64_t)B, GRT_k_LOOP_WS_CFG_ADDRS_AB);
  GRT_INSN_C(c, 0, (uint64_t)C, GRT_k_LOOP_WS_CFG_ADDRS_DC);
  GRT_INSN_C(c, A_stride, B_stride, GRT_k_LOOP_WS_CFG_STRIDES_AB);
  GRT_INSN_C(c, 0, C_stride, GRT_k_LOOP_WS_CFG_STRIDES_DC);
  GRT_INSN_C(c, ((uint64_t)a_spad_id << 18) | ((uint64_t)b_spad_id << 16) | 0ull,
           0ull, GRT_k_LOOP_WS);
}

static inline void grt_loop_ws(const grt_ctx *c, int I, int J, int K,
                               const void *A, const void *B, void *C,
                               uint64_t A_stride, uint64_t B_stride, uint64_t C_stride) {
  GRT_INSN_C(c, 0,                                          /* pad_I/J/K = 0 */
           ((uint64_t)K << 32) | ((uint64_t)J << 16) | (uint64_t)I,
           GRT_k_LOOP_WS_CFG_BOUNDS);
  GRT_INSN_C(c, (uint64_t)A, (uint64_t)B, GRT_k_LOOP_WS_CFG_ADDRS_AB);
  GRT_INSN_C(c, 0, (uint64_t)C, GRT_k_LOOP_WS_CFG_ADDRS_DC);   /* D = NULL */
  GRT_INSN_C(c, A_stride, B_stride, GRT_k_LOOP_WS_CFG_STRIDES_AB);
  GRT_INSN_C(c, 0, C_stride, GRT_k_LOOP_WS_CFG_STRIDES_DC);
  /* rs1 = (a_spad_id<<18)|(b_spad_id<<16)|(act<<8)|(low_D<<2)|(full_C<<1)|ex_accumulate
     ex_accumulate는 **0**이다. 라이브러리 식은 `!no_bias || D == NULL`이지만,
     tiled_matmul_outer가 편향이 없을 때 `D = (void*)1`로 더미 주소를 넣으므로
     `D == NULL`이 거짓이 되고 전체가 0이 된다. 1을 넣으면 누산기를 지우지 않아
     결과가 전부 틀린다(E176에서 실제로 그랬다). */
  GRT_INSN_C(c, 0ull, 0ull, GRT_k_LOOP_WS);
}

// ---------------------------------------------------------------------------
// K 분할판 (E199). 라이브러리의 tiled_matmul_outer가 하는 것을 그대로 옮긴다:
//   중간 K 청크는 C=NULL로 넘겨 결과를 누산기에 남기고 ex_accumulate=1로 누적,
//   마지막 청크에서만 진짜 C를 주어 mvout 한다.
//
// 왜 필요한가: K가 크면 스크래치패드가 I·J를 짓눌러 (1,1)까지 내려간다.
// BERT FFN2(K=192타일)에서 (1,1)은 강도 32.1 B/연산사이클로, 스톡보다 5.26배 느렸다(E198).
// K를 16타일씩 끊으면 (8,4)를 쓸 수 있고 강도가 6.08로 5.3배 준다.
// 이동 바이트 총량은 K 분할과 무관하다 — 바뀌는 것은 쓸 수 있는 블록 크기다.
static inline void grt_loop_ws_k(const grt_ctx *c, int I, int J, int K,
                                 const void *A, const void *B, void *C,
                                 uint64_t A_stride, uint64_t B_stride, uint64_t C_stride,
                                 int ex_accum) {
  GRT_INSN_C(c, 0, ((uint64_t)K << 32) | ((uint64_t)J << 16) | (uint64_t)I,
           GRT_k_LOOP_WS_CFG_BOUNDS);
  GRT_INSN_C(c, (uint64_t)A, (uint64_t)B, GRT_k_LOOP_WS_CFG_ADDRS_AB);
  GRT_INSN_C(c, 0, (uint64_t)C, GRT_k_LOOP_WS_CFG_ADDRS_DC);   /* C=0이면 mvout 없음 */
  GRT_INSN_C(c, A_stride, B_stride, GRT_k_LOOP_WS_CFG_STRIDES_AB);
  GRT_INSN_C(c, 0, C_stride, GRT_k_LOOP_WS_CFG_STRIDES_DC);
  GRT_INSN_C(c, (uint64_t)(ex_accum ? 1 : 0), 0ull, GRT_k_LOOP_WS);
}

// 출력 블록 (i0,j0) 하나를 K를 kc 타일씩 끊어 계산한다. TK는 K의 총 타일 수.
static inline void grt_block_ksplit(const grt_ctx *c, int I, int J, int TK, int kc,
                                    const void *A, const void *B, void *C,
                                    uint64_t A_stride, uint64_t B_stride, uint64_t C_stride,
                                    int elem_bytes) {
  for (int k0 = 0; k0 < TK; k0 += kc) {
    int Kc = (k0 + kc <= TK) ? kc : (TK - k0);
    int last = (k0 + Kc >= TK);
    grt_loop_ws_k(c, I, J, Kc,
                  (const char *)A + (size_t)k0 * 16 * elem_bytes,
                  (const char *)B + (size_t)k0 * 16 * B_stride * elem_bytes,
                  last ? C : (void *)0,
                  A_stride, B_stride, C_stride, k0 != 0);
  }
}

static inline void grt_mvin(const grt_ctx *c, const void *dram, uint32_t sp,
                            int cols, int rows) {
  GRT_INSN_C(c, (uint64_t)dram,
           ((uint64_t)rows << (GRT_ADDR_LEN + 16)) | ((uint64_t)cols << GRT_ADDR_LEN) | sp,
           GRT_k_MVIN);
}
static inline void grt_mvout(const grt_ctx *c, void *dram, uint32_t sp,
                             int cols, int rows) {
  GRT_INSN_C(c, (uint64_t)dram,
           ((uint64_t)rows << (GRT_ADDR_LEN + 16)) | ((uint64_t)cols << GRT_ADDR_LEN) | sp,
           GRT_k_MVOUT);
}
static inline void grt_preload(const grt_ctx *c, uint32_t bd_sp, uint32_t c_sp,
                               int bd_cols, int bd_rows, int c_cols, int c_rows) {
  GRT_INSN_C(c, ((uint64_t)bd_rows << (GRT_ADDR_LEN + 16)) | ((uint64_t)bd_cols << GRT_ADDR_LEN) | bd_sp,
           ((uint64_t)c_rows  << (GRT_ADDR_LEN + 16)) | ((uint64_t)c_cols  << GRT_ADDR_LEN) | c_sp,
           GRT_k_PRELOAD);
}
static inline void grt_compute(const grt_ctx *c, uint32_t a_sp, uint32_t bd_sp,
                               int a_cols, int a_rows, int bd_cols, int bd_rows) {
  GRT_INSN_C(c, ((uint64_t)a_rows  << (GRT_ADDR_LEN + 16)) | ((uint64_t)a_cols  << GRT_ADDR_LEN) | a_sp,
           ((uint64_t)bd_rows << (GRT_ADDR_LEN + 16)) | ((uint64_t)bd_cols << GRT_ADDR_LEN) | bd_sp,
           GRT_k_COMPUTE_PRELOADED);
}


// ---------------------------------------------------------------------------
// 런타임 타일 matmul: C[M][N] = A[M][K] * B[K][N]  (WS, 바이어스 없음)
//
// M·N·K는 dim의 배수여야 한다(패딩 없음 — 검증·측정이 목적이므로).
// 스크래치패드 배치: A 타일 1개, B 타일 1개를 매번 새로 올린다.
// 누산은 누산기에서 하고(K 방향), 타일이 끝나면 mvout 한다.
// ---------------------------------------------------------------------------
// fence를 걸지 않는 판. 두 가속기에 명령을 번갈아 쏟아넣고 **한 번만** fence 하려면
// 중간 fence가 없어야 한다 — 동시 가동 여부를 재는 실험(E156)에 필요하다.
static void grt_matmul_nf(const grt_ctx *c, const void *A, const void *B, void *C,
                          int M, int N, int K);

static void grt_matmul(const grt_ctx *c, const void *A, const void *B, void *C,
                       int M, int N, int K) {
  const int d = c->dim;
  const int eb = c->elem_bytes;
  const uint32_t A_sp = 0, B_sp = 64;      // 타일 하나씩만 쓰므로 겹치지 않게만 두면 된다
  const uint32_t C_acc_base = 0;

  grt_config_ex(c, GRT_WS);

  for (int i = 0; i < M; i += d) {
    for (int j = 0; j < N; j += d) {
      for (int k = 0; k < K; k += d) {
        // A 타일 (i,k): 원본 행길이가 K이므로 stride = K * eb
        grt_config_ld(c, (uint64_t)K * eb);
        grt_mvin(c, (const char *)A + ((size_t)i * K + k) * eb, A_sp, d, d);
        // B 타일 (k,j): stride = N * eb
        grt_config_ld(c, (uint64_t)N * eb);
        grt_mvin(c, (const char *)B + ((size_t)k * N + j) * eb, B_sp, d, d);

        // 첫 k에서는 덮어쓰기, 이후에는 누적
        uint32_t cdst = (k == 0) ? GRT_ACC(C_acc_base) : GRT_ACC_ACC(C_acc_base);
        grt_preload(c, B_sp, cdst, d, d, d, d);
        grt_compute(c, A_sp, GRT_GARBAGE, d, d, d, d);
      }
      grt_config_st(c, (uint64_t)N * eb);
      grt_mvout(c, (char *)C + ((size_t)i * N + j) * eb, GRT_ACC(C_acc_base), d, d);
    }
  }
  grt_fence();
}

static void grt_matmul_nf(const grt_ctx *c, const void *A, const void *B, void *C,
                          int M, int N, int K) {
  const int d = c->dim, eb = c->elem_bytes;
  const uint32_t A_sp = 0, B_sp = 64, C_acc_base = 0;
  grt_config_ex(c, GRT_WS);
  for (int i = 0; i < M; i += d) {
    for (int j = 0; j < N; j += d) {
      for (int k = 0; k < K; k += d) {
        grt_config_ld(c, (uint64_t)K * eb);
        grt_mvin(c, (const char *)A + ((size_t)i * K + k) * eb, A_sp, d, d);
        grt_config_ld(c, (uint64_t)N * eb);
        grt_mvin(c, (const char *)B + ((size_t)k * N + j) * eb, B_sp, d, d);
        uint32_t cdst = (k == 0) ? GRT_ACC(C_acc_base) : GRT_ACC_ACC(C_acc_base);
        grt_preload(c, B_sp, cdst, d, d, d, d);
        grt_compute(c, A_sp, GRT_GARBAGE, d, d, d, d);
      }
      grt_config_st(c, (uint64_t)N * eb);
      grt_mvout(c, (char *)C + ((size_t)i * N + j) * eb, GRT_ACC(C_acc_base), d, d);
    }
  }
  /* fence 없음 — 호출자가 건다 */
}


// ---------------------------------------------------------------------------
// grt_mm2 — **형상이 다른 두 워크로드**를 타일 단위로 교차 발행한다 (E163).
//
// E161에서 확인: 이득은 **타일 연산 수**로 균형을 맞춰야 나온다.
//   같은 64³을 양쪽에 주면 INT8(dim16) 64타일 : FP32(dim8) 512타일로 8배 불균형이라
//   절감이 11%에 그친다. 타일 수를 맞추면(INT8 128³ : FP32 64³) 1.80배가 나온다.
//   따라서 primitive가 **형상이 다른 두 워크로드**를 받아야 실용적이다.
//
// 두 워크로드는 서로 독립이어야 한다(한쪽 출력이 다른 쪽 입력이면 안 된다).
// 각 워크로드의 타일 수 = (M/dim)*(N/dim)*(K/dim).

typedef struct {                    // 워크로드 하나
  const grt_ctx *c;
  const void *A, *B;
  void *C;
  int M, N, K;
} grt_work;

typedef struct {                    // 순회 상태
  int i, j, k, done;
  int par;                          // 재사용 스테퍼의 B 이중 버퍼 패리티
} grt_cursor;

// 타일 하나를 발행하고 커서를 전진시킨다.
//
// **opcode별로 특수화한다.** 문맥을 포인터로 받으면 `GRT_DISPATCH`의
// `if (op == ...)` 분기를 컴파일러가 접을 수 없어 **RoCC 명령마다 런타임 분기**가 붙는다.
// 그 CPU 작업이 명령 발행 경로 위에 있어 중첩을 크게 깎는다 —
// 실측으로 1.80배 → 1.24배로 떨어졌다(E164). 매크로로 두 판을 찍어낸다.
// **완전 특수화**: opcode뿐 아니라 dim·elem_bytes까지 상수로 박는다.
//
// 측정으로 확인된 비용(E164):
//   손으로 짠 루프(전부 상수)        1.79배
//   opcode만 특수화 (dim은 런타임)   1.38배
//   문맥을 포인터로만 전달           1.24배
// 즉 **발행 루프에서는 추상화가 공짜가 아니다.** 컴파일러가 명령 선택과 주소 계산을
// 접을 수 있어야 하고, 그러려면 C에서는 조합마다 코드를 찍어내는 수밖에 없다.
#define GRT_DEFINE_STEP(NAME, OP, DIM, EB)                                             \
static void NAME(const grt_work *w, grt_cursor *s) {                                   \
  if (s->done) return;                                                                 \
  static const grt_ctx k = { .dim=(DIM), .elem_bytes=(EB), .acc_bytes=4, .opcode=(OP) };\
  grt_config_ld(&k, (uint64_t)w->K * (EB));                                            \
  grt_mvin(&k, (const char *)w->A + ((size_t)s->i * w->K + s->k) * (EB), 0, (DIM), (DIM)); \
  grt_config_ld(&k, (uint64_t)w->N * (EB));                                            \
  grt_mvin(&k, (const char *)w->B + ((size_t)s->k * w->N + s->j) * (EB), 64, (DIM), (DIM)); \
  grt_preload(&k, 64, (s->k == 0) ? GRT_ACC(0) : GRT_ACC_ACC(0), (DIM),(DIM),(DIM),(DIM)); \
  grt_compute(&k, 0, GRT_GARBAGE, (DIM),(DIM),(DIM),(DIM));                            \
  s->k += (DIM);                                                                       \
  if (s->k >= w->K) {                                                                  \
    s->k = 0;                                                                          \
    grt_config_st(&k, (uint64_t)w->N * (EB));                                           \
    grt_mvout(&k, (char *)w->C + ((size_t)s->i * w->N + s->j) * (EB), GRT_ACC(0), (DIM), (DIM)); \
    s->j += (DIM);                                                                     \
    if (s->j >= w->N) { s->j = 0; s->i += (DIM); if (s->i >= w->M) s->done = 1; }      \
  }                                                                                    \
}
GRT_DEFINE_STEP(grt_step_i8, GRT_OP_INT8, 16, 1)   // custom3 + INT8 16x16
GRT_DEFINE_STEP(grt_step_fp, GRT_OP_FP32,  8, 4)   // custom2 + FP32 8x8
// 같은 종류 둘 구성(E166)에서는 custom2도 INT8 16x16이다.
// 조합마다 코드를 찍어내야 하는 것이 이 접근의 실제 비용이다(E164).
GRT_DEFINE_STEP(grt_step_i8b, GRT_OP_FP32, 16, 1)  // custom2 + INT8 16x16
GRT_DEFINE_STEP(grt_step_i8c, GRT_OP_C1,   16, 1)  // custom1 + INT8 16x16 (E167)


// ---------------------------------------------------------------------------
// 재사용 스테퍼 (E172) — 위 스테퍼의 활용률이 12%뿐이라서 만들었다.
//
// 위 스테퍼는 타일 연산마다 A와 B를 **둘 다** 새로 싣는다. 16x16 INT8 타일 두 개의
// mvin은 128비트 버스에서 약 32 cycle인데 메쉬 연산은 16 cycle이라, DMA가 컴퓨트의
// 두 배다. 게다가 B를 매번 같은 스크래치패드 주소(64)에 쓰므로 WAR 의존이 걸려
// 다음 mvin이 현재 compute와 겹치지 못한다.
//
// 두 가지를 바꾼다:
//   1. 루프 순서를 i,k,j 로 (기존 i,j,k). A(i,k)는 j 전체에 재사용되므로 k당 한 번만
//      싣는다. A의 mvin이 (N/dim)분의 1로 준다. 대신 누산기가 N/dim 타일을 동시에
//      들고 있어야 하고, i 블록이 끝날 때 한꺼번에 mvout 한다.
//   2. B를 두 주소에 번갈아 실어(이중 버퍼) WAR을 끊는다. 다음 타일의 mvin이 현재
//      compute와 겹칠 수 있다.
//
// 타일 연산당 명령이 6개(config_ld·mvin A·config_ld·mvin B·preload·compute)에서
// 3개(mvin B·preload·compute)로 준다.
//
// 제약: 누산기 행이 N개 필요하다 (타일당 dim행 x N/dim개). ACC_ROWS=1024이므로
// N <= 1024 에서만 쓸 수 있다. 넘으면 조용히 틀린 답이 나온다.
#define GRT_SP_A   0
#define GRT_SP_B0  64
#define GRT_SP_B1  128

#define GRT_DEFINE_STEP_R(NAME, OP, DIM, EB)                                           \
static void NAME(const grt_work *w, grt_cursor *s) {                                   \
  if (s->done) return;                                                                 \
  static const grt_ctx k = { .dim=(DIM), .elem_bytes=(EB), .acc_bytes=4, .opcode=(OP) };\
  if (s->j == 0) {                          /* A(i,k)는 k 하나당 한 번만 */            \
    grt_config_ld(&k, (uint64_t)w->K * (EB));                                          \
    grt_mvin(&k, (const char *)w->A + ((size_t)s->i * w->K + s->k) * (EB),             \
             GRT_SP_A, (DIM), (DIM));                                                  \
    grt_config_ld(&k, (uint64_t)w->N * (EB));                                          \
  }                                                                                    \
  uint32_t bsp = s->par ? GRT_SP_B1 : GRT_SP_B0;                                       \
  grt_mvin(&k, (const char *)w->B + ((size_t)s->k * w->N + s->j) * (EB),               \
           bsp, (DIM), (DIM));                                                         \
  grt_preload(&k, bsp, (s->k == 0) ? GRT_ACC(s->j) : GRT_ACC_ACC(s->j),                \
              (DIM),(DIM),(DIM),(DIM));                                                \
  grt_compute(&k, GRT_SP_A, GRT_GARBAGE, (DIM),(DIM),(DIM),(DIM));                     \
  s->par ^= 1;                                                                         \
  s->j += (DIM);                                                                       \
  if (s->j >= w->N) {                                                                  \
    s->j = 0; s->k += (DIM);                                                           \
    if (s->k >= w->K) {                     /* 이 i 블록의 누산 완료 → 한꺼번에 내보냄 */ \
      s->k = 0;                                                                        \
      grt_config_st(&k, (uint64_t)w->N * (EB));                                        \
      for (int jj = 0; jj < w->N; jj += (DIM))                                         \
        grt_mvout(&k, (char *)w->C + ((size_t)s->i * w->N + jj) * (EB),                \
                  GRT_ACC(jj), (DIM), (DIM));                                          \
      s->i += (DIM); if (s->i >= w->M) s->done = 1;                                    \
    }                                                                                  \
  }                                                                                    \
}
// E222: 발행 비용의 **명령 수 의존성**을 재기 위한 판. preload를 REP번 반복한다.
// preload는 스크래치패드에서만 읽으므로 **DMA 바이트가 늘지 않는다** — 발행 항만 늘어난다.
// 같은 주소를 반복 preload하므로 결과도 그대로다.
#define GRT_DEFINE_STEP_RP(NAME, OP, DIM, EB, REP)                                     \
static void NAME(const grt_work *w, grt_cursor *s) {                                   \
  if (s->done) return;                                                                 \
  static const grt_ctx k = { .dim=(DIM), .elem_bytes=(EB), .acc_bytes=4, .opcode=(OP) };\
  if (s->j == 0) {                                                                     \
    grt_config_ld(&k, (uint64_t)w->K * (EB));                                           \
    grt_mvin(&k, (const char *)w->A + ((size_t)s->i * w->K + s->k) * (EB),              \
             GRT_SP_A, (DIM), (DIM));                                                   \
    grt_config_ld(&k, (uint64_t)w->N * (EB));                                           \
  }                                                                                     \
  uint32_t bsp = s->par ? GRT_SP_B1 : GRT_SP_B0;                                        \
  grt_mvin(&k, (const char *)w->B + ((size_t)s->k * w->N + s->j) * (EB),                \
           bsp, (DIM), (DIM));                                                           \
  for (int _r = 0; _r < (REP); _r++)                                                     \
    grt_preload(&k, bsp, (s->k == 0) ? GRT_ACC(s->j) : GRT_ACC_ACC(s->j),                \
                (DIM),(DIM),(DIM),(DIM));                                                \
  grt_compute(&k, GRT_SP_A, GRT_GARBAGE, (DIM),(DIM),(DIM),(DIM));                       \
  s->par ^= 1;                                                                           \
  s->j += (DIM);                                                                          \
  if (s->j >= w->N) {                                                                     \
    s->j = 0; s->k += (DIM);                                                              \
    if (s->k >= w->K) {                                                                   \
      s->k = 0;                                                                            \
      grt_config_st(&k, (uint64_t)w->N * (EB));                                            \
      for (int jj = 0; jj < w->N; jj += (DIM))                                             \
        grt_mvout(&k, (char *)w->C + ((size_t)s->i * w->N + jj) * (EB),                    \
                  GRT_ACC(jj), (DIM), (DIM));                                              \
      s->i += (DIM); if (s->i >= w->M) s->done = 1;                                        \
    }                                                                                       \
  }                                                                                         \
}
GRT_DEFINE_STEP_RP(grt_s_r1a, GRT_OP_INT8, 16, 1, 1)
GRT_DEFINE_STEP_RP(grt_s_r1b, GRT_OP_FP32, 16, 1, 1)
GRT_DEFINE_STEP_RP(grt_s_r1c, GRT_OP_C1,   16, 1, 1)
GRT_DEFINE_STEP_RP(grt_s_r3a, GRT_OP_INT8, 16, 1, 3)
GRT_DEFINE_STEP_RP(grt_s_r3b, GRT_OP_FP32, 16, 1, 3)
GRT_DEFINE_STEP_RP(grt_s_r3c, GRT_OP_C1,   16, 1, 3)
GRT_DEFINE_STEP_RP(grt_s_r6a, GRT_OP_INT8, 16, 1, 6)
GRT_DEFINE_STEP_RP(grt_s_r6b, GRT_OP_FP32, 16, 1, 6)
GRT_DEFINE_STEP_RP(grt_s_r6c, GRT_OP_C1,   16, 1, 6)

GRT_DEFINE_STEP_R(grt_step_r_i8,  GRT_OP_INT8, 16, 1)  // custom3 + INT8 16x16
GRT_DEFINE_STEP_R(grt_step_r_i8b, GRT_OP_FP32, 16, 1)  // custom2 + INT8 16x16
GRT_DEFINE_STEP_R(grt_step_r_i8c, GRT_OP_C1,   16, 1)  // custom1 + INT8 16x16 (E206)

// 재사용 스테퍼로 단일 matmul
static void grt_matmul_r_i8(const grt_work *w) {
  grt_cursor s = {0,0,0,0,0};
  grt_config_ex(w->c, GRT_WS);
  while (!s.done) grt_step_r_i8(w, &s);
  grt_fence();
}

// 재사용 스테퍼로 두 워크로드를 타일 단위 교대 발행 (둘 다 INT8 16x16)
// 재사용 스테퍼로 세 워크로드 교대 발행 (E206, c_issue 측정용)
static void grt_mm3_r_i8(const grt_work *w1, const grt_work *w2, const grt_work *w3) {
  grt_cursor s1={0,0,0,0,0}, s2={0,0,0,0,0}, s3={0,0,0,0,0};
  grt_config_ex(w1->c, GRT_WS); grt_config_ex(w2->c, GRT_WS); grt_config_ex(w3->c, GRT_WS);
  while (!s1.done || !s2.done || !s3.done) {
    grt_step_r_i8 (w1,&s1); grt_step_r_i8b(w2,&s2); grt_step_r_i8c(w3,&s3); }
  grt_fence();
}

static void grt_mm2_r_i8i8(const grt_work *w1, const grt_work *w2) {
  grt_cursor s1 = {0,0,0,0,0}, s2 = {0,0,0,0,0};
  grt_config_ex(w1->c, GRT_WS);
  grt_config_ex(w2->c, GRT_WS);
  while (!s1.done || !s2.done) { grt_step_r_i8(w1,&s1); grt_step_r_i8b(w2,&s2); }
  grt_fence();
}

// 두 워크로드를 타일 단위로 번갈아 발행하고 마지막에 한 번만 fence 한다.
static void grt_mm2(const grt_work *w1, const grt_work *w2) {
  grt_cursor s1 = {0,0,0,0}, s2 = {0,0,0,0};
  grt_config_ex(w1->c, GRT_WS);
  grt_config_ex(w2->c, GRT_WS);
  const int o1 = w1->c->opcode, o2 = w2->c->opcode;
  if (o1 == GRT_OP_INT8 && o2 == GRT_OP_FP32) {
    while (!s1.done || !s2.done) { grt_step_i8(w1,&s1); grt_step_fp(w2,&s2); }
  } else if (o1 == GRT_OP_FP32 && o2 == GRT_OP_INT8) {
    while (!s1.done || !s2.done) { grt_step_fp(w1,&s1); grt_step_i8(w2,&s2); }
  } else if (o1 == GRT_OP_INT8 && o2 == GRT_OP_INT8) {
    while (!s1.done || !s2.done) { grt_step_i8(w1,&s1); grt_step_i8(w2,&s2); }
  } else {
    while (!s1.done || !s2.done) { grt_step_fp(w1,&s1); grt_step_fp(w2,&s2); }
  }
  grt_fence();
}

// 같은 종류(둘 다 INT8 16x16, opcode만 다름) 전용 — E166 대조 실험.
// grt_mm2의 opcode 판별로는 w2가 FP32 형상이라고 가정하므로 별도 진입점이 필요하다.
static void grt_mm2_i8i8(const grt_work *w1, const grt_work *w2) {
  grt_cursor s1 = {0,0,0,0}, s2 = {0,0,0,0};
  grt_config_ex(w1->c, GRT_WS);
  grt_config_ex(w2->c, GRT_WS);
  while (!s1.done || !s2.done) { grt_step_i8(w1,&s1); grt_step_i8b(w2,&s2); }
  grt_fence();
}


// 세 워크로드를 타일 단위로 순환 발행한다 (E167). 셋 다 INT8 16x16, opcode만 다르다.
static void grt_mm3_i8(const grt_work *w1, const grt_work *w2, const grt_work *w3) {
  grt_cursor s1={0,0,0,0}, s2={0,0,0,0}, s3={0,0,0,0};
  grt_config_ex(w1->c, GRT_WS); grt_config_ex(w2->c, GRT_WS); grt_config_ex(w3->c, GRT_WS);
  while (!s1.done || !s2.done || !s3.done) {
    grt_step_i8(w1,&s1); grt_step_i8b(w2,&s2); grt_step_i8c(w3,&s3);
  }
  grt_fence();
}

#endif
