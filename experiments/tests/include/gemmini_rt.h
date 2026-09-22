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
/* E720: `block_mvin_stride`(rs1[31:16])를 직접 준다. 기본값은 DIM 이라 폭 넓은 mvin 의
 * 타일들이 연속 행에 놓이는데, **뱅크 크기**를 주면 타일 t 가 뱅크 (base+t) 로 간다 —
 * 접힌 배치의 세그먼트 S 개를 **DRAM 연속 읽기 한 번**으로 채울 수 있다. */
static inline void grt_config_ld_bs(const grt_ctx *c, uint64_t stride_bytes, int block_stride) {
  GRT_INSN_C(c, ((uint64_t)GRT_SCALE_ONE << 32) | ((uint64_t)(uint16_t)block_stride << 16)
             | (1ull << 8) | (0ull << 3) | (0ull << 2) | GRT_CONFIG_LD,
           stride_bytes, GRT_k_CONFIG);
}
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

// ---------------- SSGemm (E547~E548) ----------------
// shape 를 config_ex rs1[12:10] 에 싣는다 (ShapeshiftIsa — spacer 안, 기존 필드와
// 충돌 없음). shape=0 이 안 접힌 모양이라 하위호환. 나머지 필드는 grt_config_ex 와
// 문자 그대로 같다.
static inline void grt_config_ex_shape(const grt_ctx *c, int dataflow, int shape) {
  GRT_INSN_C(c, ((uint64_t)GRT_SCALE_ONE << 32) | (1ull << 16)
             | ((uint64_t)(shape & 7) << 10) | ((uint64_t)dataflow << 2)
             | GRT_CONFIG_EX,
           (1ull << 48) | 0ull, GRT_k_CONFIG);
}
/* E624: 세그먼트 뱅크 **간격**을 함께 준다. strideLog 0 = 간격 1 (기존과 동일).
 * 세그먼트 s 는 뱅크 `base + s*(1<<strideLog) mod sp_banks` 에 놓이므로, 소프트웨어의
 * 가중치 배치도 같은 식을 써야 한다. 기준 뱅크와 간격은 **A 의 뱅크를 밟지 않게**
 * 함께 골라야 한다 (E624b: 뱅크 2 + 간격 2 는 뱅크 0 을 밟아 11.5 % 손해). */
static inline void grt_config_ex_shape_stride(const grt_ctx *c, int dataflow, int shape,
                                              int strideLog) {
  GRT_INSN_C(c, ((uint64_t)GRT_SCALE_ONE << 32) | (1ull << 16)
             | ((uint64_t)(strideLog & 7) << 13)
             | ((uint64_t)(shape & 7) << 10) | ((uint64_t)dataflow << 2)
             | GRT_CONFIG_EX,
           (1ull << 48) | 0ull, GRT_k_CONFIG);
}

// ShapeshiftGemvLoop (funct 23/24). 소프트웨어 계약은 ShapeshiftGemvLoop.scala 참조:
// config_ex 의 shape 와 여기 shape 가 일치해야 하고, W 는 세그먼트 배치로 sp 상주,
// x 는 뱅크 0 의 aBase 부터 tile 당 M 행.
#define GRT_k_SS_GEMV_CONFIG 23
#define GRT_k_SS_GEMV        24
static inline void grt_ss_gemv_config(const grt_ctx *c, int K, int N, int M, int shape,
                                      int aBase, int bBase, int accBase) {
  GRT_INSN_C(c, ((uint64_t)K << 32) | ((uint64_t)N << 16) | ((uint64_t)shape << 8) | (uint64_t)M,
           ((uint64_t)accBase << 32) | ((uint64_t)bBase << 16) | (uint64_t)aBase,
           GRT_k_SS_GEMV_CONFIG);
}
static inline void grt_ss_gemv(const grt_ctx *c, void *y) {
  GRT_INSN_C(c, (uint64_t)y, 0ull, GRT_k_SS_GEMV);
}
/* E610: pp 블록 범위만 실행한다 (ppCount = 0 이면 끝까지 — grt_ss_gemv 와 동일).
 * 적재와 계산을 섞어 발행해야 겹치므로(E608/E609), 소프트웨어가 블록마다 부른다. */
static inline void grt_ss_gemv_blk(const grt_ctx *c, void *y, int ppStart, int ppCount) {
  GRT_INSN_C(c, (uint64_t)y,
             ((uint64_t)(uint16_t)ppCount << 16) | (uint64_t)(uint16_t)ppStart, GRT_k_SS_GEMV);
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

/* E566 경고: rows 는 하드웨어에서 log2Up(DIM+1) 비트다 (DIM=16 이면 5비트, 최대 31).
 * 그 이상을 넣으면 상위 비트가 조용히 잘린다 — rows=32 는 0 이 되고 LoadController 가
 * 16행만 실어, 남은 행이 영원히 0 으로 남는다. E561~E565 의 "16-절단" 전체가 이것.
 * DIM 을 넘는 적재는 grt_mvin_rows 를 쓰라. */
static inline void grt_mvin(const grt_ctx *c, const void *dram, uint32_t sp,
                            int cols, int rows) {
  GRT_INSN_C(c, (uint64_t)dram,
           ((uint64_t)rows << (GRT_ADDR_LEN + 16)) | ((uint64_t)cols << GRT_ADDR_LEN) | sp,
           GRT_k_MVIN);
}
/* 큰 적재를 DIM 행씩 쪼갠다. dram_row_bytes = config_ld 에 준 DRAM 행 간격(바이트) —
 * 분할 조각의 DRAM 포인터 전진에 필요하다 (sp 는 행 단위로 전진). */
static inline void grt_mvin_rows(const grt_ctx *c, const void *dram, uint32_t sp,
                                 int cols, int rows, size_t dram_row_bytes) {
  int done = 0;
  while (done < rows) {
    int nb = rows - done > c->dim ? c->dim : rows - done;
    grt_mvin(c, (const char *)dram + (size_t)done * dram_row_bytes, sp + (uint32_t)done, cols, nb);
    done += nb;
  }
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
// !!! REP > 1 사용 금지 !!! compute 없이 preload를 연속 발행하면 가속기가 멎는다(E222).
// REP=3에서 ping·ssh 모두 무응답, 재프로그래밍이 필요했다. REP=1만 안전하다.
//
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

// E225: mvin 개수만 바꾸는 판. 여분 mvin은 **쓰이지 않는 스크래치패드 주소**로 보내므로
// 결과가 바뀌지 않는다. preload 반복(E222)과 달리 mvin은 파이프라인 순서 제약이 없다.
// 목적: c_issue를 명령 **종류별**로 분해 — mvin 개수 1~4에서 선형 회귀.
#define GRT_SP_X0 192
#define GRT_DEFINE_STEP_RM(NAME, OP, DIM, EB, XMV)                                     \
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
  for (int _x = 0; _x < (XMV); _x++)                                                     \
    grt_mvin(&k, (const char *)w->B + ((size_t)s->k * w->N + s->j) * (EB),               \
             GRT_SP_X0 + (_x)*(DIM), (DIM), (DIM));                                      \
  grt_preload(&k, bsp, (s->k == 0) ? GRT_ACC(s->j) : GRT_ACC_ACC(s->j),                  \
              (DIM),(DIM),(DIM),(DIM));                                                  \
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
GRT_DEFINE_STEP_RM(grt_m1a, GRT_OP_INT8, 16, 1, 1)
GRT_DEFINE_STEP_RM(grt_m1b, GRT_OP_FP32, 16, 1, 1)
GRT_DEFINE_STEP_RM(grt_m2a, GRT_OP_INT8, 16, 1, 2)
GRT_DEFINE_STEP_RM(grt_m2b, GRT_OP_FP32, 16, 1, 2)
GRT_DEFINE_STEP_RM(grt_m3a, GRT_OP_INT8, 16, 1, 3)
GRT_DEFINE_STEP_RM(grt_m3b, GRT_OP_FP32, 16, 1, 3)

// E227: B를 **4타일씩** 싣는 판. E226에서 mvin 비용이 폭과 무관하게 명령당 76 cycle로
// 일정함이 확인됐으므로, 1타일씩 싣는 것은 그대로 4배 손해다.
//
// 4타일 그룹마다 mvin 한 번(cols=4·DIM), 그 뒤 4번의 preload/compute는 적재 없이 돈다.
// config_ld의 block_mvin_stride가 DIM이므로 4타일은 bsp + 0/DIM/2·DIM/3·DIM에 놓인다.
// 스크래치패드: A 0~15, B0 64~127, B1 128~191 (각 4타일 = 64행).
// 제약: N이 4타일(64원소)의 배수여야 한다.
#define GRT_SP_WB0 64
#define GRT_SP_WB1 128
#define GRT_DEFINE_STEP_W(NAME, OP, DIM, EB)                                           \
static void NAME(const grt_work *w, grt_cursor *s) {                                   \
  if (s->done) return;                                                                 \
  static const grt_ctx k = { .dim=(DIM), .elem_bytes=(EB), .acc_bytes=4, .opcode=(OP) };\
  int sub = (s->j / (DIM)) & 3;                                                        \
  if (s->j == 0) {                                                                     \
    grt_config_ld(&k, (uint64_t)w->K * (EB));                                           \
    grt_mvin(&k, (const char *)w->A + ((size_t)s->i * w->K + s->k) * (EB),              \
             GRT_SP_A, (DIM), (DIM));                                                   \
    grt_config_ld(&k, (uint64_t)w->N * (EB));                                           \
  }                                                                                     \
  uint32_t bsp = s->par ? GRT_SP_WB1 : GRT_SP_WB0;                                      \
  if (sub == 0)                                                                          \
    grt_mvin(&k, (const char *)w->B + ((size_t)s->k * w->N + s->j) * (EB),               \
             bsp, 4*(DIM), (DIM));                                                       \
  grt_preload(&k, bsp + sub*(DIM), (s->k == 0) ? GRT_ACC(s->j) : GRT_ACC_ACC(s->j),      \
              (DIM),(DIM),(DIM),(DIM));                                                  \
  grt_compute(&k, GRT_SP_A, GRT_GARBAGE, (DIM),(DIM),(DIM),(DIM));                       \
  s->j += (DIM);                                                                          \
  if (((s->j / (DIM)) & 3) == 0) s->par ^= 1;                                             \
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
// E228: E227의 기제 가설 검증판 — 넓은 mvin이 공유에서 손해인 이유가 "이중 버퍼가
// 4 compute마다 한 번만 뒤집혀 겹칠 여유가 없다"면, 버퍼를 **4개**로 늘리면 회복돼야 한다.
// 스크래치패드 64/128/192/256에 각 4타일(64행). A는 0~15. 총 320행 (한계 16384).
#define GRT_DEFINE_STEP_W4(NAME, OP, DIM, EB)                                          \
static void NAME(const grt_work *w, grt_cursor *s) {                                   \
  if (s->done) return;                                                                 \
  static const grt_ctx k = { .dim=(DIM), .elem_bytes=(EB), .acc_bytes=4, .opcode=(OP) };\
  int sub = (s->j / (DIM)) & 3;                                                        \
  if (s->j == 0) {                                                                     \
    grt_config_ld(&k, (uint64_t)w->K * (EB));                                           \
    grt_mvin(&k, (const char *)w->A + ((size_t)s->i * w->K + s->k) * (EB),              \
             GRT_SP_A, (DIM), (DIM));                                                   \
    grt_config_ld(&k, (uint64_t)w->N * (EB));                                           \
  }                                                                                     \
  uint32_t bsp = 64 + (uint32_t)(s->par & 3) * 64;                                      \
  if (sub == 0)                                                                          \
    grt_mvin(&k, (const char *)w->B + ((size_t)s->k * w->N + s->j) * (EB),               \
             bsp, 4*(DIM), (DIM));                                                       \
  grt_preload(&k, bsp + sub*(DIM), (s->k == 0) ? GRT_ACC(s->j) : GRT_ACC_ACC(s->j),      \
              (DIM),(DIM),(DIM),(DIM));                                                  \
  grt_compute(&k, GRT_SP_A, GRT_GARBAGE, (DIM),(DIM),(DIM),(DIM));                       \
  s->j += (DIM);                                                                          \
  if (((s->j / (DIM)) & 3) == 0) s->par = (s->par + 1) & 3;                               \
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
GRT_DEFINE_STEP_W4(grt_step_w4_i8,  GRT_OP_INT8, 16, 1)
GRT_DEFINE_STEP_W4(grt_step_w4_i8b, GRT_OP_FP32, 16, 1)
GRT_DEFINE_STEP_W4(grt_step_w4_i8c, GRT_OP_C1,   16, 1)

GRT_DEFINE_STEP_W(grt_step_w_i8,  GRT_OP_INT8, 16, 1)
GRT_DEFINE_STEP_W(grt_step_w_i8b, GRT_OP_FP32, 16, 1)
GRT_DEFINE_STEP_W(grt_step_w_i8c, GRT_OP_C1,   16, 1)

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

/* ---------------------------------------------------------------------------
 * SSGemm 계획기 (E638). 이 트랙의 측정 규칙을 코드로 옮긴 것이다 — 지금까지는
 * 산문으로만 있었다. 근거는 각 항에 실험 번호로 달아 둔다.
 *
 *   grt_ss_plan(&plan, M, K, N, sp_banks, sp_bank_entries)
 *
 * 반환: shape(0/1/2), strideLog, 그리고 상주 불가 시 필요한 N 분할 수.
 * ------------------------------------------------------------------------- */
typedef struct {
  int shape;        /* 0 = 16x16, 1 = 8x32, 2 = 4x64 */
  int segs;         /* S = 1 << shape */
  int stride_log;   /* config_ex 의 세그먼트 뱅크 간격 log2 (E624) */
  int nsplit;       /* 1 이면 상주. >1 이면 N 을 이만큼 쪼개야 한다 */
  int rows_per_bank;/* B 의 뱅크당 필요 행 (진단용) */
  int a_rows;       /* A 발자국 = (K/h)·M 행. 뱅크 0 을 넘으면 배치 불가 (E639) */
  int a_base;       /* 권장 A 시작 행 (뱅크 0 위쪽). <0 이면 자리가 없다 (E640) */
  int spill_rows;   /* 세그먼트 넘침이 뱅크 0 을 침범하는 행 수. A 는 그 위에 두어야 한다 */
  int risky;        /* 1 이면 검증 범위 밖 — 보드가 wedge 할 수 있다 (E641/E643). 쓰지 말 것. */
  int use_wide;     /* 1 이면 4-타일 wide mvin 을 쓸 것 (E759). 접힌 배치는 세그먼트마다
                     * 따로 실어 **명령 수가 S 배**이므로, 명령을 4 배 줄이는 wide 가
                     * 기본 팔(1.16×)보다 접힌 팔(1.40~1.50×)에 훨씬 크게 듣는다.
                     * 조건은 `rpb % 64 == 0` (E715) — 안 맞으면 조용히 틀리므로 0 이다. */
  int use_fsm;      /* 0 이면 **FSM 을 쓰지 말고** 기본 타일 경로를 쓸 것 (E648). M>=16 에서
                     * FSM(shape 0)이 타일 경로보다 7 % 느리다. 그 경로는 shapeshift 보드에서
                     * 기본과 0.1 % 안에서 같으므로 손해가 없다. */
} grt_ss_plan_t;

/* E702: **체제(regime)가 shape 규칙을 바꾼다.** 아래 M 규칙은 가중치가 scratchpad 에
 * 상주할 때의 것이다. 토큰마다 DRAM 에서 다시 싣는 **차가운 스트리밍**에서는 세 모양
 * ([256x256], [256x512], [512x256]) 과 M=1/8 전부에서 **8x32 (S=2) 가 최적**이고 4x64 는
 * 세 shape 중 **꼴찌**다 (S=2 가 S=4 대비 1.36~1.41x, 크기에 거의 무관 — E700/E702).
 * 상주 규칙을 그대로 쓰면 접기 이득이 1.52x 대신 1.08x 로 보인다 (E701).
 * `streaming = 1` 을 주면 그 규칙으로 고른다. */
/* E837c: `max_sh` = **하드웨어가 지원하는 최대 shape 인덱스** (0/1/2 = S 1/2/4).
 * 이것이 필요한 이유는 안전이다: mesh 의 shape 포트는 `log2Up(segsSupported.size)`
 * 비트라, `max_segments=2` 보드에 shape 2 를 발행하면 **유효한 shape 로 aliasing 되어
 * 조용히 틀린 답이 나온다**(hang 이 아니다 — E587). 그런데 계획기는 M<=12 면 무조건
 * shape 2 를 골랐다. E837 이 `max_segments=2` 를 권장 구성으로 만들었으므로, 그 보드에서
 * 기존 계획기를 쓰면 **전 단계가 조용히 틀린다.**
 * 하드웨어의 `max_segments` 는 소프트웨어가 알 수 없으므로 호출자가 넘겨야 한다. */
static inline void grt_ss_plan_hw(grt_ss_plan_t *o, int M, int K, int N,
                                  int sp_banks, int sp_bank_entries, int streaming,
                                  int max_sh);
static inline void grt_ss_plan_ex(grt_ss_plan_t *o, int M, int K, int N,
                                  int sp_banks, int sp_bank_entries, int streaming) {
  grt_ss_plan_hw(o, M, K, N, sp_banks, sp_bank_entries, streaming, 2);
}
static inline void grt_ss_plan(grt_ss_plan_t *o, int M, int K, int N,
                               int sp_banks, int sp_bank_entries) {
  grt_ss_plan_ex(o, M, K, N, sp_banks, sp_bank_entries, 0);
}
static inline void grt_ss_plan_hw(grt_ss_plan_t *o, int M, int K, int N,
                                  int sp_banks, int sp_bank_entries, int streaming,
                                  int max_sh) {
  if (max_sh < 0) max_sh = 0;
  if (max_sh > 2) max_sh = 2;
  /* --- shape: M 축 규칙 (E593/E596, 8 뱅크 보드에서 E627 로 갱신) ---
   * M <= 12 -> 4x64.  M >= 16 -> 접지 않는다.
   * 8x32 은 8 뱅크 보드의 12 개 (M, 크기) 칸 어디에서도 단독으로 이기지 않아
   * 선택지에서 뺀다 (E627). M=12·작은 K·N 은 삼자 동률이라 4x64 로 두어도 무해. */
  int sh = (M <= 12) ? 2 : 0;
  /* E639: M 이 8 이상이면서 K·N 이 크면 4x64 가 8x32 에 진다 — pp 블록당 비용이 S 와 함께
   * 뛰기(E632) 때문에 N 이 커질수록 접기의 이득이 상쇄된다. 경계는 측정으로
   * **(122.9k, 147.5k]** 이고 여기서는 128 KiB 를 대표값으로 쓴다. M<=4 에서는 같은 K·N
   * 에서도 4x64 가 30~50 % 이기므로 M 조건이 필수다. */
  if (M >= 8 && (long)K * (long)N > 128L * 1024L) sh = 1;
  if (sh > max_sh) sh = max_sh;      /* E837c: 하드웨어가 못 하는 shape 은 고르지 않는다 */
  /* E702/E706: 차가운 스트리밍이면 M 규칙을 덮어쓴다. **M 경계는 상주와 같은 12/16 이고
   * 바뀌는 것은 접는 정도뿐이다** — 상주는 4x64, 스트리밍은 8x32.
   * 측정 ([256x256], 5 회 최소값, S=1/S=2/S=4):
   *   M=1  8137 / **6520** / 9200      M=8  8140 / **6637** / 9385
   *   M=12 8155 / **7367** / 9437      M=16 **8102** / 8817 / 10487
   * M=16 은 어차피 아래에서 use_fsm=0 이 되어 타일 경로로 떨어지므로 손댈 필요가 없다. */
  /* E744: **스트리밍 전용 shape 규칙을 제거한다.** E700~E716 의 규칙(S=2, K·N 하한)은
   * 두 비교 팔의 회전 작업집합이 달랐던 혼입에서 나온 것이고(E742), 작업집합을 통일하면
   * 세 shape 의 차가 **0.1~3.7 %** 로 접힌 경로의 배치 간 변동(6~7 %)보다 작다 —
   * 즉 **측정 불가능한 차이**다. 상주 규칙만 쓰고, 어느 쪽을 골라도 무해하다.
   * (`use_fsm` 의 체제 절은 유효하므로 남긴다 — 그쪽은 1.13~1.18× 로 잡음 위다.) */
  /* 접힌 shape 은 N 이 16*S 의, K 가 h 의 배수여야 한다 — 아니면 한 단계 덜 접는다. */
  while (sh > 0 && (N % (16 << sh) || K % (16 >> sh))) sh--;

  /* --- stride: 뱅크당 필요 행이 뱅크 용량을 넘으면 세그먼트 간격을 벌린다 (E624/E626) ---
   * 상한은 sp_banks/S — 넘기면 세그먼트가 감겨 서로를 덮어 전 단계 FAIL 한다 (E626 음성 대조).
   * E638: 고른 shape 이 그래도 안 들어가면 **더 접는다**. 접으면 뱅크당 행이 S 배 줄기
   * 때문이다 (rows/bank = N·K/(16·S) at h>=4). 이 검사가 없으면 M>=16 + 큰 K·N 에서
   * 상주 불가능한 shape 0 을 골라 놓고 nsplit=1 이라고 보고했다. */
  int S = 1 << sh, h = 16 >> sh, w = 16 * S, ks = h < 4 ? 4 : h;
  int need = (N / w) * (K / h) * ks;
  int cap = (S > 1) ? sp_banks / S : 1;
  int st = 1;
  while ((long)st * sp_bank_entries < need && st < cap) st *= 2;
  while ((long)st * sp_bank_entries < need && sh < max_sh) {   /* E837c: 여기도 상한 */
    int nsh = sh + 1;
    if (N % (16 << nsh) || K % (16 >> nsh)) break;   /* 더 접을 수 없으면 포기하고 분할로 */
    sh = nsh;
    S = 1 << sh; h = 16 >> sh; w = 16 * S; ks = h < 4 ? 4 : h;
    need = (N / w) * (K / h) * ks;
    cap = sp_banks / S;
    st = 1;
    while ((long)st * sp_bank_entries < need && st < cap) st *= 2;
  }

  /* --- 그래도 안 들어가면 N 분할 (조각의 열 수가 w 의 배수여야 한다, E623) --- */
  int nsp = 1;
  if (S > 1) {
    const int nt = N / w;
    while ((long)(nt / nsp) * (K / h) * ks > (long)st * sp_bank_entries || nt % nsp) {
      nsp++;
      if (nsp > nt) { nsp = nt; break; }
    }
  }
  int sl = 0; while ((1 << sl) < st) sl++;
  o->shape = sh; o->segs = S; o->stride_log = sl; o->nsplit = nsp; o->rows_per_bank = need;
  o->a_rows = (K / h) * M;   /* E639: 호출자가 뱅크 0 안에 들어가는지 확인해야 한다 */
  /* E640: 세그먼트가 여러 뱅크에 걸치면 마지막 세그먼트의 넘침이 뱅크 0 으로 감길 수 있다.
   * 그때 A 를 넘침 구간 위에 두지 않으면 **B 를 덮어 보드가 wedge 된다**. */
  { const int nb = (need + sp_bank_entries - 1) / sp_bank_entries;
    int hits0 = 0, j, sg;
    for (sg = 0; sg < S; sg++)
      for (j = 0; j < nb; j++)
        if (((1 + sg * st + j) % sp_banks) == 0) hits0 = 1;
    o->spill_rows = (hits0 && nb > 1) ? (need - (nb - 1) * sp_bank_entries) : 0;
    { const int base = sp_bank_entries - o->a_rows;
      o->a_base = (o->a_rows > sp_bank_entries || base < o->spill_rows) ? -1 : base; } }
  /* E643: 접힌 shape 2 는 K·N > 147.5 KiB 에서 **보드를 wedge 시킨 사례가 둘** 있고, 계산
   * 가능한 어떤 배치 변수로도 예측되지 않는다 (뱅크 0 접촉 가설은 반증). 검증된 범위를
   * 넘으면 표시만 하고 선택은 호출자에게 맡긴다 — M<=4 에서는 shape 2 가 38~53 % 빠르므로
   * 무조건 막으면 손해가 크다. */
  o->risky = (o->shape == 2 && (long)K * (long)N > 147456L) ? 1 : 0;
  o->use_fsm = (M < 16) ? 1 : 0;
  { const int h_ = 16 >> o->shape, ks_ = h_ < 4 ? 4 : h_;
    o->use_wide = (((K / h_) * ks_) % 64 == 0) ? 1 : 0; }
  /* E707: `use_fsm` 도 체제 의존이다. 상주에서는 M>=16 일 때 FSM 이 타일보다
   * 7 % 느려 타일로 떨어뜨리는 것이 옳다(E648). **차가운 스트리밍에서는 반대로
   * FSM(S=1)이 타일보다 31 % 빠르다** — [256x256] M=16 에서 8,102 대 10,615.
   * 적재가 지배하면 FSM 의 적재 파이프라이닝이 이기기 때문이다. */
  if (streaming) o->use_fsm = 1;   /* E648: M>=16 은 접기 이득이 없고 FSM 이 오히려 느리다 */
}

/* 계획을 그대로 하드웨어에 적용한다 (B 배치는 호출자가 계획에 맞춰 깔아야 한다). */
static inline void grt_ss_apply(const grt_ctx *c, const grt_ss_plan_t *p, int dataflow) {
  grt_config_ex_shape_stride(c, dataflow, p->shape, p->stride_log);
}


#endif
