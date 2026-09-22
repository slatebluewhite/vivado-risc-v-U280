# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Generator for an FPGA RISC-V SoC (Rocket Chip / BOOM, optional Gemmini accelerator) targeting AMD/Xilinx
boards, plus the full software stack (bootrom, OpenSBI, U-Boot, Linux, Debian rootfs) needed to boot
Debian on it. Everything is driven from the top-level `Makefile`; there is no test suite.

## Terminology in write-ups (JOURNAL.md, chat, comments)

Keep technical terms in their exact original form and explain the *meaning* in plain Korean around
them. Do not invent a Korean translation for something that already has a precise name in the code
or the domain.

- Write `total_rows`, `MeshWithDelays`, `ExecuteController`, preload, scratchpad bank, accumulator,
  segment, skew, systolic array, WS/OS dataflow, `d_row_is_not_all_zeros` — as they are.
- Explain around the term, not instead of it: "preload는 가중치를 배열에 미리 실어두는 단계"
  is good; renaming preload to a coined Korean word is not.
- Signal and parameter names are addresses into the source. A translated name cannot be grepped,
  and a reader cannot match it to the RTL. This has already cost time here: "급전 창 길이" for the
  feed window meant `total_rows`, and the invented name hid that the fix was one existing signal.
- Korean transliterations that are already standard (스크래치패드, 뱅크, 세그먼트) are fine.
  The rule is against *coining*, not against the Korean language.

## Build commands

Vivado must be on PATH first: `source /opt/Xilinx/<version>/Vivado/settings64.sh`.

```
make apt-install                                    # one-time host package install (needs sudo)
make update-submodules                              # after clone or pull; also clears workspace/patch-*-done
make CONFIG=rocket64b2 BOARD=nexys-video bitstream   # full flow: Scala -> Verilog -> Vivado -> .bit + .mcs
make CONFIG=... BOARD=... vivado-project             # create the Vivado project only
make CONFIG=... BOARD=... vivado-gui                 # open that project in the GUI
make CONFIG=... BOARD=... flash                      # program board SPI flash over JTAG
make CONFIG=... BOARD=... jtag-boot                  # program FPGA + download boot.elf/Image via xsdb
make linux                                           # kernel only
make u-boot bootloader                               # U-Boot, then OpenSBI wrapping it -> workspace/boot.elf
./mk-sd-image ; ./mk-sd-card                         # build and write the Debian SD card
./qemu/boot_qemu.sh                                  # boot the same image under QEMU (ssh -p2222 debian@localhost)
```

`BOARD` and `CONFIG` default to `nexys-video` / `rocket64b2`. Persist overrides (including `MEMORY_SIZE`,
`ROOTFS`, `ETHER_MAC`) in `workspace/config`, which the Makefile includes if present. Individual build
stages can be run by naming their output file as the make target, e.g.
`make workspace/rocket64b2/system-nexys-video.v`.

Full bitstream builds take hours and need ~32GB RAM; `MAX_THREADS` defaults to 1 because Vivado
multi-threading causes intermittent failures.

## Build pipeline

Understanding the chain matters more than any single file — each stage is a Makefile rule whose target
lives under `workspace/$(CONFIG)/`:

1. **Config selection.** `CONFIG` is lowercase; the Makefile converts `rocket*` → `Rocket*`
   (`CONFIG_SCALA`) and elaborates `Vivado.$(CONFIG_SCALA)`, a class in `src/main/scala/rocket.scala`.
   Adding a new SoC variant means adding a `Config` class there — the name is the whole interface.
2. **Clock frequency** comes from `board/rocket-freq`, an awk-matched table of
   `<board regex> <config regex> <MHz>`; first match wins. Gemmini and BOOM configs are clocked much lower.
3. **Default device tree**: `sbt runMain freechips.rocketchip.diplomacy.Main` emits `system.dts`
   (cores/caches/CLINT/PLIC only, no peripherals).
4. **Board device tree + bootrom**: `workspace/$(CONFIG)/system.dts` is concatenated with
   `board/$(BOARD)/bootrom.dts` (which describes UART/SD/Ethernet/GPIO under `io-bus`), then `sed`-patched
   with memory range, clock and timebase frequency, MAC address and PHY mode. The result is compiled into
   `bootrom/` and elaboration is re-run so the ROM image lands inside the generated hardware.
5. **FIRRTL → Verilog**: `firrtl.stage.FirrtlMain` produces `system-$(BOARD).v`.
6. **VHDL wrapper**: the ANTLR-based Java tool in `vhdl-wrapper/` parses the generated Verilog and emits
   `rocket.vhdl`, a component declaration Vivado's block designer can instantiate.
7. **Vivado project**: `vivado.tcl` (shared) creates the project, adds the fixed source list
   (`uart/`, `sdc/`, `board/*.v`, `vhdl-wrapper/*.vhdl`) and constraints, then sources the board- and
   Vivado-version-specific block design script `board/$(BOARD)/riscv-<major>.<minor>.tcl`.
8. **Synthesis / implementation / cfgmem** run through generated one-shot tcl files; the rules grep the
   Vivado logs for `ERROR:` and fail the build.

Because the device tree is baked into the FPGA bootrom, one SD card image boots any board/config combo.

## ⚠ `src/main/scala/rocket.scala` 에 파괴적 git 명령을 쓰지 말 것 (E850)

이 파일에는 **수백 줄의 미커밋 연구 코드**가 정상적으로 들어 있다 — shapeshift 계열
config 12 개, 다중 가속기 계열 12 개 이상, 그리고 `WithGemmini*` 헬퍼들. 커밋되지 않은
것이 이 저장소의 정상 상태다.

`git checkout src/main/scala/rocket.scala` **한 번으로 전부 사라진다.** git 은 staged
이력이 없으면 dangling blob 도 남기지 않으므로 복구 수단이 없다. 실제로 2026-09-02 에
그렇게 잃었고(E850), **빌드 산출물에서 파라미터를 역산해 20 개 전부 재구성·등가 확인
했다** (E850a/E850c). 역산이 가능했던 것은 `workspace/<config>/system-u280.v` 가
남아 있었기 때문이다 — 인스턴스 수, 포트 폭, 모듈 유무가 파라미터를 되돌려 준다.

**되돌릴 때는 `git diff <file> > /tmp/backup.patch` 를 먼저 하거나, 내 hunk 만
역적용할 것.** 이것은 `patches/` 쪽 위험(E848a/E848b)과 **다른 경로**다 —
그쪽은 도구에 안전장치를 넣었지만 `git checkout` 자체는 그대로였고, 그래서 같은
실수가 막히지 않은 경로로 다시 났다.

**지금은 `experiments/refresh-patch.sh` 가 실행될 때마다
`patches/.backup/rocket.scala.{1,2,3}` 으로 3 세대 보관한다 (E850b).** 패치 갱신은
RTL 을 만질 때마다 하므로 실질적으로 작업 직전 상태가 남는다. 단 **패치를 안 건드리고
`rocket.scala` 만 오래 고치면 그 사이는 안 남는다** — 그럴 때는 손으로 복사할 것.

**교훈: 안전장치는 사고가 난 _경로_ 가 아니라 잃는 _자산_ 에 붙여야 한다.**

**복구는 끝났다 — `rocket.scala` 는 108 클래스이고 43 개를 재구성해 40 개를 검증했다**
(E850d~E850k). 남은 셋은 `gem4x4`(보존 산출물 없음, elaborate 는 성공),
`gem2x8`/`gem8x2`(**원래도 elaborate 안 됐다** — E153 의 비정방형 조사이고 `w_mask` 대
`sp_width` 충돌에서 막힌다. 재구성하지 않았다).

**역산에 실제로 쓸 수 있는 산출물은 셋이다** (셋 다 처음엔 목록에 없었다):
`workspace/<c>/system-u280.v` (포트 폭·인스턴스 수와 위치·모듈 유무),
`workspace/<c>/system.dts` (L2 `cache-size`/`cache-sets`, 코어 수),
`workspace/<c>/system-u280/RocketSystem.fir` (앞 둘이 없을 때).

**⚠ `auto_spad_id_out_a_bits_data` 는 DMA 폭이 아니라 `SystemBusKey.beatBytes` 다**
(E850i). `WithGemmini(mesh, bus_bits)` 가 `dma_buswidth` 와 `beatBytes` 를 **같은 값으로**
넣기 때문에 그 계열에서는 구별되지 않지만, `w256` config 은 DMA 를 128 로 두는데 그 포트가
**256** 이다. 두 축이 늘 같이 움직이는 표본에서 잰 서명은 그 두 축을 가르지 못한다 —
어긋나는 표본을 하나 찾아 확인할 것.

**등가 검사를 쓸 때는 "통과하면 무엇이 배제되는가" 를 먼저 적을 것.** 이 복구에서 기준의
구멍이 네 개 나왔고 넷 다 표본이 우연히 균질해서 안 보였다: ① 최상위 모듈 본문 diff 는
**배치**를 못 본다 (가속기 3 개짜리와 2 개짜리가 diff 0 — 표본 20 개가 전부 1 코어였다),
② 클럭 접미사(`f40`)는 RTL 무관이라 통과가 **동어반복**, ③ 기준 산출물이 현재 RTL 보다
**오래됐을 수 있다** (`gem8ssf2` 의 diff 82 는 GEMV FSM 추가 이전 파일이라서다),
④ 최상위만 보면 **서브모듈만 바꾸는 파라미터**를 놓친다 (`tile_latency` 가 그렇다).
검사는 `experiments/verify-config.sh` 에 있고 ①을 막는다 (음성 대조 포함).

**그리고 전면 비교(전 모듈 본문 해시)는 corpus 검증에 못 쓴다** — 기준 산출물과 현재 RTL
사이의 **패치 드리프트**를 재구성 오류로 읽는다 (shapeshift 를 안 켠 config 에서도 다섯
모듈이 갈리고, 옛 산출물엔 `module Mesh` 가 오늘 빌드엔 `module ShapeshiftMesh` 가 있다).
**드리프트에 면역인 형태로 물을 것: 두 config 의 _차이_ 를 두 RTL 상태에서 각각 재어
비교하면 공통 오염원이 상쇄된다.** 이것으로 `l1 = tile_latency 1` 을 확증했다. 성능 쪽의
"같은 바이너리 안에서 비교하라 / 비를 인용하라" 와 같은 형태이고, 축만 다르다.

**역산할 때는 산출물보다 기록을 먼저 grep 할 것.** 이 복구에서 세 번, 답이 이미 JOURNAL 과
CLAUDE.md 안에 있었는데 코드를 먼저 뒤졌다 — `gem2x8`/`gem4x4` 의 정체(E153),
`GemminiFPConfigs.BF16DefaultConfig` 의 존재(CLAUDE.md 가 명시하고 있었다),
`l1` 의 뜻("tested 2 and 1"). 기록은 파라미터가 아니라 **의도** 를 담고 있고, 의도를 알면
산출물에서 무엇을 확인해야 하는지가 정해진다. 관련해서 **"X 가 없다" 는 부재 주장이고
한 파일을 보고는 못 세운다** — `Configs.scala` 만 읽고 "Float config 가 없다" 고 적었으나
`ConfigsFP.scala` 에 있었다.

## Submodules and the patch model

`rocket-chip`, `linux-stable`, `u-boot`, `opensbi`, `generators/*`, `ethernet/verilog-ethernet` are
submodules. **Never commit edits inside them** — `make clean-submodules` / `update-submodules` wipes
working trees. Instead:

- Put HDL/generator changes in `patches/<name>.patch`; `workspace/patch-hdl-done` applies them
  (idempotently — it tries `git apply -R --check` first).
- Linux: `patches/linux.patch`, `patches/linux.config`, and the out-of-tree drivers
  `patches/fpga-axi-{sdc,eth,uart}.c` which are copied into the kernel tree.
- U-Boot: `patches/u-boot.patch` plus the board port under `patches/u-boot/vivado_riscv64*`.
- OpenSBI: `patches/opensbi/` is copied to `opensbi/platform/vivado-risc-v` (the platform is defined
  entirely by these files); the payload is U-Boot, and the output is `workspace/boot.elf`.
- Delete `workspace/patch-*-done` to force re-application.

`generators/gemmini/software/gemmini-rocc-tests` is a *nested* submodule listed in
`SKIP_SUBMODULES`, so nothing checks it out or patches it automatically. Its fixes live in
`patches/gemmini-rocc-tests.patch` and must be applied by hand (the header of that file has
the commands). Two of them matter for any measurement work: user-mode `rdcycle` traps on
modern kernels, and the CPU softmax reference is wrong in a way that makes correct hardware
look broken. Note also that `include/gemmini_params.h` in that tree is *generated* by Chisel
elaboration and is overwritten per CONFIG — build test binaries only after elaborating the
config you intend to run them on.

## Adding a peripheral

The three places that must agree: the Vivado block design (added via `vivado-gui`, saved into
`board/$(BOARD)/riscv-*.tcl`), the `io-bus` node in `board/$(BOARD)/bootrom.dts` (address and interrupt
number must match the design), and the driver's `CONFIG_*=y` in `patches/linux.config`. Then rebuild
kernel/bootloader and the bitstream.

## Board support

Each `board/<name>/` supplies `Makefile.inc` (`XILINX_PART`, `BOARD_PART`, `CFG_DEVICE`, `CFG_PART`,
`MEMORY_SIZE`), `bootrom.dts`, `top.xdc`/`sdc.xdc`/`uart.xdc`, an `ethernet-<board>.{v,tcl}` pair, and one
block-design tcl per supported Vivado version. Adding support for a new Vivado release means adding a new
`riscv-<ver>.tcl` for every board — these are generated by exporting the block design from the GUI.

The clock a board/config pair runs at is *not* in `Makefile.inc` — it comes from the awk table in
`board/rocket-freq`, so a new board needs an entry there too or it falls through to the 31.25 MHz default.

The Alveo/PCIe cards (`u200`, `u250`, `u280`, `vcu1525`, `x3522pv`) form a sub-family: no SD slot, so
`bootrom.inc` swaps in a `bootrom.c` that just parks the harts for JTAG, `CFG_BOOT` appends `boot.elf` to
the flash image, and `ROOTFS ?= NFS`. They also vendor the Xilinx board files under
`board/<name>/board_files/`, which `vivado.tcl` registers via `board.repoPaths`; almost every off-chip
connection (DDR4, PCIe, IIC, PERSTn) is then made through board interfaces rather than explicit LOCs, so
those names must match the board file exactly. Their block-design tcls are otherwise interchangeable —
`u200`, `u250` and `vcu1525` are byte-identical, and `u280` differs only where the card does
(2 DDR4 channels on `sysclk0`/`sysclk1` at 100 MHz, a different GTY quad, no FPGA-side QSFP low-speed
pins, and `hbm_cattrip` held low). `u280` supports Vivado 2021.2-2023.2 only — do not add newer
`riscv-<ver>.tcl` files there.

## Bare-metal

`bare-metal/{hello-world,coremark,dhrystone}` build `boot.elf` (loaded from the SD card DOS partition by
the bootrom) using the RV toolchain auto-downloaded to `workspace/gcc/riscv`. Shared flags live in
`bare-metal/common.mk`; set `FPGA_CPU_CLK_FREQ` there to match the actual bitstream clock or benchmark
results are wrong.

## Gemmini on U280 — measured findings

Full experiment log: `experiments/JOURNAL.md` (E1–E95). The short version:

**Recommended configs** (both in `src/main/scala/rocket.scala`):

| Use | Config | Result |
|---|---|---|
| CNN / general | `Rocket64b2gem16wf40` | ResNet-50 **2.46 s (1.55× vs `rocket64b2gem16`)**, +1.4%p LUT |
| Transformer | `Rocket64b2gem16n` | BERT-base 221 ms/layer; LayerNorm/Softmax/iGELU work |

**Where the performance came from.** Not from more compute. Four axes were built and measured:
clock +28% (free, biggest single win), bus width 64→128 bit (+1.4%p LUT), L2 512KB→2MB, and
4× Gemminis. Clock and bus **multiply** (1.31 × 1.18 = 1.55, measured 1.55×). L2 adds ~2% once
bandwidth is fixed. Four Gemminis cost +32%p LUT and gained 2% — the array was never the
bottleneck. Prefer clock and bus width.

**Clock.** The 31.25 MHz in `board/rocket-freq` is conservative for u280; 40 MHz closes
(WNS +0.16). Name a config `...f40` and the existing regex picks 40 MHz up. Do not combine
40 MHz with `WithGemminiNorm` — the normalization unit leaves only +0.013 ns at 31.25 MHz.
The critical path is the (unpipelined) `scale_func` chain in `AccumulatorScale`'s
`num_scale_units == -1` branch; `acc_scale_args.latency` does *not* split it (it appends
registers after the combinational block).

**Do not use the OUTPUT_STATIONARY dataflow.** It accumulates in 20-bit PE registers
(`spatialArrayOutputType`) instead of the 32-bit accumulator, so INT8 saturates after ~32
accumulation steps, and intermittent wrong results were observed on the raw preload/compute
path. Everything in the Gemmini library (`tiled_matmul_auto`, ResNet, BERT) uses WS; that path
was clean over 1.28 M tile operations.

**Accumulator precision is 24 bits, not 32.** `accType` is `SInt(32)`, but mvout scaling
converts through `Float(8, 24)` (`Configs.scala:118`), so values above 2^24 are rounded.
Split large-K matmuls if exactness matters.

**Gemmini is pinned to an old commit and upstream bug fixes are missing.** Two
`write_issue_q` valid-check fixes were needed to make BF16 stop deadlocking. The upstream
`data drop` fix (`493c45a`) **cannot be applied alone** — it deadlocks without the matching
`Mesh`/`MeshWithDelays` changes. Check `git log HEAD..origin/master` in `generators/gemmini`
before blaming the hardware.

**BF16 (`WithGemminiBF16`) does not work.** Deadlocks are fixed, but matmul results never
reach the accumulator; `mvin`→accumulator→full-width `mvout` round-trips fine, so the fault is
in the compute→accumulator write. Bundled tests also cannot validate BF16 (`elem_t` is a raw
`uint16_t`, and the CPU reference does integer arithmetic on it).

**Measuring rare failures.** Three metrics were tried and two were wrong: exit codes conflate
`mlockall` failures with test failures, and turning `exit(1)` into a counter counts cascade
errors. Only "compare values, print the diff, keep the log" is trustworthy — see
`bareMetalC/stress_matmul.c` and `bf16_verify.c`.

## Floating-point Gemmini

`Rocket64b2gem8fp32` (`WithGemminiFP32`, 8×8 mesh, Float(8,24)) **works** — Gemmini's own
`matmul`, `tiled_matmul_ws` and `mvin_mvout` all pass, and `tiled_matmul_auto` is clean over
39,680 elements. Peak is 64 MAC/cycle; measured utilization tops out at **81%**.

**BF16 (`WithGemminiBF16`) does not work.** The defect is narrow and characterized: the mesh,
the float multiplier, DMA, scratchpad and accumulator are all fine, but **loading weights via
the `preload` instruction always yields zero**, which is exactly what the standard WS flow in
`sp_tiled_matmul_ws` does — so every library matmul returns zeros. Passing B through
`compute_preloaded` instead works (98%). Not caused by `tile_latency` (tested 2 and 1), not a
width mismatch (`inputType == outputType` for BF16), and no software workaround exists (fence
and repeated preload both fail). Use FP32 unless you want to debug hardfloat handling of
`Float(8,8)`.

**Batch size matters more than expected, and differently per mesh size** (same benchmark,
K=N=256, no bias, best of 3×N runs):

| batch M | INT8 16×16 | FP32 8×8 |
|---|---|---|
| 8 | 22.5% | 38.5% |
| 16 | 42.2% | 67.4% |
| 64 | **80.7%** | 81.2% |
| 256 | 61.0% | **81.6%** |

A 16×16 mesh needs M≈64 to fill and loses efficiency again at M=256; an 8×8 mesh is far less
sensitive. For small-batch inference the INT8 advantage shrinks from 4× to 2.3×.

Other measured effects: bias (`D != NULL`) costs **+17–34%**, growing with N; K below ~128
costs about 13 points of utilization; matrix size shows **no scratchpad capacity cliff** up to
256×256 FP32 (`tiled_matmul_auto` tiles it away).

**BERT-base encoder layer** (seq=128, hidden=768, FFN=3072, 12 heads), same code on both:

| | INT8 16×16 @40MHz | FP32 8×8 @31.25MHz |
|---|---|---|
| one layer | **135 ms** | 691 ms |
| utilization | **86.1%** | 67.3% |

INT8 is **5.1×** faster — more than the 4× PE ratio, because BERT is dominated by large-K
matmuls (K=768, 3072) where the bigger mesh wins. Attention itself is only 4% of the layer;
the projections and FFN are 96%, so benchmarking attention alone tells you almost nothing.

**Benchmark methodology** (learned the hard way here):
- Measure **best-of-3 sets of ≥10 iterations**. Single short runs produced 3× errors — see
  JOURNAL E113, retracted wholesale in E114–E116.
- **Compare only measurements made with the same code.** Comparing a hand-written layer against
  the bundled `transformer` benchmark gave 3.13× where the matched comparison gives 5.12×
  (E119 → E120).
- **Best-of-N does not catch a disturbance that outlasts the whole set.** A 192³ point came in
  at 1.34× against 1.84× at 128³ despite best-of-5; re-running *the same binary* on the same
  bitstream and boot gave 1.89×, matching the neighbours. Best-of-N protects against single slow
  reps, not against something that degrades all N. When a point violates a shape you have
  physical reason to expect (here, monotonicity), re-run the identical binary before explaining
  it — that, not more reps, is what separates "the code did it" from "the measurement did it".
- **After fixing a bug, grep the whole corpus for its pattern the same day.** A work-count-depends-on-m
  bug fixed in one function survived in another in the same file for forty experiments, hidden because
  the correctness check was parameterised the same way. Auditing all 164 test programs for both
  patterns took three commands and found nothing else.
- **Never extrapolate a curve's shape from its last two points.** Two separate pre-registered
  predictions failed this way, in mirror image: two rising points read as a trend that was really a
  step, and two flat points read as saturation that was really still climbing. Outside the measured
  range, say "outside range" instead of predicting.
- **Check a claim with a script before writing it as a sentence.** Any "X is within Y of Z" claim is
  a one-line computation. Three conclusions here were wrong for want of it: a session whose starting
  bitstream was assumed rather than stamped, a 25-row sweep whose true minimum was missed by eye, and
  a "within 1 %" summary that was actually off by 16.6 % — written in the same breath as a rule
  saying to script it.
- **Reboot between the two runs when you claim an absolute number.** Re-running the *identical*
  binary on the *identical* bitstream after a reprogram cycle moved m=3 times by **+21–32 %** on
  shapes that were near-ideal (η≈1.2), while m=1 reproduced within 1.5 % and bandwidth-bound shapes
  within 3.5 %. Forty repetitions inside one process all land within 3 %, so best-of-N cannot see
  it — repetition measures that boot, not the hardware. **A 28 % shift that looked exactly like this
  turned out to be the bus width**, found only after stamping the bitstream signature; see the bus
  entry above. One real effect does remain: shapes with slack are **bimodal — 76 % at the minimum and
  24 % about 30 % slower, with the band between empty** — while bandwidth-bound shapes stay unimodal
  within 12 %. Best-of-N minima are uncontaminated (best-of-3 misses the fast mode 1.4 % of the
  time), but means and single-shot numbers are not. **Quantified across sessions**: over 20 cells
  measured in two or more separate experiments, a 5-relaunch per-cell minimum reproduces to
  **0.61 % mean / 0.43 % median / 2.44 % worst at m=3** (0.27 % / 0.17 % / 0.95 % at m=1), with
  0/20 over 3 % — while the individual runs inside each experiment span 2.3–2.7 % on average and
  up to 24 %. So a *difference* between two minima carries ~0.9 %p of typical noise: **do not read
  under 1 %p, treat 1–3 %p with care, trust 3 %p and above.** Those bands describe *two 5-run
  minima against each other*; a 5-run minimum can still sit above the true one. Going 5 -> 10
  relaunches moved one cell in eight by **4.2 %** and its gain figure by 4.3 %p (E423), so
  **use 10 relaunches whenever a conclusion hinges on less than 3 %p** — a boundary, a
  threshold, a tie-break. That corpus cost no board time — it
  came from repeats already present in eight experiments' logs, and short (~3 ms) stages need more repetitions.
  **Measured directly on 72 cells x 6 process relaunches (E305): 21 % of cells span more than
  10 % across runs, 36 % more than 5 %, and only 53 % are within 2 % — and the spread itself
  scales with accelerator count (mean 1.6 / 3.0 / 4.9 % at m=1/2/3, with 0 / 5 / 15 cells over
  10 %). A difference under ~6 % at m=3 is therefore not measurable in a single process, however
  many repetitions it does inside it: relaunch at least five times and take per-cell minima.
  Three secondary conclusions here were built on single-process data and had to be retracted.**
  Assembled *layers* are far cleaner (0.2-3.7 % spread) because the contamination hits stages
  independently and summing dilutes it, so the headline layer numbers moved only 0.3-1.7 % when
  re-measured properly (E306) — it is short single-shape measurements that need the relaunches.
  There is also a **second, much larger disturbance**: in roughly one run in six of a ~30 s
  program, *every* stage comes out uniformly ~2x slower, because this is a single-core board and
  the kernel shares the benchmark's core. **Run every benchmark under `chrt -f 99`** — it removes
  that one completely (0 disturbed runs in 8, spread 106 % -> 0.8 %) and costs nothing. It does
  *not* remove the per-cell bimodality (m=3 spread 4.87 % -> 4.36 %), so both defences are needed.
  **Which configuration is bimodal is a property of the binary, not of the matrix shape, and it
  cannot be predicted or designed around** (E312): the identical computation ([1024x1024], (8,4),
  Kc=32) measures 13.0 % spread in one program and 0.3-1.3 % in another, and adding a single
  semantically-null `memset` to one of them *moved* the bad point from N=1024 to N=1216 rather
  than removing it — each binary has one or two bad cells out of ten, at ~13 %. Ruled out as
  causes: scheduling (chrt), matrix shape, work-vs-stride (strides are a separate argument to
  `grt_block_ksplit`; sweeping them changes nothing), array declaration sizes, call order within
  the process, and — earlier — block traversal order and buffer offset. What is left is the
  program's memory state; the mechanism is unidentified and this board cannot count physical page
  colouring. An earlier reading of this as "N=1024 is a bad point, pad around it" is **retracted**
  (E310/E311 -> E312): moving N moves the problem with it. The only defence is process relaunches
  with per-cell minima, which is also why this project's recorded results are safe — minima are
  uncontaminated; means and single shots are not. The bimodality is otherwise a property of the **configuration, not the run**: the cells that are bimodal
  under normal scheduling are the same ones under SCHED_FIFO (9 of 15 overlap against 2.1 expected
  by chance), which generalizes the earlier note about `(3,2)`/`(2,4)`/`(3,4)` (E307). Use **minima, never means** — every disturbance here is
  one-directional, so the minimum is unbiased and the mean is not (18 % off on one h2048
  schedule) — and report the spread, since a 106 % spread is what pointed at this second mode.
- Element counts are not byte counts: `bwrate` reports elements, so multiply by `sizeof(elem_t)`
  before comparing datatypes.

## Working with the U280 target (as of 2026-08-18)

> **📄 접속·파일 조작의 실무 절차는 `../TARGET-ACCESS.md` (= `/home/jaemin/03_gemmini/TARGET-ACCESS.md`) 를 볼 것 (E858).**
> 그 문서는 2026-09-11 에 보드에서 **직접 확인한 것만** 담았다. 아래 절에는 그 뒤로
> 죽은 경로가 섞여 있다 — 특히 **`scratchpad/tssh.py` 는 없어졌고
> `experiments/tssh.py` 로 옮겼다.** 충돌하면 `TARGET-ACCESS.md` 가 우선이다. (저장소 밖에 두었으므로
> `make clean-submodules` 나 git 명령에 안 날아간다.)

**The serial console TX is dead.** `/dev/ttyUSB2` still *receives* — boot logs and the
`debian login:` prompt appear — but nothing sent from the host is echoed, so you cannot log in
over serial. Not a flow-control issue (`-crtscts -ixon` verified) and not UART FIFO overrun
(0.35 s per character still no response). Use the console for watching boot progress only.

Two traps when the console looks silent: the reader process (`cat /dev/ttyUSB2 >> log`) can hold
a stale fd and keep running while the log stops growing — check the file size before blaming the
target; and `pkill -f` is banned here (it matches your own command line) — resolve PIDs with
`pgrep -x` and kill by PID.

**Reach the target over ssh instead.** After boot the board DHCPs to 192.168.1.120 and sshd comes
up (well after DHCP — wait on port 22, not on ping). There is no `sshpass` on the host and no
passwordless sudo to install one, so drive ssh through a pty:
`scratchpad/tssh.py '<command>' [timeout]` writes the password when it sees `assword`.
Two target-side quirks: `/tmp` is a tmpfs shadow, so files delivered via NFS must be read from
`/mnt2/tmp` after `sudo mount --bind / /mnt2` (non-interactive sudo works as
`echo debian | sudo -S ...`); and `/mnt/ram` is root-owned, so make a work dir
(`sudo mkdir -p /mnt/ram/w && sudo chmod 777 /mnt/ram/w`).

**Rebooting needs no human.** A wedged target recovers with
`env BITSTREAM=<path> HW_SERVER_URL=tcp:localhost:3121 xsdb -quiet board/jtag-boot.tcl`
(run `board/jtag-freq.tcl` first). Source `/opt/Xilinx/Vivado/2023.2/settings64.sh` — note xsdb
lives under `Vivado/`/`Vitis/`, not on the default PATH. Driving xsdb directly instead of
`make jtag-boot` lets you boot any archived `.bit` without make deciding to re-synthesize.

**`timeout` does not bound a RoCC hang.** A hart stuck on a RoCC instruction does not take the
signal, so target-side `timeout` is not a safety net; host-side timeouts plus a ping watch are.

**Print progress to an NFS file, not stdout.** ssh buffers output until the command ends, so a
target that wedges prints nothing at all. Since the rootfs is NFS, writing to `/mnt2/tmp/x.log`
with `fflush` + `fsync` makes progress readable from the host at
`/srv/nfs/debian-riscv64/tmp/x.log` even after the target dies completely. Every diagnostic in
`experiments/tests/` uses this.

## Runtime-parameterized Gemmini library

`experiments/tests/include/gemmini_rt.h` drives Gemmini with `dim`, `elem_bytes` and `opcode` as
**struct fields** instead of the compile-time macros in the generated `gemmini_params.h`. It
includes only `rocc-software/src/xcustom.h`. This is what makes "same binary, different bitstream"
comparisons possible at all — the stock header is regenerated per CONFIG, so every configuration
previously needed its own build and no controlled comparison existed.

Copy encodings from the real macros in `gemmini.h`, never from memory — four bugs came from not
doing so. The expensive one: `GARBAGE_ADDR` is `0xFFFFFFFF`; masking bit 31 off turns it into a
real (huge) scratchpad address and wedges the machine. Also `config_ld`'s scale is the float
bit pattern `0x3f800000` (integer 1 is a denormal and silently zeroes every mvin), and its
`block_mvin_stride` field is DIM. Opcode must stay a compile-time literal (`CAT(CUSTOM_, x)`
token pasting), so runtime selection means branching between two emitted variants.

## Heterogeneous accelerators

Per-tile accelerators work: `BuildRoCC` lambdas receive tile-local `Parameters`, so
`p(TileKey).tileId` selects *which* accelerator a tile gets. To vary *how many* (including none),
branch on `site(TileKey).tileId` and return Seqs of different length — `Seq(a, b)` for tile 0 and
`Seq()` for tile 1 elaborates cleanly.

**Two different Gemminis on two tiles wedges the system bus** (`Rocket64b2gemhet`). Any custom
instruction on tile 1 — even `config_ex`, which touches neither memory nor TLB — kills the whole
machine: a watchdog pinned to core 0 dies at the same instant, and ping stops. It is not the
opcode (both tiles share one `RoccCommandRouter` module decoding `7'h7b`), not tile 1 (INT8-only
and FP32-only builds both pass on cpu1 with the same binary), not the accelerator or tile RTL
(diff 0 lines against a working build), and not timing (WNS +0.195, WHS +0.009, 0 failing
endpoints). Unresolved; the evidence points at the TileLink side. See JOURNAL E123–E129.

`Rocket64b2gembl` sidesteps it and **works** — verified end to end, including ResNet-50 (PASS,
1,004,937 timer ticks = 3.22 s at 31.25 MHz, which matches d9's 2.46 s at 40 MHz to within 2 %
once scaled, so the idle FP32 accelerator costs nothing measurable). Details: core 0 carries **both** accelerators
(INT8 16×16 on custom3, FP32 8×8 on custom2) and core 1 carries none, which keeps two cores for
Linux and puts every accelerator on the tile already known to work. One binary drives both
accelerators from one core — no task passing, no per-config rebuild — and core 1 rejects custom
instructions with a clean SIGILL rather than wedging anything. That last point also narrows the
open bug: asymmetric RoCC across tiles is *not* what breaks it; only two *different* Gemmini
designs, one per tile, do. Area is the reason for this shape rather than one Dual-Gemmini core
per tile — see below.

**Use `Rocket64b2gemblf50`, the 50 MHz variant** — ResNet-50 in **1.945 s**, 21 % faster than
`d9` while also carrying an FP32 accelerator, timing clean (WNS +0.091, WHS +0.009, 0 failing).
Note that 50 MHz was reached only by *trying* it: extrapolating from the 40 MHz slack predicted
failure by 1.5 ns, but the critical path is not a fixed quantity — Vivado stops optimizing once a
loose constraint is met, and works harder under a tight one. **Never infer fmax from slack
measured at a looser target.** The frequency ladder is also constrained to 1000/N MHz by the
clock wizard (Makefile line 197 lists the valid values); 45 MHz simply cannot be generated.

Earlier notes on the 40 MHz variant: Same area (LUT 40.48 %), timing still clean
(WNS +0.143, WHS +0.009, 0 failing), and it runs ResNet-50 in **2.465 s** — matching `d9`'s 2.46 s
while also carrying an FP32 accelerator. Both configs cost two accelerators' worth of area; this
one spends the second on FP32 instead of a rarely-used second INT8. Throughput scales *exactly*
with the clock (1.280× at every size up to N=192), so the design is compute-bound, not
memory-bound, at 40 MHz. The Rocket/Gemmini clock domain still has **+3.484 ns** of slack at
40 MHz — a 21.5 ns critical path, i.e. room to roughly 46 MHz — so the 31.25 MHz that
`board/rocket-freq` hands `gem` configs is very conservative.

## Area budget on U280

Measured post-place, with `rocket64b2` (2 cores, no Gemmini) at **10.12 % LUT** as the baseline:
INT8 16×16 costs **≈14.2 %p**, FP32 8×8 costs **≈15.5 %p**. FP32 8×8 costs *more* LUT than
INT8 16×16 despite a quarter of the PEs — float MACs are LUT-hungry.

The real ceiling is CLB, not LUT: across builds CLB/LUT is consistently **1.44–1.49**. So two
cores each carrying both accelerators would be ~69.5 % LUT ≈ **101 % CLB — unplaceable**. One
core with both, plus a bare second core, lands at ~39.8 % LUT (CLB ≈58 %), the same as the
existing heterogeneous bitstream.

Upstream's `DualGemminiConfig` is not usable as-is: its FP side is `BF16DefaultConfig` (BF16 is
broken on this pinned Gemmini), and its `use_shared_ext_mem` wiring has `require()`s tuned to the
INT8-8-bit + BF16-16-bit pairing that FP32 violates. Sharing mainly saves BRAM, which is not the
constraint.

## INT8 vs FP32: the ratio depends on the regime, not the PE count

Measured on `Rocket64b2gembl`, where both accelerators sit on core 0 — so core, clock, cache and
code are all identical and no clock correction is needed:

| Path | INT8:FP32 | Why |
|---|---|---|
| Naive tiling, 64³ | **6.80×** | bandwidth-bound; the 1 B vs 4 B element width dominates (bytes moved differ 7.6×, time 6.8×) |
| `tiled_matmul_auto`, 64³ | **2.93×** | 64³ underfills a 16×16 mesh — FP32 8×8 hits 62.2 % utilization vs INT8's 45.7 % |
| BERT layer (E120, clock-corrected) | **~4.0×** | large K fills both meshes |

A size sweep on the same core pins this down — the ratio is a function of matrix size, and the
efficiency ordering *inverts* between N=64 and N=128:

Early conv layers behave like the small-matrix end of that curve: ResNet's first layer has only
3 input channels, so after im2col the effective K is tiny and the 16×16 mesh barely fills. INT8
wins by just **2.1–2.4×** there (conv 224→112: 21,908 vs 52,700 ticks; with pooling: 29,840 vs
62,997), about half the PE ratio. Judging a CNN by one mesh size hides that.

| N | INT8:FP32 | INT8 util | FP32 util |
|---|---|---|---|
| 32 | **1.83×** | 13.9 % | **30.7 %** |
| 64 | 2.72× | 40.3 % | **59.5 %** |
| 128 | **4.47×** | **78.5 %** | 70.2 % |
| 192 | 4.44× | **89.6 %** | 80.6 % |
| 256 | 4.30× | 86.7 % | 80.7 % |

Below the crossover the *smaller* mesh wins outright: at N=32 an 8×8 mesh is more than twice as
efficient as a 16×16, which only has 2×2 tiles to work with. Above it, INT8 pulls ahead of the 4×
PE ratio because its utilization exceeds FP32's. So estimating a datatype's benefit from PE count
overestimates it by 2× on small matrices and slightly underestimates it on large ones. Which
accelerator wins depends on matrix shape — the practical argument for picking one at runtime.

FP32 8×8 plateaus at 80.7 %, reproducing the 81 % ceiling measured earlier on an entirely
different bitstream, opcode and benchmark — so that ceiling is real, not a measurement artifact.

Absolute numbers need the same care: the runtime library's own tiling is **3.26× slower** than
`tiled_matmul_auto` on identical hardware (0.246 ms vs 0.076 ms at 64³, both correct), because it
re-mvins both operand tiles per step and issues two `config_ld`s per tile while the library reuses
B and drives the loop FSM. Use `gemmini_rt.h` for control (same binary across bitstreams) and the
stock library for absolute performance.

## Cost model for multi-accelerator scheduling

`experiments/model/` holds a cost model that predicts what happens when several accelerators
share one machine. It exists because the decisions that matter — block shape, traversal order,
how many accelerators to build — cannot be made from a single-accelerator profile (see the block
shape and traversal sections above), and the model supplies exactly that missing information.

    T = ( max( max_a(t_a), T_issue )³ + T_onchip³ + T_dram³ )^(1/3)

- `T_compute` — for N ≥ 192 just use the roofline `N³/(dim²·f)`; **no profiling needed**, and it
  is *more* accurate than feeding in a measured single-accelerator time (7.0 % vs 8.5 % mean
  error), because a measured time carries per-accelerator inefficiency that disappears under
  sharing. Below 192³ the ratio to roofline climbs (1.3 at 128³, 3.0 at 64³) and you must measure.
- `T_issue` — `(commands on the busiest core) × c_issue`. Count only **work-bearing** commands
  (`mvin`/`mvout`/`preload`/`compute`); `config_*` is free. That is 4 per tile-op for a naive
  stepper, 3 for the reuse stepper, and 6 per *block* for `loop_ws` (i.e. negligible). `c_issue`
  depends on the **command mix**, not just the count: a stepper with one `mvin` per tile-op measures
  **13.4** cycles per work-command, one with two measures **~16**. Splitting that into per-command-type
  costs is **not measurable on this hardware** — every way to add issue pressure is blocked. Extra
  `config_*` does not consume issue bandwidth at all; repeated `preload` hangs the machine; extra
  `mvin` saturates on-chip bandwidth (14 B/cycle at two accelerators) before enough points accumulate,
  so the regression mixes the two terms and gives `mvin` ≈ 8 / `other` ≈ 14 where a two-equation solve
  gave 30 / 4. Use the per-family numbers and do not try to decompose them. Neither measurement *size* order nor *stepper* order affects results (0.2–0.4 %
  across orderings), so use whichever is convenient; just measure `c_issue` per stepper family.

**Each L2-missing strided run costs ~5.3 cycles regardless of its length** (measured at 16, 32 and
64 bytes; 5.2/5.2/5.3), so effective bandwidth is simply `run_bytes / 5.3` once the data spills — 3.1
B/cycle for 16-byte runs, 12.2 for 64-byte, flat from 2 MB out to 24 MB. Inside L2 the cost is
`run_bytes/16 + 1` cycles instead, ceiling ~15.8 B/cycle. In a matmul the run length is `J*16`, so
**the block shape sets the effective bandwidth directly**: `(8,2)` gets 6.0 B/cycle and `(4,4)` gets
12.1. That is a second reason to prefer large `J`, on top of minimizing bytes — and it is consistent
with `(4,8)` winning at 128³ on three accelerators.

**An `mvin` costs the same no matter how wide it is — always load `MAX_BLOCK_LEN` tiles at once.**
**(Scope: only while the path is command-bound. Measured on one accelerator that is already
bandwidth-bound at width 1, the rule reverses — E678: same bytes, width 1 is 8,245 cyc against
width 4's 10,740, i.e. **width 4 is 23 % slower** when the source is L2-resident, and a tie when
cold. Per-command cost there is not flat but nearly proportional to bytes, 32.2 -> 167.8 cyc.
E604's "wide mvin cost 2.5 % more" was the same effect. Check which floor you are on first.)**
Measured on a pure-DMA loop with total bytes held fixed: one-tile `mvin`s reach 3.21 B/cycle across
three accelerators, four-tile `mvin`s reach **13.33** — a 4.2× difference — and the per-command cost
stays at ~76 cycles regardless of width (79.7/73.9/75.9/76.8 for 1/2/3/4 tiles). `MAX_BLOCK_LEN` is
`MAX_BYTES/DIM` = 4 here. This is a large part of why hand-written steppers sit at 12–22 %
utilization while `loop_ws` reaches 64 % — the steppers in `gemmini_rt.h` load B one tile at a time.
**But widening them only helps a single accelerator.** A four-tile-wide variant runs 1.29–1.36×
faster at m=1 (utilization 21 % → 28 %) and **0.86–0.90×, i.e. slower, at m=2 and m=3**: this is *not*
double-buffer granularity (quadrupling the buffers changes nothing — 0.86–0.88× either way, while the
m=1 gain survives, so the variant works), and the remaining untested suspicion is bus arbitration
granularity: a four-tile request holds the shared bus for 1024 B instead of 256 B, so the other
accelerators interleave more coarsely. Keep the one-tile stepper for
multi-accelerator work. This is the third case in this project where an optimization measured on one
accelerator reverses under sharing — and the first where the *sign* flips rather than the magnitude.
(A three-tile width is anomalously bad at one accelerator; just use four.) The ~76 cycles does not
contradict the 8–30 cycle figures inferred from compute loops: there, most of the `mvin` occupancy
hides behind compute, and how much hides is workload-dependent — which is exactly why the
per-command-type decomposition is not identifiable.

**Never issue `preload` twice in a row without a `compute` between them — the accelerator hangs.**
Repeating a preload of the same scratchpad address looks harmless (it moves no data and leaves the
result correct) and is the obvious way to add issue pressure without adding bytes, but at three
repetitions the machine went unresponsive to both ping and ssh and needed re-programming. The mesh
pipeline assumes preload/compute alternation. `GRT_DEFINE_STEP_RP` in `gemmini_rt.h` is marked
REP=1 only. There is consequently **no way on this hardware to add issue load without also adding
bandwidth load**, which is why the stepper-dependence of `c_issue` is left unresolved.
- `T_onchip` — `total bytes / B_sbus`, with `B_sbus` = 15.0 B/cycle at 128-bit, 24.0 at 256-bit.
- `T_dram` — **misnamed: this is not a memory term.** Traffic counting is correct (verified against
  the FSM's load structure), but the divisor is not memory bandwidth — pure-DMA benchmarks reach
  11–12 B/cycle on the same working sets, access widths, stream counts and read/write mixes where
  matmul manages 7.07, and a *single* accelerator runs FFN2 at k=1.05 (near-perfect) while three run
  at k=2.58 each. The degradation comes entirely from sharing, i.e. from `loop_ws` instances blocking
  each other's load/compute dependency chains. Read the term as **"effective throughput of N
  concurrent FSM instances"**: 23.6 B/cycle for square matmuls, 14.6 for BERT projections, 7.07 for
  FFN2. To port the model, benchmark *that*, not the memory system — sweep K at fixed M,N and read
  off the curve. Measured here it follows **`B_eff = 16.9 * ws^-0.364`** (ws in MB, three
  accelerators), i.e. **doubling the working set costs about 22 % of the effective rate**, with the
  sharing slowdown running from 1.06× on a small square matmul to 2.50× on FFN2. **That effective
  rate is the same for two and three accelerators** (13.5 vs 14.9, 13.1 vs 13.2, 10.4 vs 10.1, 7.4
  vs 7.6 across a K sweep) — it is a fixed shared throughput, and the larger slowdown at m=3 is just
  more units dividing the same pie. That is why "useful accelerator count = 15.0 / per-accelerator
  demand" works at all, and it means **you can find the useful count for a new workload by measuring
  at m=2 only**. That curve cannot replace the two memory terms, though — it was fitted at m=3, and
  substituting it as a single term triples the model's error (18 % vs 7.6 %) because the existing
  `m*bytes/B_sbus` form carries the accelerator-count dependence implicitly. The curve below is what was
  measured for it — working set past L2, divided by `B_dram(ws)`: 15.1 B/cycle at
  576 KB, 10.3 at 1.1 MB, 6.8 at 1.7 MB, 4.4 at 2.3 MB, 2.85 at 4.6 MB. Despite the name this is
  **not DRAM bandwidth** — it is the effective throughput of *narrow-run* access, measured with
  16-byte runs. More precisely, the model's byte accounting is correct (verified against the FSM's
  load structure: FFN2 moves 7.18 MB per matmul, including re-reading the same 393 KB of A twelve
  times, and `gen_bytes` matches), and what is empirical is the **bandwidth** — square matmuls achieve
  24 B/cycle on three accelerators while BERT FFN2 achieves **7.07**, a 3.4× gap that run length does
  not explain (FFN2's A runs are 3072 B, longer than square's 192 B) and that pure-DMA sweeps do not
  reproduce. Porting the model to other hardware means re-measuring those two numbers; the structure
  carries over. Re-measuring with 64-byte runs gives 11.9–12.0 B/cycle flat all the way out to 24 MB,
  four times higher and nearly insensitive to working set. Keep the narrow curve (matmul reads B with
  the matrix row stride, so its runs are `J*16` bytes — 64 B for a `(4,4)` block, 32 B for `(8,2)`),
  but expect it to shift with `J`, and do not read it as a property of the memory system.

**When measuring a bandwidth, measure it with at least two access shapes.** If the two disagree, the
number describes the access, not the medium. This project read an access-limited number as a hardware
ceiling twice: a pure-DMA loop that "confirmed" a 15 B/cycle bus limit but was actually
request-rate-bound (it did not move when the bus was widened), and a working-set curve that looked
like DRAM falling off a cliff but was 16-byte-run throughput (64-byte runs stay flat at 12 B/cycle
out to 24 MB).

**Before adding a free parameter, turn every existing term of the model back on and re-read the
residual.** This project got that wrong twice: a `(1 + 0.085(m−1))` contention factor that turned
out to be patching `max()`'s optimism at a resource crossover, and an accelerator-combination
exponent of 4.5 that turned out to be standing in for an omitted issue term. Both fit their data
to ~5 % and both were physics-free.

The cube (`p=3`) combination replaces an earlier `(1 + 0.085(m−1))` contention factor. That factor
was a patch over `max()` being optimistic when two resources saturate together — back-solving it
gave 0.037 in one case and 0.183 in another, and the large one sat exactly at a compute/on-chip
crossover. Plain `max()` (p→∞) is the *worst* choice tested, at 12.8 % out-of-sample.

Note which combinations are `max` and which are the cube: accelerators are genuinely parallel, and
issue pipelines against compute by design, so both take `max`; only the bandwidth terms truly
contend and take the p-norm. **Accuracy**: **7.6 % mean over 73 points** spanning five datasets,
three bitstreams, two bus widths, symmetric/asymmetric/unbalanced work, and a pre-registered
prediction for hardware that did not exist yet (5.6 % on those ten points); 70 of 73 within 20 %.
One free parameter (`p`);
every other constant is independently measured, and `B_sbus` was validated by predicting that
widening the bus would raise the ceiling before the bitstream existed.

**a(K) is two terms, and only one of them is the bus.** Comparing the (8,4) curves at both widths:
widening the bus removes **82 %** of a(K) at K=512 and 58 % at K=1024, but only 5 % at K=2048 and
2 % at K=3072; b(N) drops just 11–27 %. So the small-K part of a(K) is on-chip bus and can be
designed away, while the large-K part and b(N) cannot — and the large-K part is now the only term in
the model with neither a mechanism nor a fix. Ruled out for it so far: L2 live footprint, B re-fetch
count, on-chip bus, load imbalance, block live set. Refitted on the recommended hardware the model
holds out at **3.3 %** (m=1 at 0.69 %), with p=2.0 at m=3 and p=1.0 at m=2 — the p-norm now confirmed
on three independent block/bus combinations. Widening the bus does **not** revive the third
accelerator: at K≥1536 it is still worth 0.2–0.8 %, so bus width and accelerator count are separate
bottlenecks.

**`a` is a function of the K-split chunk count, not of K.** Pooling 74 verified points on the
recommended configuration gives `a(chunks)` = 0.14 (2), 0.27 (4), 0.93 (6), 1.20 (8), 1.58 (12),
1.76 (16) — and the eleven points at 8 chunks span K=512 to 4096, an 8× range, within ±12 %. The
**3.4× jump between 4 and 6 chunks** is what the "keep chunks at 4 or fewer" advice actually is: a
cliff in the curve, not a rule. With this the model predicts the times the tuning rule produces
(4.9 % mean on four tuned stages), which the earlier `Kc=16` fit could not:

    T(m=3) = M*K*N/(dim^2*f)/3 * [ 1 + ( a(chunks)^2 + b(N)^2 )^(1/2) ]
    b(N) = 0.11 (768), 0.03 (1152), 0.00 (2304), 0.22 (3072), 0.54 (4608), 0.51 (6144)

Above six chunks that curve is **block-independent too** — (8,4) and (4,8) coincide and (4,4) runs
9 % higher, matching their 6.0 vs 8.0 intensity — and K is invisible there **only up to K≈3072**.
Sorted, the (4,4) eight-chunk values read 1.19, 1.25, 1.30, 1.32, 1.35, 1.38 for K ≤ 3072 and then
1.61 at K=4096 — using the mean cost a pre-registered prediction 17 % on a K=6144 stage. Pushing K
to 8192 shows it is a **step, not a trend**: one rise between 3072 and 4096 (+13 % at eight chunks,
+6 % at sixteen), then flat within 2 % out to K=8192. Filling that in takes the same layer
prediction from −11.2 % to −3.9 %. Likewise the contention for
one-matmul-per-accelerator is not a constant 4 % — it is 1.04× at [1024×1024] and **1.17× at
[1536×1536]**. Below four chunks both block and K reappear: at K=2048 with four chunks, (8,4) gives
0.25 and (4,4) gives 0.63, a 2.5× gap for 1.33× the bytes. So keep block-and-K-resolved values only
for the low-chunk regime. With that filled in, the model predicts all six tuned stages of both BERT
models to **4.4 % mean, 7.7 % worst**, and the layer totals to +4.3 % and +1.0 % — but two pre-registered
tests on unseen workloads both failed: hidden=1536/FFN=6144 at **−11.2 %** and hidden=2048/FFN=8192 at
**−15.3 %**, against a 10 % bar. Extrapolating each curve's shape from its last two points was part of it, but the deeper cause is
that **the separable form itself breaks down at large K and N**. Measuring b out to N=12288 shows the
back-solved b depends on K — at N=8192 it is 0.33, 0.45 and 0.96 for K = 512, 1024 and 2048 — which
it cannot if b is a function of N alone. Scoring the model against all 186 measured points shows the boundary is not size at all — mean error
is 5–13 % at every max(K,N) from 768 to 12288 — but **chunk count**: at 6 or more chunks the model
predicts to **3.5 % mean, 11.5 % worst** across 73 points spanning K=512–8192; at 4 or fewer it is
12–21 % mean and up to 43 %. There the `a` term is small and the residual dominates, and that
residual resists modelling: across 45 points the best single predictor is per-accelerator working
set (log-correlation 0.82, but 3.2× spread at equal working set), and a two-exponent power law in K
and N gives R² = 0.43–0.66 with 35 % mean residual. **That regime is a measurement table, not a
model** — a negative result from four candidate variables and a regression, not an assumption. It splits by block too: (8,4) 5.1 % against (4,6) 14.4 %.
**The tuning rule prefers 2–4 chunks, so the model is weakest exactly where the compiler works** —
in that regime it is a (block, K, N) lookup table rather than a curve, which is why the tuned-stage
4.4 % needed a K-and-block-resolved table.

**The tuning rule generalizes; the model does not.** Across three unseen workloads the rule picked
the fastest of six schedules every time, while the model missed the layer time twice. Knowing which
setting wins needs only the right ordering; knowing how long it takes needs the curve's absolute
values, and those had to be re-measured at every new extreme of K and N. **Use the rule to choose and
then measure the time**; use the model for hardware decisions and for shortlisting, and trust its
absolute times only at 6+ chunks. Inputs are the
roofline, the block and chunk count the compiler picks, and N.

**How that was found:** Sweeping `Kc` at
fixed K shows η3 tracks `chunks = Ktil/Kc` far more than K: chunk count 8 measures 2.098 at K=1024
and 2.150 at K=2048, and raising `Kc` at fixed K cuts the term by more than half. (Chunks are not the
*only* variable — below four chunks a K-dependent floor emerges — but they dominate everywhere else.) Chunks accumulate serially
into the same accumulator, so the chain blocks other accelerators — which is why the term never
appears at m=1 and is untouched by bus width, L2, or re-fetch count. Every candidate ruled out
earlier was about *moving bytes*; the cause is a dependency chain. **Tune `(block, Kc)` per stage — one adaptive rule hits the optimum exactly.** Across ten shapes from
three sweeps and two models (BERT-base and BERT-large, the latter not used to develop any of this):

    block  : among blocks that can reach **4 chunks or fewer** (some legal Kc with
             Ktil/Kc <= 4 and Kc*(I+J) <= 512), take the one minimising intensity
             16(1/I+1/J); break ties toward larger I.  No undefined interval, and
             scored over 31 shapes: **0.56 % mean / 5.5 % worst**, against 1.45 % / 13.5 %
             for "must hit the chunk target exactly" and 2.25 % / 28.0 % for the older
             "(8,4) when N/K >= 0.5, measure both when K/N >= 4" (E316/E317).
             Then: if a block was rejected for chunks but has **25 % lower intensity**,
             measure it too and keep the faster — that costs a second run on 5 of 36 shapes
             and takes the rule to **0.38 % mean / 5.5 % worst** (E318).
             The target itself only sets Kc, not the block: **missing the target is
             asymmetric.** Falling from 2 chunks to 4 costs nothing (measured −4.5 % to
             +1.8 %), so a lower-intensity block wins there even though it misses; falling
             from 4 to 8 crosses E282's cliff and costs 13 %. Both refinements came from
             pre-registered out-of-sample tests that the previous version failed: E316 lost
             12–19 % on three held-out shapes, E317 lost 6 % on one (E317/E318).
    chunks : 4 when K*N >= threshold, else 2.   (K*N is B's byte count.)
             **The threshold belongs to the block**: 1.4 MB at (8,4) with standard on-chip
             memory (E302), **~2.4 MB at (8,8) with doubled memory** (E406/E408). Using the
             old value on the new block costs 2.3 % mean / 14.7 % worst over fifteen shapes;
             the new one is **0.00 % on all fifteen with a single measurement**. What is
             actually measured is the *interval* **(2.25, 2.375] MiB**, pinned by holding Ktil=64
             and walking N alone across it (E408, halved by E423 with 10 relaunches) — every
             value inside scores identically, so the constant is a representative, not a fit.
             The crossover is single and monotone.
             On ties (several Kc giving the same chunk count) take the **smaller** Kc at
             target 2 and the larger at target 4 (E405).
             `K*N` is the variable, not `Ktil` and not the aspect ratio: two shapes with
             `Ktil=192` split opposite ways along `K*N`, while three shapes at fixed `K*N`
             and aspect 3 / 1 / 1/3 all agreed (E406).
             **Do not use "largest legal Kc at target 4"** — that clause (E398/E400) was
             derived from two shapes, held on both, and lost 5.2 % mean / 16.2 % worst once
             nine were measured. Only tall (K/N >= 4) shapes want it. Retracted E403–E406.
    (block clause verified out-of-sample across K = 768-6144: on eight unseen shapes at
     K <= 1280, (8,4) beat (4,8) beat (4,4) in that exact order, 8/8 in every one of six
     runs — E304/E305. Measured properly the complete rule lands on the swept optimum in
     **8 of 8**, mean 0.15 %, worst 1.2 %. The transpose clause is worth **nothing** in this
     size range — (8,4) wins outright everywhere — and measuring it once is actively risky,
     since one run picks the slow mode 21 % of the time.)

    Kc     : Ktil / chunks; if Kc*(I+J) > 512, take the legal Kc whose **chunk count
             is closest to that target** — neither double the chunk count (skips legal
             intermediate values, up to 18 %) nor take the largest legal Kc (E299's
             fix, which is worse than what it replaced: 2.8 % mean vs 1.1 %).

**The block ranking is order-preserving under Kc.** Giving each block its own swept-optimal Kc
instead of the rule's leaves the ranking identical on three shapes and all four positions
(square, wide and tall); the rule's Kc *is* the block's own optimum in 8 of 12 cells and costs
2.8–10.0 % in the other four, always at a block already ranked third or fourth. So a table
measured at rule-Kc is tilted in **magnitude** but not in **order**: (8,8) still wins, by
**9–22 % over the runner-up** rather than the 45 % a tilted table suggests (E411). The earlier
case where own-best-Kc collapsed a ranking into a three-way tie had blocks at intensity
5.3/6.0/8.0; here (8,8) sits alone at 4.0 against 6.0.

**Pick the block first (minimum intensity), then Kc — the order is verified.** A larger block
caps Kc (`Kc*(I+J) <= sp_limit`), so a smaller block can reach a bigger Kc: (4,4) reaches
Kc=128 where (8,8) stops at 64. That trade — twice the bytes for half the chunks — **loses
badly**: on the two tall FFN2 shapes (8,8) at Kc=64 beats (4,4) at Kc=128 by **27 % and 38 %**,
and the whole ranking follows arithmetic intensity exactly (4.0 < 6.0 < 8.0). Within (4,4),
doubling Kc is worth only 5 %. So the large Kc effects below hold **inside a fixed block** and
do not transfer across blocks; no tall-shape exception is needed (E409). Two side notes: (4,8)
beats (8,4) by 4–7 % at equal intensity on tall shapes (the known I↔J asymmetry, J-large wins
here), and the block spread is 7.7 % at m=1 against 34.5 % at m=3.

**Chunk count, like block shape, only matters when accelerators share the bus.** Correcting the
backoff on five affected stages moved a *single* accelerator by −0.0 to +0.9 % (i.e. nothing) and
three accelerators by **1.0–10.4 %** (E301). Same binary, same shapes, only the schedule differs.
That is the third axis showing this pattern — block shape (E271), `mvin` width (E205, where the
sign flips), and now chunk count — and the common cause is total bytes: one accelerator has
bandwidth to spare and does not count them. **A parameter measured as "no difference" on a single
accelerator is untested, not rejected.**

Scored over 50 (block, shape) cells where three or more chunk counts were measured, the
optimum is chunks 2 or 4; chunks 6 wins only where 4 is illegal, and 3 and 8 never win. So the
target is always 2 or 4 and the backoff is a nearest-legal search, not a direction. Mean cost
over the per-cell optimum: **0.14 %** (max 1.9 %, nothing over 3 %) for the rule above, against
0.43 % for a `K >= 1536` threshold, 1.67 % for E278's `N/K <= 2` test, 1.13 % with doubling
backoff and 2.76 % with "largest legal Kc".

**The variable is K·N, not K** — and that came from a pre-registered test that confirmed the
hypothesis in its treatment group and refuted it in its control (E302). Sorted by K·N the cells
split cleanly: chunks 2 wins below 0.6 MB (7–0), the band 1.0–1.2 MB is a tie, chunks 4 wins
above 1.6 MB (12–0), and it splits at the same place for every block shape — *away from the
threshold*. (An apparent counter-example at [896×1024], where `(8,4)` and `(4,8)` seemed
to want opposite chunk counts, was measurement bimodality and is retracted — E305.) On (8,4)
specifically, measured with six process relaunches, the crossover sits between 1.33 MB (tie) and
1.64 MB (chunks 4 by 5.6 %), matching the threshold. The threshold sweep
is flat from 1.2 to 1.6 MB, so 1.4 is a band midpoint, not a fitted constant.
**That crossover moves with the block**: re-measured at (8,8) on the doubled-memory board it
sits in (2.25, 2.375] MiB, i.e. about 65 % further out (E406/E408/E423). The *structure* — a K·N threshold
between chunks 2 and 4, independent of aspect ratio — carried over unchanged across block and
board; only the constant moved. Twelve shapes were used, built as a 3×4 grid of three aspect
ratios (N/K = 4, 1, 1/4 plus a 3 / 1 / 1/3 set) at four K·N values, so the aspect axis is
controlled rather than assumed. Note the grid was *designed* with matched K·N, which makes
"K·N separates them" partly circular; the follow-up at the geometric midpoint (E406) is what
breaks the tie, because there `Ktil` and `K·N` predict opposite answers and `K·N` wins. The `N/K` test
becomes unnecessary — scoring `K*N` alone is identical to scoring it with `N/K` kept, because
every high-`N/K` cell also has a large `K*N`. K·N is B's size in bytes. Its threshold divided by
three accelerators is 467 KB against a 512 KB L2, which looked like a mechanism — but that was
**tested and refuted** (E303): if the cause were per-accelerator B against L2, the threshold
would scale with m and sit at 0.93 MB for m=2; measured, chunk 4 only starts winning at 1.64 MB
at m=2, i.e. the threshold moves the *opposite* way. What is real is that the chunk effect is
about **3x larger when accelerators share** — mean |chunks 2 − chunks 4| is 0.85 % at m=1 against
2.3–2.6 % at m=2 and m=3 — which is the quantitative form of E301. (An earlier reading had it
rising monotonically through m=3; that was measurement bimodality, which itself scales with m.
See E305.) **A number landing near a cache size is not
evidence**; that is the second L2 explanation refuted here (see also E243→E244). No m-specific
rule is needed: applying the m=3 threshold at m=2 costs 0.40 % mean, 3.5 % worst. The threshold
holds on 58 cells (0.22 % mean, 3.9 % max, one cell over 3 %).

That rule is exact on all eight shapes it was derived from, and on a pre-registered out-of-sample
test of six new shapes it is **exact on five and 3.3 % off on one** (mean 0.55 %). Fixing chunks at 4
instead costs up to **16.6 %** (worst at N/K ≤ 1) and fixing at 2 costs up to 9.3 % (at N/K = 4), so
the N/K test matters. The single miss is (8,4) losing to (4,8) at identical intensity, chunk count
and scratchpad use — the I↔J asymmetry. **That is no longer a reason to measure twice**: orient
the block by `N >= K` (larger I) vs `N < K` (larger J), which is 33/33 (E412, see above). The
older advice was "also measure the transpose and keep the faster one", which with one extra
measurement landed on the optimum in all fourteen shapes tested; the rule now gets there with none. Note the speedup falls with
model size — 2.24× on BERT-base, **1.93× on BERT-large** (69.0 ms/layer measured as an assembled
layer, 24 layers ≈ 1.66 s) — because a(K) and b(N) both grow; extrapolating the small model's number
would overstate the large one by 16 %. Per-stage times compose into the layer to 1–2 % on both models.

**When a stage is several independent matmuls sharing one activation — QKV, multi-head, gate+up —
choose the allocation by load imbalance, not by a divisibility test** (E315). For `k` independent
matmuls, `nj` J-blocks each and `m` accelerators:

    imbalance(one-each) = m*ceil(k/m)/k        imbalance(J-split) = m*ceil(nj/m)/nj

Take the smaller; **when both are 1.0 the two schedules move identical bytes and tie exactly**
(measured −0.13 % for 2 matmuls on 2 accelerators). This replaces the earlier "one each when the
count is a multiple of m" heuristic, which got the right answer for the wrong reason: the 16 %
win at 3-on-3 was J-split *imbalance* (nj=16 splits 6/5/5), not a saved split cost. The formula
gets the direction right on all four measured cases (2-on-2 tie, 3-on-3 one-each by 16 %, 3-on-2
J-split by 21 %, 2-on-3 J-split by 32 %) but over- or under-states the size by up to 15 points, so
use it to order the options and then measure. It also covers a case the old rule could not speak
to at all — 2 matmuls on 3 accelerators, worth 32 %.

The original observation, for reference:
**give one to each accelerator instead of J-splitting each, but only if their count is a multiple of m.** With
three projections on three accelerators that pays no split loss at all, only contention: 11.4 ms on
BERT-large against 12.5–13.6 ms for three sequential J-splits, a 16 % win (3.4 % on BERT-base). With
three projections on *two* accelerators it reverses and loses 21–23 %, because the second round runs
one projection alone — J-split instead. An earlier measurement said the opposite, but its
"independent" case gave each accelerator its own copy of *both* operands — 3× the unique traffic and
1.76× contention, against 1.04× when the activation is shared. Synthetic independence is more
pessimistic than the real structure.

**There is no single (block, Kc) rule across shapes.** On the three BERT stage shapes the
optimum differs in *direction*: [768×768] wants two chunks ((8,4) Kc=32), [768×3072] wants four
((8,4) Kc=12, where two chunks costs 19 %), and [3072×768] wants a different block entirely
((4,4) Kc=48, beating both (8,4) and (4,6)) — that last one is **not an exception**: (8,4) cannot
reach the chunk target there (Kc=48 needs 576 tiles against a 512 budget) while (4,4) can, which
is exactly what the block rule above selects (E316). The same mechanism decides Llama's down
projection [2816×1024], where (4,4) wins by **25 %** despite the *worst* arithmetic intensity of
the three candidates — hitting the chunk target beats minimising bytes. Sweeping nine (block, Kc) combinations per stage takes
about a minute and is worth 7 % on the layer — widening that to twenty finds only 0.5 % more, so nine
is enough. Do not chase stage-level gains below about 5 %: a refinement worth 3.9 % on one stage
(making `Kc` divide `Ktil` evenly) reproduced at stage level but **reversed by 0.7 % in the layer**,
because the even-chunk variant's stage time scatters 13 % run-to-run against 0.1 % for the uneven
one. Per-stage tuning gives per-stage tuning runs a BERT layer in **33.5 ms,
2.24×** versus 35.9 ms for the best uniform choice, and the per-stage times compose into the layer
to 2.3 %. Every attempt here to compress this into a rule turned out to be conditional on whatever
shape it was measured at — "block ranking is K-invariant" was Kc-conditional, "maximize Kc" was
wrong below four chunks, and "target four chunks" was N-conditional. Use the model to shortlist,
then measure.

**Within one shape, `Kc` still matters most.** The scratchpad bound is
`Kc·(I+J) ≤ 512` tiles (512 passes, 576 fails), which caps (8,4) at Kc=32 and lets (4,6) reach
Kc=48. Getting there is worth **22–27 %** at K=3072 on every block — far more than the block choice,
which spans only 3.4 % once each block runs at its own best Kc. Going *below* four chunks loses
again (6.2 % at K=2048, 7.4 % at K=3072), because a larger `Kc` grows the per-block scratchpad
footprint. And the chunk term is not the whole story: at four chunks or fewer a **K-dependent floor**
appears (0.28 / 0.62 / 1.00 at K=1024 / 2048 / 3072), which is the same bus-independent term E268
isolated and is still unexplained. So: subject to `I·J ≤ 32`, `I ≥ 4`, `J ≥ 4` and the scratchpad
bound, hit four chunks and break ties on intensity `16(1/I+1/J)` — at K=3072 that is `(4,6)+Kc=48`. This also puts a condition on the
earlier block ranking — (8,4) leads at Kc=16, but at each block's own best Kc the order is
(4,6) 22.77, (8,4) 23.52, (4,4) 23.55 ms, a three-way tie. **Verify correctness when raising Kc** — and note that
**the only reliable detector is the correctness check itself**. Measured over 63 cells (E356),
over-budget configurations are *slower*, not faster (27/27, +8% to +40%), and η₁ does not
separate them: passing cells span 1.04–1.19 and failing cells 1.05–1.46, with 21 of 39 failures
inside the passing range. An earlier note here claiming inflated η₁ as a tell is retracted.
The two bounds are independent and both were confirmed 63/63: `Kc·(I+J) <= 512` tiles and
`I·J <= 32` — `(8,8)` at `Kc=32` uses only 512 tiles yet fails, on the accumulator rule alone.

**On the recommended configuration — one core, 256-bit bus, three INT8 accelerators, block (8,4),
Kc=32 — a BERT layer runs 35.9 ms, 2.10×** (12 layers ≈ 0.43 s at seq=128), verified with the
bitstream signature stamped before measuring. The whole gain over Kc=16 comes from FFN2, the only
stage whose chunk count moves (12→6); the K=768 stages go 3→2 chunks and barely change. With Kc=16
the same layer runs 39.9 ms, 1.89×.
A prediction registered in advance from the 128-bit numbers times the bus ratio, with nothing
refitted, landed at 39.8 ms (0.2 % off). One core costs nothing on the real workload either (75.484
vs 75.392 ms at m=1). Use all three accelerators: forcing FFN onto two costs 8.6 %, all of it from
FFN1 (K=768, where the third pays 19 %), while FFN2 (K=3072) is a 1.5 % tie — exactly the K≤1536
rule. The trajectory on fixed silicon was 73.0 → 53.3 → 43.8 → 39.9 → 35.9 → 33.5 ms (12 layers ≈ 0.40 s).

**On the older 128-bit two-core board at block (8,4), three accelerators give 1.72×** (layer time
43.8 ms) — changing only the block from (4,6) cuts 17 % while leaving m=1 unchanged, with the gain
concentrated in FFN1 (−36 %), the stage with the largest N. **That also reverses the scheduling
answer**: on (4,6), FFN1 ran 3.9 % faster on two accelerators than three, and on (8,4) it runs 9.3 %
faster on *three* — so "the best schedule leaves an accelerator idle" is retracted, and how many
accelerators to use depends on the block shape. Fix an axis only at that axis's optimum.

**On the older (4,6) block, three accelerators give 1.43×, and the schedule decides
almost all of it.** Same bitstream and same work, issuing whole per-accelerator ranges instead of
interleaving at block granularity collapses that to **1.03×** — a 43 % gap from ordering alone.
(An earlier board state measured 1.54× with per-stage tuning; see the reproducibility note below.
Re-running the identical binary after the state change landed at 1.43×, within 0.4 % of what the
state-change characterization predicted in advance.)
The per-stage speedups fall monotonically with per-accelerator working set (A+B+C bytes, with B and
C divided by m when a matmul is split along J): 0.33 MB → 2.26×, 0.79 MB → 1.99×, 1.02 MB → 1.41×,
1.21 MB → 1.18×. FFN1 and FFN2 are 62 % of the layer and have the largest working sets, which is
why the layer number lands so far below the 1.93–2.21× microbenchmarks. Budget accelerators per
stage by working set, not by counting matmuls.

**Splitting one matmul across all accelerators beats giving each its own** — the opposite of what
"independent work parallelizes best" suggests. Measured at three BERT shapes, J-split scales better
than independent copies everywhere (2.33× vs 1.84× at [768×768], 1.45× vs 1.32× at [768×3072]),
because a J-split shrinks each accelerator's working set (B and C divide by m) while independent
copies leave it fixed. So the working-set curve predicts it; there is no separate J-split penalty.

**And the best schedule leaves an accelerator idle for most of the layer.** Tuned per stage —
sequential J-splits for Q/K/V, three accelerators on the projections, **two** on FFN1 and FFN2 —
a BERT layer runs 1.54× versus 1.45× for using three everywhere. FFN1 is 7 % *slower* on three
accelerators than on two, and FFN2 is flat, so 77 % of the layer's time is best spent with one
accelerator idle. Deciding *how many* to split across matters as much as deciding how to split —
but **the cost model cannot make that call.** A pre-registered test on six unmeasured shapes
predicted m=3 optimal for all six; three of them measure m=2 as the winner, and the error grows
with m (1.9 % → 14.0 % → 17.6 %). The failure is structural: the shared-throughput terms are
m-independent (E235), so predicted time is monotone decreasing in m by construction, while a real
fixed problem can get *slower* on a third accelerator (+16.5 % on one shape). Load imbalance does
not explain it either — the two effects run in opposite directions across shapes. The loss is `1 + max(a(K,m), b(N,m), imbalance−1)` — a **max, not a sum**. An additive fit worked
on a grid where K and N grew together and was refuted by filling the corners: the increment from
N=576 to N=2304 is +0.68 at K=256 but **+0.00** at K=2048, because once K already binds, N is free.
The max form lands 8 of 12 corner points exactly (residual RMS 0.055) with the same parameter count,
and matches how every other term in this model combines. A pre-registered test on eight unmeasured
shapes confirmed it at **3.4 % mean error**, against +33–48 % for the alternatives. That conclusion was then **superseded**: comparing only the two extremes hid the answer. Filling the
N interval where `b` crosses `a` — the place the two forms disagree most — gives a gap that shrinks
(0.64→0.46) but does not close, which is neither max (→0) nor additive (→constant). Fitting all 37
points three ways, a **p-norm with p≈2** beats max by 4.5× and additive by 1.9× at m=3 (residual RMS
0.026 in η, about 1.2 % in time); m=2 wants p≈1–1.5. Use
`η − 1 = (a(K,m)^p + b(N,m)^p)^(1/p)`. This also makes the model internally consistent — E208 already
replaced a contention factor with a p-norm for the resource terms, for the same reason. Note
`b(576)=b(1152)=0`: N does nothing at all below about 1152.

**Load imbalance belongs inside that max, not multiplied onto it** — the sharpest single result
here. A shape with nj=4 splits 2/1/1 across three accelerators, so a multiplicative term predicts
1.5× (η=4.10); inside the max, K already binds and it predicts no cost at all (η=2.73). Measured:
**2.77**. The same shape at K=768, where compute binds, does pay 1.38 of the predicted 1.50. An
accelerator that finishes early hands its bandwidth back, so imbalance is free exactly when
bandwidth is the constraint.

Two scope limits found by pushing one axis 2× further: the `roofline × 1.06` rule for m=1 holds only
to N≈2300 (it reaches 1.23 at N=4608), and the N term does exist at m=2, just crossing over later.

**Widening the system bus to 256 bits is worth 17–21 % for K ≤ 1024 and nothing for K ≥ 2048.**
Measured at block (8,4) with both bitstreams verified in the same session. On BERT-base, whose K=768
stages are 52 % of a layer, that is 8–10 % for **+1.10 %p LUT** — a far better buy than a third
accelerator (5 % for +14.2 %p). An earlier claim that the bus changed nothing was withdrawn: it
compared two runs that were *both* on the 256-bit bitstream, because the bitstream loaded at session
start had never been verified. **Always stamp the bitstream signature before measuring** — square
192³ (4,4), three independent matmuls, m=3 takes 1.03 ms at 128-bit and 0.64 ms at 256-bit. A
long-running "board state that would not revert" was exactly this mistake: the slow state was the
128-bit bitstream, and across seven verified programmings the two clusters never overlap
(1.776–1.804 ms vs 2.260–2.292 ms on the same shape).

What the bus helps is set by **K**, not by M or by whether the work is split or replicated — a
2×2 test at K=512 finds all four cells 1.20–1.56× faster at m=3, while K=2048 and 3072 move 0–4 %.
The max form explains it quantitatively: a first-principles bus term (`bytes/B_sbus / (roofline/m)`)
is 1.35–1.36 at those large K while measured η is 2.39–2.73, so a'(K) dominates and halving the bus
term changes nothing. Do not use that bus term as-is, though — at K=256–768 it predicts 1.40–1.53
against a measured 1.20–1.22, because small blocks stay resident and the naive byte count
overstates what crosses the bus; treat it as an upper bound. Practically: the 256-bit bus
(+1.10 %p LUT) buys up to 1.56× at K≈512 and **nothing at K≥2048**. BERT has K values on both sides
— 768 for the projections and FFN1, 3072 for FFN2 — so it is worth roughly 8–10 % on a layer.
Choosing hardware from square L2-resident benchmarks gets this backwards. Fitted and validated entirely inside one reproducible board state (37 training points, held-out
shapes at unseen N, predictions pre-registered), the model predicts time to **2.0 %**, and `m=1`
needs no fitting at all — `roofline × 1.06` lands within 1.4–2.1 %. Extrapolating K 17 % past the
training range costs only 4 %.

**The curves do not transfer across block shape.** Fitted at (4,6), they mispredict other shapes by
−44 % to +57 % — a byte-proportional correction (free-parameter-less, since bytes are countable) gets
the direction right but not the size. So the model's scope is "M, K, N *at a fixed block*", and the
(m, I, J) co-optimization E246 demands still needs the curves re-measured per candidate block —
shortlist two or three with E191's intensity rule, then measure. Also: at I=2 even the `roofline ×
1.06` baseline fails (η reaches 1.51), which is a second reason for the existing "keep I ≥ 4" rule.

**Use (8,4), not (4,6).** (8,4) beats (4,6) at m=3 on every shape tested — by 2.9 %, 3.0 %, 8.6 % and
**23.2 %** — matching E244's ranking. Numbers measured at (4,6), including the BERT layer figure,
therefore have headroom.

**And at (8,4) the model collapses to a function of K alone.** Re-measuring the curves there, η
varies only 1.1–5.1 % as N triples, against up to 25.9 % at (4,6) — so `η ≈ 1 + a(K)` over
N=768–2304 and the N term effectively vanishes. That is *why* (8,4) wins: its advantage is largest
exactly where (4,6)'s N term bites hardest, so a block shape is worth choosing not because it moves
fewer bytes but because it removes the N dependence. Two structural results survive the block change
— M-independence (≤3.5 %) and `roofline × 1.06` (η₁ = 1.02–1.08) — while the a/b curves themselves do
not. Pushing N to 6144 brings b back and answers the p question: at (8,4) the best fit is **p≈2.5**
(RMS 0.0197) against max 0.0289 and additive 0.0421, matching the p≈2 found at (4,6). So the
**p-norm combination is block-independent structure**, alongside M-independence and `roofline × 1.06`;
only the a/b constants change with the block. (8,4) also pushes b(2304) from 0.70 to 0.20 and b(4608) from 1.94 to 0.61, after which b saturates
around N=4608. Those two ratios matching (0.29, 0.31) looked like a constant scale factor, but a
third block refutes it — on (4,4) the same two ratios are 0.97 and 0.63, and predicting four points
from two calibration points misses by 19.5 %. **Each block needs its own curves.** That is not a
practical problem, though: (8,4) wins on every shape against both (4,6) (2.9–23.2 %) and (4,4)
(5.6–28.4 %), and E244 showed the ranking does not move with K — so characterize (8,4) and skip the
block sweep. The practical consequence is large: at N=4608, K=512 three accelerators
give **1.80× on (8,4) and 1.02× on (4,6)** — measuring on the wrong block turns a useful third
accelerator into a useless one.

**M enters only through the roofline.** Curves fitted at M=128 predict M=256 and M=512 with no
refitting at all — η varies at most 3.3 % across a 4× range of M, and times land within **1.6 %**
(m=2,3) and **0.4 %** (m=1). So the split-efficiency loss is set by one block's shape (K, N), not by
how many blocks get traversed, and a change of sequence length needs no re-characterization.

Rocket-domain slack at 50 MHz across the built boards: **+0.358 ns** (1 core, 3 accel), **+0.655**
(2 cores, 3 accel), **+0.790** (1 core, 2 accel) — fewer accelerators leave more slack here. The
critical path is the `AccumulatorScale` `scale_func` chain (`norm_unit_passthru_q` ->
`acc_scale_unit/pipe_out_p`), 19.017 ns of data delay at 57 logic levels, and it is **62 % route,
38 % logic**, entirely inside SLR0 — so it is a placement-distance problem, not an SLR crossing
and not logic depth. The next clock the wizard can generate above 50 is **62.5 MHz** (the ladder
is 250/200/160/125/100/80/62.5/50/40/31.25/25/20 — there is no 55 MHz), which needs ~3 ns off
that path — and **that was done** (E314/E320): switching `AccumulatorScale` from its
combinational `num_scale_units == -1` branch to the shared-unit branch (`num_scale_units = 16`,
which needs `patches/gemmini-accscale-norm0.patch`) closes 62.5 MHz at WNS 0.000, 0 failing
endpoints, for **+0.59 %p LUT**. `Rocket64b1gem2i8w256su20f62` is 39.09 % LUT / 57.11 % CLB,
verified bit-exact on the mvout scale path (five scales, mixed saturation, 32768 values each),
and runs the same binaries at **0.791–0.796×** the 50 MHz time — i.e. the restructuring costs no
cycles. **Clock beats an accelerator**: this two-accelerator 62.5 MHz board beats the
three-accelerator 50 MHz board on BERT-base (−1.7 %), BERT-large (−9.9 %) and h1536 (−14.1 %)
while using **12.11 %p less LUT**. Per unit area the clock is worth more than 20× the third
accelerator. **When slack says a frequency is impossible, read the critical path's name instead
of its number** — "19.017 ns, 62 % route, `AccumulatorScale/pipe_out_p`" is an address, not an
estimate.

**The third accelerator's value falls monotonically with model size** — 21.8 % at hidden 768, 15.8 %
at 1024, 8.1 % at 1536, 4.2 % at 2048 (six process relaunches each, per-schedule minima) — for a
fixed **+12.70 %p LUT**. **Read those as lower bounds tied to the schedule they were measured on.**
Re-measured after the Kc rule was corrected (E406–E410), hidden 1536 gives **15.9 %** (confirmed
twice, on two binaries: 17.1 % in E407) and hidden 3072 gives **2.1 %** against the 0.2 % recorded
below — a better schedule makes the third accelerator worth more, not less, because the gain comes
from contention the schedule was creating. The per-LUT recommendation does not move (two
accelerators still lead by 17 % at hidden 1536), but do not quote these percentages without
naming the schedule. Both endpoints are now built and measured rather than estimated:
`Rocket64b1gem2i8w256f50` is **38.50 % LUT / 55.11 % CLB**, and the Rocket/Gemmini clock
domain closes at 50 MHz with **+0.790 ns** and 0 failing endpoints (read the Intra Clock Table,
not the design-level WNS — this bitstream has seven clocks and the design WNS of +0.072 ns
belongs to another one, E313), and the three-accelerator board is 51.20 % / 74.0 % (E308). The area model
predicted 37.0 % / 54 % before the build, i.e. within 4.1 %.

**Building the smaller board changed nothing measurable**: on four workloads the real
two-accelerator bitstream matches the three-accelerator board driven at m=2 to within **0.1 %**
(−0.10 to +0.07 % at m=2, −0.57 to +0.18 % at m=1) — so an idle accelerator costs area and
nothing else, and every "how many accelerators" judgement in this file, all of which compared m
on one board, is valid (E309). **Do not build a bitstream per accelerator count: build the
largest and sweep m.** Note this is the *opposite* of what holds for the parameters above —
block shape, `mvin` width and chunk count all measure differently under sharing. Idle resources
change nothing; contended ones change each other.

One trap when moving to a board with fewer accelerators: sending a RoCC instruction to a
non-existent opcode **hangs the machine** rather than raising SIGILL, and `buscheck` /
`sched_*` both touch all three contexts unconditionally (`grt_flush_ctx` at startup, m=1..3
loops). Fix the diagnostics as well as the benchmarks — see `bus2.c` and `m2_*.c`. (An earlier +14.2 %p figure came from a different baseline; use 12.70 %p for
same-core same-bus comparisons.) Throughput per LUT favours two accelerators everywhere, by 14 % at hidden 768 widening
to 33 % at 2048. So **use three only for hidden ≤ 1024; at transformer scale use two.** The mechanism
is the model's: bigger K and N raise a and b until each accelerator is already shared-resource-bound.
At hidden 3072 it reaches **+0.2 %** — the curve flattens at zero rather than going negative, as a
log-linear extrapolation would have wrongly predicted (−3.8 %).

**The whole multi-accelerator strategy fades with model size — but a third of that fade is a
blocking artifact, not hardware** (E376). Those numbers were all measured under `I·J <= 32` and
`Kc·(I+J) <= 512`; doubling the on-chip memory so `(8,8)` becomes legal takes the m=1→m=2 multiplier
at hidden 3072 from 1.223× to **1.503×**, and removes the size-dependent fade entirely up to hidden
1024 (1.899× vs BERT-base's 1.897×). Read the numbers below as properties of hardware **plus block
constraint**. Three accelerators give 2.22× at
hidden 768, then 1.95×, 1.66×, 1.51×, and only **1.15× at hidden 3072**, where utilisation falls from
70 % to 34 % and even a single accelerator drifts to 1.14× roofline. Past that size **what to widen instead is unknown**: the 256-bit bus is worth only 3.2 % there
(against 17–21 % at K ≤ 1024). The ceiling is byte-proportional — changing block shape at K=3072
moves the three-accelerator time 64 % — but it is not the on-chip bus, not L2, not weight residency
and not re-fetch count: holding total traffic fixed while varying unique weight size 4× and re-fetch
count 4× moves the speedup by 1.4 %. Nor is it DRAM bandwidth: at hidden 3072 the layer achieves 4.15 B/cycle against the 12 B/cycle that
64-byte runs sustain, and going from one accelerator to three raises achieved bandwidth by only 4 %
on FFN2 — half the bandwidth sits unused and cannot be reached. What fits every observation is a
**request-rate / memory-level-parallelism limit**: bytes matter because bytes are requests, a wider
bus does not reduce request count, and unique size and re-fetch count are irrelevant at equal
request count. That is inference by elimination, not measurement — this board cannot count
outstanding requests — but it points the design lever at deeper request queues or longer runs per
request rather than at more bandwidth or more arrays. Run length was then measured directly, holding intensity exactly equal and varying only J: going
from 32-byte to 128-byte runs is worth **34 %** at [12288×3072] and 5.8 % at [3072×12288], while
64→128 is inconsistent (+2.6 % / −5.0 %). **The penalty lives below 64 bytes** — the access
granularity — which gives the existing "keep J ≥ 4" rule its mechanism: J=2 means 32-byte runs and
half of every fetch is discarded. Total bytes still dominate, though: (8,4) at intensity 6.0 beats
(2,8) at intensity 10.0 despite half the run length, so minimise intensity first and keep J ≥ 4
second. This does not explain the 1.15× ceiling at large models, since the recommended blocks already
run at 64–128 bytes. Keep the 256-bit bus at
every size anyway — 3.2 % for +1.10 %p LUT still beats the third accelerator's 0.2 % for +14.2 %p.

**On (8,4) the third accelerator pays only below K≈1536.** Across 18 measured points it adds
**+10–14 % for K ≤ 1024 and exactly nothing for K ≥ 1536** (twelve measurements spanning −0.4 % to
+0.1 %), with a sharp cliff between. BERT's projections (K=768) are on the paying side and FFN2
(K=3072) is not — which is exactly what the layer measurement showed. Held out on unseen K and N,
the (8,4) model predicts m=1 to **0.24 %** and m=2/m=3 to **2.4 %**.

**Do not score the model by argmin over m.** On six held-out shapes it got the accelerator count
right 1/6 — but five of them have m=2 and m=3 within **0.7–2.4 %** of each other, which a 2 %-accurate
model cannot resolve, and the one shape with a real gap (8.7 %) it got right. An earlier 5/6 score
was a property of the shapes picked, not of the model. Report instead whether the predicted
difference clears the model's error bar: if not, call it a tie and **use fewer accelerators**, since
the time is the same and a unit is freed. Set that threshold at **5 %**, not 3 % — at 3 % the model
claimed 3.5–4.2 % wins on three held-out shapes that measured as 0.1–0.9 % ties.

**Optimal m depends on block shape — pick (m, I, J) together.** Holding the block at (4,6) makes
one shape prefer m=2; switching to (4,4) makes m=3 win by 9 %, reversing the answer. E191's warning
that the single-accelerator block optimum is not the shared optimum has a converse: an accelerator
count measured at a bad block is not the count you want. A partial ("tail") block costs 2–8 % at
m=3 and nothing at m=1 — real, but far too small to explain m-choice errors; a plausible story that
tails caused them was refuted by padding N to an exact multiple.

**A well-blocked single-accelerator matmul is just `roofline × 1.06`.** Across six shapes spanning
K=256–2048, N=512–2048 and an 8× range of working set, measured m=1 time is 1.05–1.07× the
`M·K·N/(dim²·f)` roofline — no byte counting, no bandwidth constant, no model. That localizes the
open problem precisely: what is unknown is neither compute nor bandwidth but the efficiency loss
from splitting one fixed problem across m accelerators, which runs 1.05–1.60× at m=2 and
1.29–2.42× at m=3 depending on shape.

**That loss factors into load imbalance times a working-set curve.** Sweeping K at fixed N (which
holds the block count constant) still moves the m=3 loss from 1.22× to 2.70×, so imbalance is not
the driver — but it is a real second-order term, and *counting* it (`m·max_a(columns)/N_tiles`,
nothing fitted) pulls the one outlier shape from 1.65× onto the curve at 1.10×. Corrected that way, the loss is
**additive and separable in K and N** — not a function of working set. A 3×3 grid holds working set
fixed at 1.24–1.31 MB across three shapes and measures η(3) = 1.42, 1.56 and 2.21, a 56 % spread;
meanwhile the increment from N=480 to N=1920 is +0.378/+0.377/+0.388, identical across all three K
rows. So `η(m) = imbalance · (1 + a(K,m) + b(N,m))`.

Both terms are empirical curves with **no mechanism** — an "L2 live-footprint" explanation was
proposed and then refuted by intervention: shrinking live bytes 3× via block shape made things
*worse*, and at K=2048 the only shape that fits L2 is the slowest of eight (η=4.05) while the
largest-footprint shape is the fastest. Treat a(K) and b(N) as measured, not explained.

**Block shape is separable from K, and E191's rule survives.** The ranking of eight shapes is
essentially identical at K=512 and K=2048 (same top three, same order), tracks arithmetic intensity,
and the only deviations are the known I↔J asymmetry. So choose the block once; it does not need
re-picking per K. It is worth a lot — best vs worst is **1.85×** at K=2048 with three accelerators,
more than a whole extra accelerator buys, and free.

**To answer just "does another accelerator help?", two measurements suffice — no model needed.**
Because the shared-throughput term does not scale with accelerator count, `t_m = max(t₁, (m/2)·t₂)`.
Measured on nine cases where sharing already binds at m=2 (FFN2, a K sweep, square matmuls on both
bus widths) this predicts m=3 to **4.4 % mean, 10.5 % worst**; FFN2 lands at −0.1 %. Where `t₂ ≈ t₁`
the shared term still has slack and the formula becomes an upper bound rather than an estimate —
which is exactly the right failure mode. Validated for m=2→3 only.

**Using it to pick a block shape** (`experiments/model/choose.py`): enumerate shapes under
`I*J <= 32` and divisibility, minimize the model. Its pick lands within 4.5 % of the measured best
on a 128-bit bus and 14.5 % on 256-bit, always inside the measured top three, versus **18–95 %**
for choosing from a single-accelerator profile. Every miss is an `(I,J)` vs `(J,I)` swap, which the
model cannot see — but **that swap is now predictable: take the larger `I` when `N >= K`, the
larger `J` when `N < K`.** 33 of 33 measured cells, 0.00 % mean excess, spanning N/K = 0.25–4.0
at m=2 and m=3, boundary bracketed between N/K = 0.75 and 1.00 (E412). This replaces both the
"measure the transpose too" advice and the `m<=2 and K>=2560` tie-break, which loses 4.6 % mean /
30.9 % worst on the same cells — `(m,K)` was a proxy for `N/K`. The earlier reading that the swap
has "no consistent direction" came from near-square blocks ((5,4) vs (4,5)) on shapes sitting at
N/K ≈ 1, i.e. **on the boundary**: it was a property of the measured range, not of the effect.
The recommended (8,8) block is its own transpose, so the question only arises for asymmetric blocks.

**Using it to pick hardware** (`experiments/model/space.py`): sweeps n × m × bus width with no
workload measurement at all. Its headline is that **the optimal hardware depends on the ISA's
command granularity** — with `loop_ws`, core count is irrelevant and bus width decides (best
throughput/LUT at n=1, m=3, 256-bit); with tile-by-tile issue, cores decide and bus width is
worthless (best at n=2, m=2, 128-bit). Asymmetric and unbalanced cases need **no extra parameter**: accelerator compute times combine
with plain `max` (they are genuinely parallel — each owns its array, scratchpad and accumulator),
and everything shared is already in the resource terms. Measured with unequal work on two
identical accelerators (256³ against 128³, 320³ against 256³, and so on), the interleaved time is
`max(t_a, t_b)` to within **0.6 %**, and the smaller job vanishes entirely. An earlier reading of
`blf50` data suggested accelerators needed their own exponent of ~4.5; that was **wrong** — those
runs used the naive stepper on a 128-bit bus and were issue-bound, and the "exponent" was silently
standing in for the `T_issue` term that had been left out of the comparison.

That core-count claim was then **built and confirmed**. `Rocket64b1gem3i8w256f50` (one core,
three accelerators, 256-bit) matches the two-core board to within **5 %, mostly under 1 %**, across
192³–512³ and m=1..3 — and is 4.9 % *faster* at 512³ with three accelerators. The pre-registered
performance predictions, made before the bitstream existed and using no workload measurement at
all, came in at **5.6 % mean error** over ten points. So with a coarse-grained ISA the second core
costs 2.62 %p LUT and contributes nothing; **n=1 is optimal** (and is the floor, since Linux needs
a core). Two cautions: a Rocket core costs **2.62 %p LUT, not the 4.5 %p** the area model assumed
(CLB/LUT stays at 1.449), and on a single-core board the kernel shares the benchmark's core, so
**the first run after boot is contaminated by 2–3×** — always run it at least twice.

## Switching between the two accelerators is free

Measured on `Rocket64b2gembl` by holding total work fixed and varying only the order — ten INT8
then ten FP32 (1 switch) versus strictly alternating (19 switches). Alternating came out 0.64 %
*faster*, i.e. the cost is below the noise floor: 19 switches against 0.131 ms of noise puts the
per-switch cost under ~7 µs (a 50 µs switch would have shown as a clear 4.6 % increase).

It is free because the two accelerators are **independent** RoCC units with their own config
state and scratchpad — switching is just issuing instructions under a different opcode, with no
shared state to save or restore. This is a side benefit of not using upstream's shared-scratchpad
`DualGemminiConfig`; that arrangement would contend on the shared memory.

Practically this means switching *accelerators* is unconstrained. Switching *datatypes* is not —
those are different things, and conflating them is a trap. Measured on a two-stage pipeline at
N=64: pure INT8 0.488 ms, pure FP32 3.594 ms, and mixed FP32→INT8 **5.222 ms** — the mixed path is
slower than doing everything in FP32, because the float→int8 conversion at the boundary alone
costs 3.002 ms (57 % of it). Combined with the accuracy result (mixing barely helps, since
outliers propagate into the next stage anyway), interleaving datatypes within a chain loses on
both counts. Pick a datatype per workload or phase and keep the whole chain in it; never convert
mid-chain.

Read the size sweep correctly though — **FP32 is never faster in absolute terms**. INT8 wins at
every size measured (0.029 ms vs 0.053 ms even at N=32); what crosses over is *utilization*, not
speed. So the sweep does not say "use FP32 for small matrices". It says what precision **costs**:
choosing FP32 for a small matmul costs 1.83×, while the same choice on a large one costs 4.4×.
Put the FP32 work where the matrices are small.

## What INT8 actually costs in accuracy

Same core, same library path, same data (fixed PRNG seed), CPU double as oracle. INT8 goes
through symmetric quantization (scale = max|x|/127) and back; results read with `full_C=true` so
the full-width accumulator is compared, not the saturated `elem_t` writeback.

| N (=K) | INT8 mean rel. error | FP32 mean | FP32 max |
|---|---|---|---|
| 32 | **2.07 %** | 0.0000 % | 0.00 % |
| 64 | **3.12 %** | 0.0000 % | 0.03 % |
| 128 | **4.64 %** | 0.0000 % | 0.11 % |

INT8 error grows roughly as √K, as independent quantization noise should. FP32 is effectively
exact even at K=128. Ignore INT8's *max* relative error (up to 26000 %) — random matrices produce
near-zero outputs and the denominator blows up; only the mean is meaningful.

Note the inversion this creates against the speed numbers: FP32 is cheapest exactly where INT8 is
already accurate (small matrices, 1.83× for 2.07 % error) and most expensive where INT8 drifts
(large matrices, 4.4× for 4.64 %). So "run the small ops in FP32 to buy precision cheaply" is
weaker than it sounds. Reach for FP32 for dynamic range (no saturation) or for algorithms that
need it — normalization, softmax — rather than to fight accumulated quantization error.

When measuring this, pass `full_C=true`. With `full_C=false` the result is narrowed to `elem_t`
and INT8 saturates at ±127 (K=128 accumulates to ~2 M), which reads as ~99 % error and looks like
broken hardware. FP32 has no saturation and is unaffected — one datatype looking impossible while
the other looks fine is a signal to suspect the measurement, not the chip.

## Dynamic range is what actually breaks INT8

Symmetric quantization uses scale = max|x|/127, so one large outlier raises the scale and starves
everything else of levels. Scaling 1 % of the entries by R (N=K=64, same core, same data):

| R | INT8 error (vs output RMS) | **entries crushed to 0** | FP32 error |
|---|---|---|---|
| 1 | 0.44 % | 17/4096 | 0.0000 % |
| 10 | 2.79 % | 158/4096 | 0.0000 % |
| 100 | 3.02 % | **1545/4096 (38 %)** | 0.0000 % |
| 1000 | **0.86 %** ↓ | **4055/4096 (99 %)** | 0.0000 % |

**The error metric goes down at R=1000 while the computation is being destroyed.** At that scale
almost every ordinary value rounds to zero, the output is decided entirely by the surviving
outliers — which quantize accurately — and the RMS used to normalize the error is dominated by
those same outliers. Watch the crushed-entry count instead; it rises monotonically. This is the
fourth time in this journal a single metric has pointed the wrong way, and the lesson tightens:
ratios whose denominator moves with the data can invert, so always pair them with an absolute
count.

For LLM work this is the concrete argument for float formats: activation outliers are the known
problem, and a per-tensor INT8 scale destroys 38 % of the tensor at 100× outliers and 99 % at
1000×. What makes FP8 better is the exponent, not the bit count — FP32 stays exact at every R
here. Using INT8 seriously means per-channel scales or outlier splitting, and that software cost
has to come off the 1.83–4.4× speed advantage.

## Building the bundled Linux tests

**Never pass `-DBAREMETAL=0`.** The sources branch on `#ifndef BAREMETAL`, so defining it *at
all* — even to 0 — takes the bare-metal path and silently drops the `mlockall` call. Gemmini's
DMA walks virtual addresses through its own TLB, so unpinned pages make a transfer stall and the
hart waits forever: the machine wedges with no kernel message, on any bitstream. Just omit the
flag (`riscv64-linux-gnu-gcc -O2 -static -march=rv64gc -mabi=lp64d -I. -Iinclude ...`) and check
with `nm <binary> | grep mlockall` that the symbol is actually there.

That failure mode cost several boot cycles. The check that broke it open was reproducing the hang
on `d9-FINAL`, a bitstream already known to run the same workload — **when a suspect config
misbehaves, try the same thing on a known-good one before blaming the config.**

`read_cycles()` uses `rdtime`, not `rdcycle`, so its "cycles" are timer ticks. Divide by the
device tree's `timebase-frequency` (312,500 Hz here, i.e. clock/100) to get seconds — not by the
CPU clock.

## Two traps when reasoning about INT8 in practice

**Software quantization can cost more than the accelerator saves.** A two-stage pipeline measured
on `Rocket64b2gembl` took 58.7 ms all-INT8 versus 24.0 ms all-FP32 — but the matmuls themselves
are only 0.49 ms and 3.54 ms respectively. The rest is `lrint`+clamp over every element, and on a
31 MHz core that dwarfs the compute. This does **not** contradict the size sweep, where INT8 wins
at every shape; Breaking that down: the `lrint`-through-double conversion is 7.2× more expensive than an integer
requantize (int32 → multiply/shift → int8), and the integer path still costs 12× the matmul at
N=64, falling as ~1/N (measured 11.9× / 5.8× / 2.8× at N=64/128/256, matching the model to
2–5 %). The accelerator runs ~0.9 ns per MAC against ~345 ns per element on this core, so any
O(N²) host work throttles an O(N³) accelerator op.

**But none of that host work is necessary.** Gemmini requantizes in hardware: call
`tiled_matmul_auto` with `full_C=false` and the accumulator's int32 is scaled by `ACC_SCALE` and
narrowed to `elem_t` during mvout, for free. (That is the same narrowing that saturates INT8 at
±127 when you are trying to *inspect* accumulator values — a nuisance for measurement, exactly
what you want in a real pipeline.) Measured: a two-stage pipeline costs **1.99× a single matmul** with hardware requantization
(`full_C=false`) versus **31.45×** when the host does the int32→int8 step — 15.8× apart, with 97 %
of the host version being conversion. So the host only quantizes once at the input; between layers
the data stays int8 and never round-trips through float. Use `full_C=true` only when you need to
*inspect* accumulator values, never in a production path. Note also that mvin scaling does **not**
substitute for float→int8 conversion — it multiplies data that is already `elem_t` in DRAM.

**Outliers propagate, so partial mixed precision does not rescue accuracy.** Running only the
outlier-heavy first stage in FP32 and the second in INT8 left the error essentially unchanged
(55.5 % vs 56.1 % all-INT8; all-FP32 is 0.000 %). The FP32 stage computes correctly, but its
output still carries the wide dynamic range, and quantizing it for the next stage destroys it
again. Keep the whole path that carries outliers in float, or fix the quantization itself
(per-channel scales, outlier splitting) — patching one stage buys nothing.

Measuring path B at all requires one binary driving both accelerators, which is what the
big.LITTLE config plus `gemmini_rt.h` make possible; a header-based build can only measure the
two homogeneous paths.

## One core can drive both accelerators at once — if you interleave at tile granularity

Measured on `Rocket64b2gemblf40` with equal work on both sides (1024 tiles each) and both outputs
verified:

| | Time | Correct |
|---|---|---|
| INT8 alone | 3.195 ms | PASS |
| FP32 alone | 2.453 ms | PASS |
| Interleaved | **3.897 ms** | both PASS |
| Sum (no overlap) | 5.648 ms | |
| Max (full overlap) | 3.195 ms | |

**71.4 % overlap, 31 % less wall-clock**, from nothing but the order commands are issued in.

Granularity decides it. Interleaving whole matmuls gives **0 %** overlap: one 64³ matmul issues
~384 commands, far more than the reservation station holds, so the core stalls mid-matmul feeding
one accelerator while the other sits idle. Interleaving single tiles (~6 commands) keeps both
queues fed. It never reaches 100 % because one core still has to issue every command, so issue
bandwidth caps it.

**That cap is the real limit, and every overlap number here was measured against a baseline
that was only 12 % utilized.** `grt_step` reloads both A and B per tile-op, so the accelerator
idles ~88 % of the time and a second one has lots of slack to fill. Making the baseline better
shrinks the gain: a reuse stepper (`GRT_DEFINE_STEP_R` — loop order `i,k,j` so the A tile is
reused across `j`, plus B double-buffering to break the WAR that serialized mvin against
compute) cuts commands per tile-op from 6 to 3, raises utilization to 22 %, and drops the
two-accelerator gain from 2.00× to **1.77×** at 256³ (1.63× → 1.23× at 64³). The two effects
still multiply in absolute terms (10.886 ms → 3.324 ms, 3.28×), because they attack different
bottlenecks.

Counting commands locates the bottleneck: at 256³ the split path runs at 11–13.5 cycles per
RoCC command while the single-accelerator path runs at 24 — **the single case waits on the
accelerator, the split case waits on the core's issue path.** So reducing commands per tile-op
pays twice: it speeds up the single case *and* raises the ceiling for the multi-accelerator
case. The corollary is a boundary: against the library's 86 %-utilized path, one core would
not be able to feed two accelerators at all. Single-core/multi-accelerator works only while
each command keeps the accelerator busy long enough — past that, split the issue across cores
(1.88×, spin-synchronized) or use Gemmini's loop FSM so one command drives many tiles.

**Injecting dummy CPU work into the issue loop confirms this directly and gives the budget.**
The single-accelerator path absorbs **~22 cycles per tile-op for free** (a 21.3-cycle pad costs
−0.2 %); the split path absorbs **~2.6** (a 9.3-cycle pad already costs 15.7 % and drops the gain
from 1.69× to 1.46×; 57 cycles drops it to 1.06×). This also rules out on-chip bus contention as
the limit — bus contention would be indifferent to CPU work. So **the issue loop must be
essentially empty**: no bounds checks, no address recomputation, no fused application code. That
is a hard constraint, not a code-quality preference. It also corrects the earlier claim that
single-core/multi-accelerator wins because the second core would otherwise spin uselessly — the
single core is equally unavailable, since it is saturated issuing. The remaining advantages are
area (one fewer core) and not needing a hand-tuned spinlock.

**With a hand-written stepper, two cores beat one core.** Matched comparison at 128³ (independent
matmuls both sides, spin synchronization on the two-core side): old stepper 1.88–1.90× (one core)
vs 1.89× (two cores, `d9`) — identical; reuse stepper **1.35× vs 1.95×** — two cores win by 44 %.

**But that whole comparison is an artifact of issuing tile-by-tile.** Gemmini's `loop_ws` drives
an entire I×J×K tile loop from 6 commands (512 tile-ops at 128³), so the core issues once and is
then free. Measured on one core with two accelerators: **64.0 % utilization and 1.86×** at 128³,
with the concurrent case only 4.7 % slower than a single matmul. Absolute time for two 128³
matmuls: 0.268 ms (one core, `loop_ws`) versus 0.780 ms (two cores, reuse stepper, clock-
corrected) — **2.9× faster with half the cores.** So the ordering is: command granularity matters
far more than core count, and the performance argument for single-core/multi-accelerator holds
*provided the ISA has a coarse-grained command*. The apparent "this technique dies as the
interface improves" pattern (1.88× → 1.35×) was real but mis-attributed — cutting commands
linearly also speeds the accelerator up proportionally, whereas `loop_ws` cuts them non-linearly
and escapes it. Use `grt_loop_ws` / `grt_loop_ws_config` in `gemmini_rt.h`.

**The firm constraint is `I*J <= 32`; the scratchpad side is not a clean rule.** With `K=48` tiles
(BERT's 768), `(8,4)` returns wrong results while `(8,2)` and `(4,4)` pass, which looks like a
scratchpad budget of ~512 tiles for `I*K + K*J` — but at `K=192` tiles, `(2,1)` and `(1,2)` need the
same 576 tiles and pass fine. The only failure not explained by the accumulator rule is that one
shape, where `I*J` sits *at* its limit of 32 and `A+B` is also large. So: keep `I*J <= 32`, and
**whenever `I*J` is at 32 and `I*K + K*J` exceeds ~500 tiles, verify correctness for that specific
shape** rather than trusting a formula.

**Split K for large-K matmuls — use `grt_block_ksplit`, not a single `grt_loop_ws`.** With K passed
whole, the scratchpad squeezes the block to `(1,1)` and arithmetic intensity balloons to 32 B per
compute-cycle: at BERT's FFN2 (`[128×3072]×[3072×768]`, K = 192 tiles) that is 146.040 ms at 16.2 %
utilization against the library's 27.688 ms. Chunking K into 16 tiles allows `(8,4)` and gives
**24.956 ms at 94.5 %** — 5.85× faster and **10 % ahead of stock `tiled_matmul_auto`**. K-splitting
does not change total bytes moved; it changes what block sizes are legal. `Kc=16` with `(8,4)`,
`(4,4)` or `(4,6)` all land within 1 %, so the setting is not delicate — only badly-shaped combos
like `(2,2) Kc=64` (intensity 16.1) fall off, exactly as the intensity formula says.

**But FFN layers are DRAM-bound and multi-accelerator barely helps there.** FFN2 gets 1.22× from
two accelerators and nothing from a third, sitting on the 2.91 B/cycle mixed-DRAM ceiling; with
2.33 B/cycle of unique traffic per compute-cycle, the ceiling puts the maximum at 1.25×, so 1.22×
is already the limit. Projections (K=768, 576 KB weights) get 2.0–2.25× from the same unique-traffic
ratio because they *achieve* more DRAM bandwidth (4.3 vs 2.45 B/cycle — projections sit right on the
4.75 read ceiling, FFN2 stalls at half of it). **Why is unresolved and the hunt was abandoned after
seven rejected hypotheses**: access width (identical-intensity `(8,4)` and `(4,8)` finish within
0.7 % despite 2× row width), inter-iteration L2 residency (rotating cold weight copies changes
nothing — 2.20× vs 2.24×), and read/write mix (FFN2 writes *less*, 3.4 % vs 12.5 %, yet performs
like the mixed ceiling) are all out. Treat it as a measured phenomenon: **at equal unique traffic
per compute-cycle, achieved DRAM bandwidth still varies 1.75× by layer, so profile each layer
rather than extrapolating from one.**

Use `(8,4)` or `(4,8)` with `Kc=16` for FFN2. Note `(1,32)` **fails correctness** despite
`I*J = 32` — the second such case, so re-verify whenever `I*J` sits at the limit.

**Keep each `loop_ws` block to at most half the accumulator — `I*J <= 32` of the 64 tiles.** The
loop FSM double-buffers, so it needs room for two blocks; a block that fills the accumulator
overwrites the previous one's output and results are wrong (deterministically within a run,
differently across runs). Measured boundary at 192³: 25/64 tiles passes, 36/64 fails. The correct
setting is also the fastest — `BLK=4` gives 0.626 ms and **88.3 % utilization**, beating both the
broken `BLK=8` (0.678 ms) and stock `tiled_matmul_auto` (0.636 ms). Fencing between blocks also
"fixes" it but costs 23 % and treats the symptom.

**What another accelerator buys you equals the slack in the one you have**, and with `loop_ws` that
slack is the **system bus**. Achieved byte rate (countable exactly from the blocking: `I*K + K*J`
tiles in, `I*J` out, 256 B each) tops out at **15.0 B/cycle against a 16 B/cycle 128-bit bus** —
94 % — and it lands there from every direction: utilization ranges from 22 % to 89 % across sizes
and accelerator counts, but the byte rate converges. That makes the rule predictive:

> useful accelerator count ≈ 15.0 / (B/cycle of one accelerator)

Measured: 192³ predicts 1.81, gets 1.81; 160³ predicts 1.64, gets 1.65. It over-predicts below
~160³ (128³: 2.03 vs 1.85; 64³: 3.79 vs 2.02) because per-block fixed overhead binds first there,
so the formula is only valid once one accelerator already uses half the bus.

The design prescription follows directly: **a third accelerator is starved by the bus, not by
having too little work — widen the system bus rather than adding arrays.** This was built and it
paid off: `Rocket64b2gem3i8w256f50` (accelerator DMA left at 128 bits, `SystemBusKey.beatBytes`
raised to 32) costs **+1.10 %p LUT** — one thirteenth of an accelerator — with CLB slightly *down*
and timing better (WNS +0.101). The matmul ceiling rose 15.0 → **24.04 B/cycle** and 192³/3-accel
utilization went 53.6 % → **85.9 %**. Most usefully, **two accelerators now scale essentially
perfectly at every size — 1.96–1.98×**, versus 1.57–1.69× on the narrow bus. At 512³ with three
accelerators the hardware and software wins multiply: 1.50× (128-bit, `(4,4)`) → 1.99× (128-bit,
`(8,4)`) → 2.20× (256-bit, `(4,4)`) → **2.79×** (256-bit, `(8,4)`).

**The wide bus only helps L2-resident work.** On a real workload — BERT-base Q/K/V, three
independent `[128×768]×[768×768]` matmuls, one per accelerator — a single accelerator hits **94.7 %
utilization** (the best measured anywhere in this track, because a large K amortizes per-block
overhead), two give **1.92×**, and the third gives **nothing** (1.91×; 2→3 costs exactly +51 % time
for +50 % work). The 768×768 weight matrix is 576 KB, larger than L2 all by itself, so it streams
from DRAM: unique traffic is 3.56 B/cycle at two accelerators and 3.53 at three, sitting on the
DRAM ceiling (4.75 read / 2.91 mixed) rather than the 24 B/cycle on-chip one. **So the right answer
is workload-dependent: three accelerators for L2-resident matmuls (2.79–2.90×), two for
weight-streaming layers (1.92×).** A configuration tuned on synthetic benchmarks gives the wrong
count for the real one.

**Order the block loops so the big operand stays fixed.** The real limit on that BERT shape is not
unique data but **re-fetch**: with `for i0 { for j0 }` each `i0` row sweeps all 576 KB of W, which
exceeds L2, so W is re-read once per `i0` block. Swapping to `for j0 { for i0 }` pins a 72 KB W
block across all `i0` and W is read once — three accelerators go 1.97× → **2.35×** at M=512, two go
1.87× → 2.03×. Batching does *not* fix this (raising M from 128 to 512 cut unique DRAM traffic from
4.25 to 1.52 B/cycle, a third of the ceiling, and the third accelerator still added 0.03). Rule:
whichever operand gets swept is re-fetched, so if it is larger than L2, make it the outer loop
instead. Square benchmarks cannot show this — with a 108 KB B matrix, sweeping is free, which is
why traversal order measured as irrelevant there. Nor can a single accelerator: on one unit the two
orders differ by **0.1 %** (6.224 vs 6.218 ms), because one unit's re-fetch is 3.7 B/cycle, under
the DRAM ceiling; three units put it at 11 B/cycle, over. Like block shape, this is a decision that
is invisible without sharing.

For reference on that shape, stock `tiled_matmul_auto` runs it in 6.068 ms at **97.2 %
utilization** and `grt_loop_ws` is 2.5 % behind at 94.9 % — so the multi-accelerator numbers are
measured against a baseline that is essentially the library's, not an inferior one. **The
end-to-end headline is 2.0–2.3× over the best single-accelerator path on a real BERT projection
shape.**

A caution about how that ceiling was diagnosed: a no-compute workload (pure strided `mvin`/`mvout`)
also topped out at 15 B/cycle on the narrow bus, which looked like independent confirmation. It
was not — on the *wide* bus it still tops out at 15.3 while matmul reaches 24. Its limit is
request rate, not bus width: each `mvin` row is a separate 16-byte request against 192-byte
striding, so widening the beat buys nothing, whereas matmul's contiguous 192-byte row loads use
the full width. **Two measurements landing on the same number is not evidence of the same cause;
only changing the suspected cause separates them.**

**Streaming past L2 (512 KB) collapses that ceiling** to 4.75 B/cycle (read), 3.54 (write), 2.91
(mixed), and it then *falls* with accelerator count (7.83 → 7.76 → 4.75) instead of rising, because
three strided streams destroy each other's locality. Mixing reads and writes is nearly free in L2
(10 %) and costly in DRAM (39 %).

**But blocked matmul does not stream, so it barely suffers.** What is live is the block, not the
matrix: a `(4,4)` block at 384³ touches `4*24 + 24*4` tiles = 48 KB, so three accelerators keep
only ~156 KB live no matter how big the matrices are. Measured out to 512³ (matrices 2304 KB, 4.5×
L2), a single accelerator does not degrade at all — 7.98–8.45 B/cycle at every size. Keep
`I*J <= 32` and the multi-accelerator result carries from toy sizes to real ones.

**Pick the block shape from the arithmetic-intensity formula, and pick it for the shared case.**
For an `(I,J)` block over `T` tiles per side, bytes are `256*(T³/J + T³/I + T²)` against `16*T³`
compute cycles, so

> intensity = `16 * (1/I + 1/J + 1/T)` bytes per compute-cycle

At 512³ (T=32) that is 8.50 for `(4,4)`, **6.50 for `(8,4)`**, 10.50 for `(8,2)`. The formula
predicts multi-accelerator throughput directly: `(4,4)`→`(8,4)` predicts 1.31×, measured **1.32×**
on three accelerators; `(8,2)` predicts 0.81×, measured 0.75–0.79×. On a *single* accelerator all
five shapes land within 3 % — the choice is invisible there.

**The intensity formula is blind to *which* operand carries the traffic, and that matters once
`M >= 256`.** A block reads A once per j-block and B once per i-block: `A = 16*K*N/J`,
`B = K*N*(TI/I)` with `TI = M/16`. The sum is exactly proportional to intensity, but `(16,4)` —
legal only when `TI >= 16`, i.e. `M >= 256`, and only under the doubled-memory `I*J <= 64` limit —
is the **only block that reads B exactly once** (at M=256), paying 4× the A re-fetch instead.
It wins even though it moves **1.25× more total bytes**, so B bytes must cost more than A bytes —
but **not because A is L2-resident**: that explanation was pre-registered and refuted (E417). Two
cells with an identical `A = 384 KB` give +7.1 % and +4.4 %, and crossing the 512 KB L2 with A
changes nothing (+3.4 % at A = 768 KB). This is the third L2 explanation refuted in this project;
a number landing near a cache size is not evidence. What is observed rather than explained: the
gain shrinks with M at a **K-dependent** rate — at K=1536 doubling M costs exactly −3.7 %p
(identical at N/K = 3, 4 and 6) and at K=768 it costs −1.1 %p; below the crossover M does nothing
(+0.2 %p). Measured at M = 256/512/1024,
`[768x3072]` gives +5.5/+4.4/**+4.1 %** (flattening near 4 %) while `[1536x6144]` gives
+6.3/+2.4/**+1.3 %** (decaying toward zero). `TI/I` is identical for both at each M, so it does
not explain the split either; the mechanism is unresolved. **The crossover in N/K does not move
with M** — at K=1536 both the M=256 and M=512 rows flip sign between N/K = 2 and 3 (E420). Its
location does depend on K, though: N/K = 3 is **+7.9 % at K=1536 (the largest gain measured)** and
−3.1 % at K=1024. Sweeping K along the N/K = 3 line settles that it is **not a function of K
either**: +0.4 / −5.5 / +5.3 / +8.0 / −1.2 % at K = 768/1024/1280/1536/2048 — non-monotone with
three sign changes, and no variable tried (Kc, chunk count, nj, TI/I, A/B/C size, K·N) is
non-monotone there (E421). So `N >= 4K` is kept because it has **zero false positives** across
every measured cell, while knowingly leaving the best single cell on the table; treat N/K = 3 as
a measure-and-see region, not a rule. One caution when measuring it: that `(16,4)` cell moved **2.4 %**
between two sessions — but that is the **worst of 20 repeated cells** whose median is 0.43 %
(E422), so read gains under 1 %p as noise and 1–3 %p with care, rather than discarding
everything under 3 %p. Practically there is **no upper M
bound** — all six cells are positive across seq 256–1024 — but at large M with large K the gain
is ~1 %, below this project's "worth a second measurement" bar. m=1 is −0.4 to −1.1 % throughout,
so the effect stays shared-only along the M axis too (E417/E418). Measured at M=256, m=3 (eight points, two of them a pre-registered out-of-sample test with
A held constant): `(16,4)` vs `(8,8)` is −22 % at N/K = 0.25, −14/−17 % at 1, −3.2 % at 2, −3.1 % at
3, **+6.6 % at 4, +8.8 % at 6, +10.7 % at 8** — monotone in N/K with the crossover in (3, 4]
(E413–E415). N/K is the variable, not A size: A varies 4× across the fitted points and is constant
across the held-out ones, and both lie on one curve. **Do not put `(16,4)` into the intensity
ranking** — at intensity 5.0 it loses to intensity-6.0 blocks below N/K = 1, so the formula
mis-orders it; `tune.py` prints it as "also measure" when `M >= 256 and N >= 4K` instead.
`[H x 4H]` is the transformer FFN1 shape, so at seq >= 256 that stage is in the winning regime —
**confirmed in an assembled layer**: BERT-base at seq=256 goes 43.28 -> **42.49 ms**, a stage gain
of +5.2 % carrying to +1.9 % on the layer (E416). **But only at m=3**: the same swap is −0.6 % at
m=2 and −0.8 to −6.5 % at m=1, because the trade moves DRAM traffic to L2 and with fewer
accelerators there is DRAM headroom to spare. That is the mirror image of this project's usual
pattern — normally a parameter that shows no difference at m=1 is untested rather than rejected;
here one that *gains* at m=3 *loses* at m=1. Either way the shared resource's slack sets the sign.
That layer also confirms "M enters only through the roofline" at layer level for the first time:
seq=256 lands within **0.3 %** of exactly twice the seq=128 layer.
This blind spot existed because `all_blocks()` capped `I` at 8 — true when `M = 128` (TI = 8),
silently wrong after the on-chip memory was doubled.

So minimize `1/I + 1/J` subject to `I*J <= 32` (the accumulator half-rule; `I*J = 32` is verified
safe at 512³) and `T` divisible by both. **Use this to shortlist two or three candidates, then
measure among them — it does not pick the winner outright.** Where the shape is not in a penalized
class the formula is exact (`(5,4)` +0.7 %, `(6,4)` +1.0 %, `(6,2)` −1.1 %), but odd×odd shapes run
7–17 % slower than it predicts, and swapping `I`↔`J` at identical intensity moves time by up to
12 % with no consistent direction (`(5,4)` beats `(4,5)` at T=20; `(3,4)` beats `(4,3)` at T=12).
Concretely, the formula's pick at T=20 is `(5,5)` but the real winner is `(5,4)`, 4.5 % faster
despite 11 % worse intensity. Switching `(4,4)` →
`(8,4)` takes three accelerators from 1.50× to **1.99×** at 512³: a 32 % gain from software alone,
and the largest single software win in this track. It also reverses what looks true under `(4,4)`
blocking, where two accelerators beat three at every size ≥ 320³ — that is a property of the bad
block shape, not of the hardware, and `Rocket64b2gem3i8f50` is worth its area once blocked
correctly.

The full timing model is **`time = max(compute_floor, bytes / (15.0/N))`** — each accelerator gets
an Nth of the shared 15 B/cycle. Verified at 192³ across a 1.86× range of bytes and 1→3
accelerators: predicted 0.639/0.619/1.917/1.032 ms against measured 0.650/0.626/1.844/1.024, all
within 4 %, with both terms taking turns as the binding one. `compute_floor` is
`k * N³/256` cycles where the overhead factor `k` falls with size (1.62 at 96³, 1.33 at 128³,
1.21 at 160³, 1.12 at 192³). There is **no per-block fixed cost** — fitting one gives a negative
constant; at 192³ moving 86 % more bytes across 4× as many blocks costs only 4 % on one
accelerator. (`BLK=3` is an unexplained outlier — slower than `BLK=2` despite less of everything,
reproducibly at both 160³ and 192³. Avoid it.)

**The practical consequence: block size only matters when accelerators share the bus.** Going from
`BLK=2` to `BLK=4` at 192³ is worth **3.8 % on one accelerator and 80 % on three**. Tuning against
a single-accelerator profile makes the choice look irrelevant. Conversely "one accelerator is not
bandwidth-bound" holds only for good blocking — `BLK=2` alone already pulls 14.75 B/cycle.

**Blocks need not be square, and the best shape inverts with accelerator count.** Sweeping `(I,J)`
independently: at 192³ the compute-optimal shape is `(6,2)` (k=1.110) but the byte-optimal is
`(4,6)`, and `(6,2)` wins by 2 % on one accelerator while losing by **44 %** on three. At 128³ the
inversion is sharper — the single-accelerator top three are `(4,4)/(2,2)/(8,2)` at 0.222/0.224/
0.224 ms (within 1 %, i.e. a coin flip against 0.3 % timer noise), and those same three run
0.358/**0.594**/0.420 ms on three accelerators, a **66 % spread decided by a tie**. `k` also depends on
shape, but be careful with it: a single `loop_ws` block is **symmetric** in `I`↔`J` (at 192³
`(2,3)` and `(3,2)` both take exactly 66 µs), so any shape asymmetry comes from how blocks chain,
not from the block itself — and for three shapes, `(3,2)`, `(2,4)`, `(3,4)`, that chaining is
**bimodal**: repeated runs of the same binary land at either ~670–710 µs or ~790–900 µs, a swing
of up to 28 %, while the other twelve shapes reproduce within 2 %. Cause unknown — block traversal
order and buffer address offset (swept to 32 KB) were both ruled out. Repetition cannot catch it:
40 measurements inside one process all land within 3 % of each other, so only **relaunching the
process** samples the other mode. **Keep `I >= 4, J >= 3`** — not because those shapes average
better but because they are the ones whose performance is reproducible (k = 1.11–1.15).

Which path shape acts through changes with size: at 192³ it moves `k` by 1.5× and bytes by 1.5×;
at 128³ it barely moves `k` (1.36–1.49) but moves bytes by **2.25×**. So when accelerators share,
**minimize bytes**; the N=3 ranking follows byte count almost exactly.

The two halves of the model are not equally trustworthy. The bandwidth term `bytes/(15/N)` is
genuinely **predictive** — pre-registered on an unmeasured size (160³) it got the three-accelerator
ordering exactly right and four of six magnitudes within 3 %. The compute term is **not**: the same
pre-registration predicted `(5,5)` as the single-accelerator winner and the real winner was `(5,2)`,
because `k` was extrapolated from square blocks and `k` is strongly asymmetric in `I` vs `J`.
Measure `k` for the exact `(size, I, J)` you care about; never extrapolate it.

The headline holds across all three sizes measured: **the single-accelerator optimum is never the
shared optimum** — using it costs 18 % at 128³ (95 % if you pick a different member of its
near-tie), 46 % at 160³, 44 % at 192³.

Beware the baseline when reporting multi-accelerator gains. The same 128³ point measured 2.39×
with `BLK=8` and **1.85×** with the correct `BLK=4`, purely because the correct blocking made the
single-accelerator baseline 14 % faster and left less slack to fill. A multi-accelerator speedup
is meaningless without stating that its baseline was tuned.

Do **not** pass `a_spad_id`/`b_spad_id` of 1/2 (`grt_loop_ws_id`): the library pairs those with a
NULL operand pointer (`tiled_matmul_outer` lines 819-820), and imitating half the protocol wedges
the machine hard enough to need a re-program.

One encoding trap there, and the macro alone will not reveal it: **`ex_accumulate` must be 0** for
the no-bias case. The library computes it as `!no_bias || D == NULL`, which looks true when there
is no bias — but `tiled_matmul_outer` substitutes `D = (void*)1` as a dummy first, so `D == NULL`
is false and the whole expression is 0. Passing 1 leaves the accumulator un-cleared and every
result is wrong *at identical speed* — nothing but the correctness check catches it. When copying
an encoding, check what the caller actually passes, not just the macro.

Two further effects worth keeping separate from the issue-bandwidth story. **Operand sharing
starts to matter once loads are cut**: splitting one matmul across the two accelerators (both read
the same B, so it hits in L2) gives 1.58× while two independent matmuls give 1.35× — the reverse
of the old stepper's ordering (1.84× vs 1.93×). And **interleaving keeps two working sets live at
once, so it crosses the L2 threshold before the sequential version does**: at 256³ with six 64 KB
arrays (448 KB against a 512 KB L2) the measured gain flipped between 0.98× and 1.53× across runs
of the same binary. Keep benchmark footprints well under L2 or you will measure capacity, not
concurrency.

Use `grt_mm2(&w1, &w2)` from `gemmini_rt.h`, which walks two independently-shaped workloads a
tile at a time and fences once at the end. `tiled_matmul_auto` cannot do this — it fences
internally, which makes the overlap impossible and the experiment unmeasurable.

**Specialize the issue loop or lose most of the gain.** Passing the accelerator context through a
pointer stops the compiler folding `GRT_DISPATCH`'s opcode branch and the dim/element arithmetic,
and that CPU work sits directly on the issue path — which matters here precisely because one core
has to keep two queues fed. Measured on the same binary, same run: pointer context 1.24×, opcode
constant 1.38×, opcode+dim+elem constant **1.69×**, fully inlined by hand 1.79×. So the macro
`GRT_DEFINE_STEP(NAME, OP, DIM, EB)` generates one specialized stepper per accelerator shape.
Note this does not contradict the earlier result that cutting command *count* changed nothing:
with a single accelerator the core is waiting on the queue anyway, so spare CPU work is free.

On real matmuls the gain is bounded by how evenly the work splits, and the unit to balance is
**tile-ops = (N/dim)³, not matrix size**. Two 64³ matmuls look balanced but are not: INT8 (dim 16)
does 64 tile-ops while FP32 (dim 8) does 512, so interleaving hides only the small side and saves
11 %. Size them to equal tile counts — INT8 128³ against FP32 64³, 512 each — and the same
interleaving gives **1.7–1.8×**, with both results verified. That holds across a 64× range of
problem sizes (1.45× at 64 tile-ops, 1.68× at 512, 1.82× at 1728, 1.71× at 4096) — small problems
give less because the fixed overhead is not amortized, but nothing drops below 1.45×.

Compare the cost of buying throughput other ways on this board: four Gemminis cost +32 %p LUT for
2 %, the 31.25→40 MHz clock bump is free for 28 %, and this is +15.5 %p LUT for **80 %**. Adding a
*second* accelerator and overlapping it beats making the array bigger — consistent with every
other measurement here showing the array is not the bottleneck.

The second accelerator does **not** have to be a different type. `Rocket64b2gem2i8f50` puts two
identical INT8 16×16 units on core 0 (custom3 + custom2) and overlaps them **better** than the
mixed pair — 89 % / 95 % / 96 % at 64³ / 128³ / 192³, i.e. 1.79–1.91×, versus 71–90 % for
INT8+FP32. Matched units interleave without either side waiting; the mixed pair has different
per-tile costs (3.12 µs vs 2.40 µs) so the faster one stalls. It is also *smaller* (39.92 % LUT,
57.33 % CLB) since FP32 8×8 costs more than INT8 16×16. The catch is timing: WNS is only
**+0.002 ns**, essentially no margin, because two 16×16 meshes stretch the critical path further
than a 16×16 plus an 8×8. Use it for single-datatype workloads; use `blf50` when you need the
precision option.

**Two is the sweet spot; three does not pay.** A three-accelerator build
(`Rocket64b2gem3i8f50`, 54.08 % LUT, 78.82 % CLB, WNS +0.021) reaches only **2.21×** at 128³
versus 1.93× for two — the second unit adds 0.93× and the third adds just 0.28× for the same
+14.16 %p LUT, a 3.3× drop in marginal return (96.5 % of ideal at two units, 73.7 % at three).
Both the on-chip fabric bandwidth and the single core's issue path cap it, exactly as the earlier
overlap experiments predicted. Four is not placeable at all (CLB would exceed 97 %). Note that
timing does *not* degrade with more accelerators — the critical path is inside one accelerator,
so three actually routed with more slack than two.

A related observation: per-tile time is 3.12 µs (INT8 16×16) versus 2.40 µs (FP32 8×8) despite a
4× difference in PEs, because a single tile is dominated by config/mvin/mvout overhead rather than
mesh compute. That is the same effect that makes big meshes lose on small matrices.

## SSGemm — 런타임 접이식 systolic array (shapeshift)

**현재 위치 (2026-09-01, E766~E807 로 갱신).** 아래 절은 오래 누적된 기록이라 철회·정정
표시가 섞여 있다. **인용할 값은 이 블록이다** — 전부 출하 경로(`grt_ss_plan_ex` 가
shape·FSM·stride·폭을 결정), 작업집합이 **모델 전체**임을 로그로 검증, 독립 배치 3 회의
배치 간 최소값, 음성 대조 포함. 분석은 `experiments/tests/analyze.py` 로 한다.

**비(比)의 범례 — 숫자를 읽기 전에 분모를 확인할 것 (E830b).** 이 절에는 네 종류의
비가 섞여 있고 형태(`1.53×`)로는 구분되지 않는다:

    ① tile / plan   타일 경로 대비 **전체 이득** (= ③ × ②)
                    -> 레이어·서빙·헤드라인 수치는 전부 이것
    ② s0 / s2       안 접은 FSM 대비 **접기만의 몫**
                    -> ops 곡선, S=4 의 K 벌금 (E811/E812/E821~E828)
    ③ tile / s0     타일 대비 **FSM 만의 몫** (E809/E810)
    ④ 두 비트스트림  기본 보드 대 SSGemm 보드 (E644 의 면적·경로 손실)

  **OOC 타이밍은 읽지 말 것 — 그 경로가 설계의 임계 경로인지 먼저 확인하라 (E840h).**
여섯 빌드 전부 임계 경로가 `AccumulatorScale` 이고 mesh 는 한 번도 아니었다. 그러므로
E837d 가 "접기의 타이밍 비용이 부분적으로 도달한다" 고 정정한 것은 **다시 철회**한다 —
움직인 것은 접기의 경로가 아니라 접기가 만든 **혼잡**이고, 그 차이는 예측을 바꾼다
(접기를 줄여야 클럭이 오르는 것이 아니라, 면적을 줄이는 무엇이든 오른다).

(옛 기록) **OOC 타이밍은 3.5 배 부풀린다 (E835 -> E837d 정정).** 접기는 OOC 에서 임계 경로를
**1.330 ns** 늘리는데(전부 route, logic 은 0), **짝 맞춘** 두 비트스트림(`max_segments`
만 다름)의 WNS 차는 **0.376 ns** 다. E835 가 처음 쓴 0.043 ns(31 배)는 버스·뱅크·nsu 가
같이 다른 짝이라 효과를 **9 배 과소평가**한 값이고 철회한다. 즉 접기는 임계 경로에
**부분적으로 도달한다** — OOC 를 0 으로 치지 말고 3~4 배 부풀려 읽을 것.
이유는 구조적이다: **면적은 더해지지만 타이밍은 `max` 다** — 모듈 면적은 전체에
기여하는 반면 모듈의 경로는 그것이 **설계의 임계 경로일 때만** 보인다. 여기서 설계의
임계 경로는 mesh 가 아니라 `AccumulatorScale` 이므로 mesh 안에서 1.33 ns 를 아껴도
클럭이 안 오른다. **OOC 타이밍은 그 경로가 설계의 임계 경로인지 확인하기 전까지 읽지
말 것** — 그러므로 `max_segments=1` 의 값어치는 면적뿐이고 클럭은 안 오른다.

**면적 귀속은 "무엇을 안 쟀는가" 부터 세라 (E836).** OOC 로 모듈을 재고 설계 수준
  증분과 비교할 때, 간극은 **척도가 아니라 누락**이기 쉽다 — LUT 은 합성이 정하지
  배치가 만들지 않으므로 배치 오버헤드로 2 배가 벌어질 수 없다. 이 트랙이 그렇게
  틀렸다: shapeshift 를 "접힌 mesh + FSM" 으로 생각해 그 둘만 OOC 로 쟀는데,
  설계 수준 계층별 리포트를 뽑아 보니 **면적의 78 %가 Scratchpad 의 뱅크별 누산기
  adder(+19,972 LUT)** 였고 mesh 쪽은 21 %(+5,299), FSM 은 1.4 %(+365)뿐이었다.
  실제로 잰 모듈은 설계 수준에서 오히려 **작다**(문맥 안에서 안 쓰는 논리가 깎인다).
  **hunk 수는 면적의 대리 변수가 아니다** — hunk 26 개짜리 파일이 5 천 LUT,
  4 개짜리가 2 만이었다. 그리고 **계층별 리포트는 모듈을 가리키지 파라미터를 가리키지
  않는다**: "Scratchpad 가 비싸다" 에서 멈추지 말고 설정을 읽어 "`acc_banks` 가 2 배라서"
  까지 갈 것 — 그 한 걸음이 후속 빌드의 설정을 바꿨다 (E836b). 배치된 `.dcp` 에 `report_utilization -hierarchical` 을
  돌리면 재합성 없이 몇 분에 답이 나온다

**①과 ②를 섞어 합성하지 말 것** — 실제로 그렇게 해서 −13~−16 % 로 빗나갔다 (E829).
③이 ops 의 함수가 아니므로 **레이어 합성은 지금 자료로 불가능하다** (E830).

**언제 쓰는가 — 조건 두 개.**

    ① 모델 _전체_ 가 L2 에 든다:  `L·(4H² + 2·H·FF) <= 512 KB`
       (FF=2H -> `L·8H²`: 4 레이어 H<=128, 12 -> 73, 24 -> 52.
        FF 비율은 이 식으로만 작용하고 그 밖에는 무관 — E802 에서 0.7 % 차)
    ② 각 단계의 `N` 이 `16·S` 로 나눠떨어진다 (트랜스포머면 N = H 또는 FF)

**얼마나 버는가 — 이득이 두 층.**

| 조건 | shape | decode 레이어 | 그중 **접기의 몫** | 확인 |
|---|---|---|---|---|
| `H % 64 == 0` | S=4 | **1.59×** | **1.24~1.52×** (모양 의존) | H=64 (E802/E810) |
| `H % 32 == 0` | S=2 | 1.29~1.38× | **1.045~1.097×** | H=32/96 (E806/E807/E809) |
| 그 외 | 접기 불가 | 1.31~1.35× | — (FSM 만) | H=48 (E800/E801) |
| 조건 ① 불만족 | — | **1.004×** (= 없음) | — | 12 레이어 H=256 (E786) |

**⚠ 단위 주의 (E829/E830): 위 표의 "decode 레이어" 는 _타일 대비_ 이고, 아래의 "접기 몫"
  과 ops 곡선은 _FSM 대비_ 다.** 둘은 `전체 = FSM 몫 × 접기 몫` 으로 이어지며 (E809/E810:
  FSM 1.13~1.34, 접기 1.05~1.52), **섞어서 합성하면 안 된다** — 실제로 그렇게 합성했다가
  두 모델에서 −13~−16 % 로 빗나갔다. FSM 몫은 ops 의 함수가 **아니므로**(단조도 아니다)
  **레이어 합성은 지금 자료로 불가능하다. 규칙으로 고르고 레이어 시간은 재라.**

  **접기 몫(FSM 대비)은 `K <= 128` 에서 `ops = N·K/256` 의 함수다 (E821/E822)**:
  ops 가 같고 (K,N)이 다른 네 점이 0.2~0.6 % 로 일치한다. 곡선은 **1.24 / 1.46 / 1.53 / 1.47 at
  ops = 16/32/64/128** 으로 **ops=64 (`K·N ≈ 16 K`)에서 최대**이고, 그 근처는
  `[128×128]`·`[64×256]` 이다. **단 S=4 는 `K >= 192` 에서 ops 곡선보다 8~9 % 낮다 (E823/E826)** — 계단이 `K` 128 과
  192 사이에 있다. **청크 가설은 반증됐다**: 같은 청크 48 에서 S=4(K=192)는 −8.4 %인데
  S=2(K=384)는 곡선과 나란하다. S=2 는 `K <= 384` 까지 일정한 오프셋(−14 %)만 있고
  계단이 없다. **기전 미상이고 S=4 에만 있다.**
  **긍정 체제(H=64·FF=2H)의 최대 K 는 128 이라 안 걸린다**; H=128 이면 FFN2 의 K=256 이
  걸리고, 그것이 4 레이어 H=128 이 12 레이어 H=64 를 못 이긴 이유로 보인다. K 벌금은 적재가 아니라
  **겹침**이다 — 적재 cyc/K 는 두 shape 이 0.3 % 안에서 같고(재배열 덕), 접힌 계산의
  **노출 비율이 13 % -> 50 %** 로 오른다(안 접은 쪽은 86 -> 71 % 로 내린다).
  E632 상주 모델은 접힌 계산이 K 와 함께 싸진다고 하므로 계산 비용의 문제가 아니다.
  **기전 미상** — 세그먼트 흩어짐·누산기 압력·겹침 입도 셋 다 기각됐다 (E813~E813d).
  관측은 "접힌 경로는 계산을 적재 뒤에 숨기는 정도가 **K=128 에서 최적**이고 양쪽으로
  나빠진다" 는 것이고, **소프트웨어로 못 고친다** — 계산 앞에 다음 블록 적재를 미리
  내는 파이프라인은 **2~7 % 느리다** (E815: RoCC 큐가 순서대로라 계산이 밀린다) (노출 0.80 / 0.35 / 2.60 cyc/K at K=64/128/256, 3/3 배치 일관).
  안 접은 팔은 단조로 좋아지므로(14.18 / 13.02 / 11.96) 비만 보면 단조 감소로 보인다.

  **접기가 값어치 있는 것은 `H % 64 == 0` 뿐이다 (E809).** `H % 32` 는 접기가 legal 해도
**5~10 % 짜리**이고, 그 모델이 얻는 것은 대부분 **FSM 인데 FSM 은 모든 H 에서 얻는다**
(`use_fsm` 은 shape 과 별개 필드). 조건 ② 를 "범위가 두 배" 로 읽지 말 것 — 넓어지는
것은 **접기가 legal 한 범위**이지 **접기가 값어치 있는 범위**가 아니다.

서빙 전체는 작은 모델 **1.40~1.59×** (decode 1.60 / prefill 1.24 합성, E777/E787),
큰 모델 **~1.00×**. 면적당(+1.96 %p LUT)은 작은 모델 **0.20~0.30 /%p** (버스 0.173 보다
낫다), 큰 모델 **0.002 /%p** (버스가 압도).

**⚠⚠ 이 절의 모든 `/%p` 수치는 분모가 `+1.96 %p` 이고, 그 분모는 뭉치다.**
E644 의 `gem16` 대 `gem16ss8b` 는 **접기 + 누산기 뱅크 + sp_banks** 를 한꺼번에 바꾼
쌍이었다. 세션 E836~E840e 가 그것을 쪼갰으므로 **아래 어디서든 `/%p` 를 인용할 때는
분모를 갈아 끼울 것:**

| 무엇을 샀는가 | 실제 면적 | 옛 분모 대비 |
|---|---|---|
| FSM 만 | 366 LUT (**0.028 %p**) | 0.01 배 |
| S=2 접기 | < ~700 LUT (**0.054 %p**) | 0.03 배 |
| **S=4 접기 (깊이 유지)** | 12,536 LUT (**0.962 %p**) | **0.49 배** |
| S=4 접기 (깊이 안 지킴) | 24,842 LUT (1.906 %p) | 0.97 배 |

즉 이 절에 적힌 **"작은 모델 0.31~0.42 /%p"·"큰 모델 0.002 /%p"·"0.038 /%p"·
"0.256 /%p" 는 전부 최소 2 배 과소평가**이고, FSM 만 사는 경우에는 **두 자릿수 배**
과소평가다. 개별 문단은 당시 측정을 그대로 두었으니 **여기서 환산해 읽을 것.**

**⚠ 그 면적당 수치는 조각들의 _평균_ 이고, 조각마다 26 배까지 다르다.** 아래는
E834~E840e 가 짝을 맞춰 다시 잰 **확정 분해**다 — 그 전에 인용되던 "shapeshift =
+1.96 %p" 는 세 가지를 뭉뚱그린 값이었다 (FSM · 접기 · **누산기 뱅크 확장**).

**네 비트스트림, 전부 라우팅 완료 failing 0, bus 256 / nsu 16 / sp_banks 8 동일:**

| config | max_seg | acc_banks | acc_kb | LUT (배치) | WNS |
|---|---|---|---|---|---|
| `...ms1f50` | 1 | 2 | 64 | 321,683 | +1.789 |
| `...ms2f50` | **2** | 2 | 64 | **321,247** | **+1.809** |
| `...ab4f50` | 1 | 4 | 64 | 338,461 | +1.933 (LUTRAM) |
| `...ab4k128f50` | 1 | 4 | **128** | **325,529** | **+1.641** |
| `...f50` (구 권장) | 4 | 4 | 64 | 345,474 | +1.413 |
| **`...k128f50`** | 4 | 4 | **128** | **333,811** | **+1.571** |

**축을 갈라 보면 면적과 타이밍의 귀속이 _반대_ 다 (E840/E840b):**

| 축 | 뱅크 2→4 (BRAM 유지) | BRAM→LUTRAM | 접기 S=1→S=4 |
|---|---|---|---|
| 면적 | +3,846 | **+12,932** | +7,013 |
| 타이밍 | −0.148 | S=1 +0.292 / S=4 −0.158 | LUTRAM −0.520 / **BRAM −0.070** |

**타이밍은 접기와 메모리 방식이 강하게 상호작용한다 (E840g, 2×2 로 확인):**

| WNS | acc 64 KB (LUTRAM) | acc 128 KB (BRAM) |
|---|---|---|
| S=1 | 1.933 | 1.641 |
| S=4 | 1.413 | **1.571** |

**패치를 워킹 트리에서 _생성_ 하는 구조에서는 트리를 되돌리면 패치도 사라진다
(E848a, 도구로 막음 E848b).** `refresh-patch.sh` 가 이제 덮어쓰기 전에
`patches/.backup/*.prev` 로 복사하고, 새 패치가 이전의 절반 아래로 줄면 **중단**한다
(음성 대조로 96→0 차단 확인). 그래도 `.backup` 은 한 세대뿐이니 두 번 연속 잘못
돌리면 잃는다. `experiments/refresh-patch.sh` 는 "트리 → 패치" 단방향이므로,
`git checkout <file>` 로 트리를 되돌린 뒤 그 스크립트를 돌리면 **빈 diff 가 패치를
덮어쓴다**. 이 저장소의 `patches/gemmini-*.patch` 는 대부분 **git 추적 밖**(`??`)이라
되살릴 수단이 없다. **되돌리기 전에 패치 파일을 먼저 복사할 것**, 그리고 여러 출처의
변경이 겹친 파일은 `git checkout` 이 아니라 **내 hunk 만 역적용**할 것.
(이번에 `gemmini-accscale-norm0.patch` 를 124→0 으로 날렸고, `patches/gemmini.patch` 에
사본이 있어 우연히 복구했다.)

**⚠ 아래 E849 의 "못 한다" 는 E851 에서 **뒤집혔다**. 기각 사유가 곧 요구사항이었다.**
메타데이터가 안 따라오는 것이 문제라면 **따라오게 하면 된다** — `Pipe(io.in, N)` 으로
메타데이터를 N 단 밀고 뒤쪽을 `Pipe(out, latency - N)` 로 줄이면 양쪽이 다 `latency` 가
되어 정렬이 지켜진다. E848·E849 둘 다 이 한 줄을 안 했다. 구현은
`ScaleArguments.pipe_split`(기본 0, **0 이면 stock 과 바이트 동일**)이고, 분할 지점은
이름을 붙일 수 있는 두 곳 — `scale_func` 뒤/포화 앞(1), 활성화 뒤/`scale_func` 앞(2).
`scale_func` **안**은 여전히 못 자른다 (주입되는 조합 함수다).

**bit-exact 는 `AccScaleEquiv.scala` 로 시뮬 검증했다 — 그리고 그 하네스가 왜 값만 보면
안 되는지를 수치로 보여준다.** ps=0 판과 ps=P 판에 같은 자극을 넣고 데이터·`index`·`id`·
`full_data` 를 함께 대조한다 (2048 자극, 지수 120~135 로 포화 양쪽 밟음):

| | badData | badIndex |
|---|---|---|
| ps1/ps2 정상 | **0** | **0** |
| 음성① 메타데이터 한 단 어긋남 (= E849 의 버그) | **0** | 1926 |
| 음성② 데이터 한 사이클 밀림 | 1014 | **0** |

**E849 의 버그는 `badData = 0` 이다** — 값은 마지막 비트까지 맞고 자리만 틀린다. 이 트랙이
쓰던 값-전용 대조(E314/E320 의 "다섯 scale, 혼합 포화, 32768 값")로는 **못 잡는다**.
두 음성 대조가 직교하는 것이 두 검출기의 독립성을 보인다.

**판정 완료 (E851f): `pipe_split = 2` 는 62.5 MHz 에서 Rocket 도메인 여유를
+0.050 -> +0.536 ns 로 열 배 넓히고, LUT 은 오히려 2,512 개(CLB 1,054 개) **줄인다.**
사이클 수도 안 변한다.** 짝 맞춘 두 비트스트림(`ms2f62` 대 `ms2ps2f62`, `pipe_split`
만 다름), 둘 다 failing 0:

| | Rocket WNS | 임계 경로 | logic | route | 단수 | LUT |
|---|---|---|---|---|---|---|
| ps0 | +0.050 | `arbOut -> pipe/io_out_pipe_b_data` | 7.364 | 8.323 | 57 | 322,466 |
| ps2 | **+0.536** | `pipe/e_act_p -> pipe/e_scaled_p` | **6.772** | 8.400 | **53** | **319,954** |

**임계 경로가 `scale_func` 구간 하나로 좁혀졌고, 거기서 더 자를 곳이 없다** — 남은 것은
주입된 조합 함수 자체다. 그 안을 자르는 것(E849 가 지목한 작업)이 **이제 유일하게 남은
RTL 클럭 지렛대**다. 여유는 주기의 0.3 % -> **3.4 %** 이고, E642 가 "닫혔는데 안 켜졌다" 를
기록한 지점이 1.0 % 였으므로 3.4 배 넓다 — **다만 타이밍 폐쇄는 충분조건이 아니고
(E642), 부팅 확인은 보드 작업이라 아직 안 했다.**

**⚠ `Intra Clock Table` 을 안 읽으면 정반대 결론이 나온다 (E313 재확인).** 설계 전체
WNS 는 ps0 0.050 / ps2 **0.043** 이라 ps2 가 나빠 보이는데, ps2 의 0.043 은
`mmcm_clkout0`(DDR4/PCIe 계열) 것이고 Rocket 과 무관하다.

**⚠ E851d 를 철회한다 — OOC 는 logic/route 분해를 _거꾸로_ 알려줬다.** OOC 세 점에서
"분할은 logic 을 못 줄이고 route 만 줄인다" 는 결론을 냈는데, 설계에서는 반대다:

    logic   OOC 6.051 -> 6.059 (불변)      |  설계 7.364 -> 6.772 (**−0.592**)
    route   OOC 7.825 -> 6.858 (−0.967)    |  설계 8.323 -> 8.400 (**+0.077**)

그러므로 E840h 의 규칙에 세 번째 조건이 붙는다: **OOC 는 경로의 존재와 순서만 말한다 —
크기도, logic/route 분해도 설계에서 다시 재라.**

(아래는 판정 이전의 기록이다.)

**OOC 로는 판정할 수 없다 (E851a).** `AccScalePipe` 를 그냥 top 으로 잡으면 사슬이 입력
포트에서 시작하는 in2reg 경로가 되어 ps0 이 **0.285 ns** 로 나온다 — 안 자른 판이 자른
판보다 45 배 빠르다는 값이다. **레지스터를 넣는 변경은 경로의 시작점을 바꾸므로 이
함정을 특히 잘 밟는다**: E840h 의 "그 경로가 설계의 임계 경로인지 확인하라" 에
**"비교하는 변형들이 같은 _종류_ 의 경로를 재고 있는지도 확인하라"** 를 더할 것.
입력을 레지스터로 감싼 `AccScaleShell` 로 통일하면 논리 단수가 설계 임계 경로의
**57 단과 일치**하고 ps0/ps1/ps2 가 13.876 / 13.521 / **12.917 ns**(57/55/54 단)로
단조롭다. 다만 **크기는 설계로 못 옮긴다** — 설계 경로는 19.017 ns(62 % route)이고
E848 이 설계에서 잰 ps1 상당 감소는 OOC 의 약 3 배였다. 62.5 MHz 판정은 짝 맞춘 두
비트스트림(`ms2f62` 대 `ms2ps2f62`, `pipe_split` 만 다름)으로 **진행 중**이다.

(아래는 E849 당시의 기록으로 남긴다.)

**62.5 MHz 는 `AccScalePipe`(stock RTL)를 _번들째_ 파이프라인해야 열린다 (E849).**
`scale_func` 은 `ScaleArguments` 로 주입되므로 서브모듈 없이 고칠 수 있어 보이지만,
계약이 **조합 함수** `(T,U)=>T` 라 **상태를 넣으면 호출자가 짝지어 둔 메타데이터가
따라오지 않는다**: `AccScalePipe` 는 `out = WireInit(io.in)` 로 index·id·full_data 를
함께 싣고 `Pipe(out, latency)` 로 번들째 지연하므로, 내부에 3 단을 넣고 latency 를
8→5 로 줄이면 **data 만 8, 나머지는 5** 가 되어 결과가 엉뚱한 자리에 쓰인다.
**upstream 이 `Pipe` 를 사슬 뒤에 둔 것은 구조적 이유가 있다.** (단서: 파이프를 넣었는데
`AccScalePipe` 의 reg 가 80→58 로 **줄었다** — 파이프 추가 시 레지스터가 줄면 무언가를
동기화에서 떨어뜨린 것이다.) 그 변경은 회계·bit-exact·`nsu=-1` 분기까지 걸리므로
**shapeshift 트랙의 곁가지가 아니라 독립 작업**이다.

**클럭을 올리려면 `scale_func` **안**을 잘라야 한다 (E848).** `AccScalePipe` 의
`e_scaled -> e_clipped`(포화) 사이에 파이프 한 단을 넣어 봤더니 경로가 18.179 ->
**16.830 ns**(WNS +1.571 -> **+2.898**), LUT 은 오히려 **−2,317** — **공짜지만 부족**하다
(62.5 MHz 는 주기 16 ns 라 여전히 불가). 기각이 위치를 알려준다: `logic` 이 7.079 ->
7.000 으로 안 줄었고 DSP 사슬과 `CARRY8` 13 개가 **둘 다 남아 있다** — 긴 사슬은
`scale_func` 안이고 포화 구간은 4 단계뿐이었다. `scale_func` 은 `ScaleArguments` 로
config 에서 주입되므로 자르려면 그 함수를 파이프라인화해야 한다.
패치는 `patches/gemmini-accscale-norm0.patch` 에 있고 `num_scale_units > 0` 경로에만
적용된다. **bit-exact 검증은 미실시** — 쓰기 전에 E314/E320 식 대조를 통과시킬 것.

**모듈까지 좁혔으면 그 안의 어느 _연산_ 인지도 경로 리포트에서 읽을 것 (E848).**
셀 종류 히스토그램(`CARRY8=13`, `DSP_MULTIPLIER` …)이 모듈 이름보다 정확한 주소다.

**⚠ 여섯 빌드 _전부_ 임계 경로가 `AccumulatorScale` 이고, 그 모듈의 Verilog 가
여섯 빌드에서 **byte-identical**(`eaed7e01`)이다 (E840h -> E847 증명).** 같은 RTL 에서
WNS 가 1.413~1.933 로 **0.520 ns** 벌어지므로, 이 절의 모든 WNS 차이는 논리가 아니라
**배치·혼잡**이다 — 그리고 LUT 과의 상관은 **r = −0.47** 로 "크면 나쁘다" 조차 아니다
(가장 큰 `ss` 가 최저 WNS). **WNS 차이를 shapeshift 설정의 성질로 인용하지 말 것.** 그러므로 이 표의 WNS 차이는 전부
**혼잡 효과**다 — 접기·뱅크·LUTRAM 이 그 경로의 _논리_ 를 바꾼 것이 아니라 주변 배치를
바꿔 남의 경로를 밀어낸 것이다. 실무적으로: **클럭을 올리려면 `AccumulatorScale` 을
고쳐야 하고**(E314/E320 이 `num_scale_units=16` 으로 한 번 했다) **shapeshift 설정은
잘못된 지렛대다.** 그리고 이 수치들은 배치 시드에 취약하니 크기만 참고할 것.

**BRAM 조건에서 접기의 타이밍 비용은 −0.070 ns — 주기의 0.35 %로 사실상 0 이다.**
LUTRAM 조건의 −0.520 은 7.4 배 크다. 그래서 초기 판단들이 차례로 쪼개졌다: "접기가
타이밍을 지배한다"(E840b)는 LUTRAM 조건 한정이고, "LUTRAM 이 0.292 빠르다"(E840f)는
S=1 한정이다. **권장 구성(`k128`)은 `ss` 대비 LUT 11,663 적고 WNS 0.158 높아 두 축 모두
이득이며 거래가 없다.** (0.070·0.158 은 주기의 1 % 미만이라 배치 시드 변동과 같은
자릿수다 — 크기는 믿되 부호 반전은 재현 확인 전까지 인용하지 말 것.)

**면적을 고치는 법: 뱅크를 늘리면 뱅크당 _깊이_ 를 지켜라 (E840c/d/e).** 총 용량을
고정한 채 누산기 뱅크를 2→4 로 늘리면 깊이가 512→256 이 되어 **BRAM 추론이 끊기고
128 RAMB18 이 통째로 10,240 LUTRAM 이 된다.** `acc_kb=128` 로 깊이를 512 로 되돌리면:

    S=4, 깊이 안 지킴 : stock 대비 **+24,842 LUT**, LUTRAM 10,240, RAMB18 0
    S=4, 깊이 512 유지 : stock 대비 **+12,536 LUT**, LUTRAM 0,      RAMB18 256

**1.98 배 싸고 누산기 용량은 2 배이며, 성능은 같다** — 두 빌드의 `ShapeshiftMesh` 와
`ShapeshiftMeshWithDelays` 가 **byte-identical** 이고, `ShapeshiftGemvLoop` 은 346 줄 중
4 줄(세그먼트 누산기 오프셋 `9'h100`→`10'h200`), `ExecuteController` 는 10,019 줄 중
72 줄만 다른데 전부 비트폭이다. **보드 없이 성능 등가를 주장할 수 있는 드문 경우다.** 대가는 RAMB18 128 개 추가(소자의 3.2 %)이고
이 보드의 천장은 CLB 이므로 명백한 이득이다. 부수로 누산기 타일이 64→128 이 되어
`I*J <= 32` 가 `I*J <= 64` 로 풀린다 ((8,8) 이 legal — 다중 가속기 트랙의 "doubled
on-chip memory" 조건과 같다). **단 이것은 뱅킹 일반이 아니라 _얕고 넓은_ 메모리의
함정이다**: 같은 설계의 스크래치패드는 16 뱅크인데 깊이 2048 이라 멀쩡하다.

**그래서 면적당 순위가 뒤집힌다 (초과이득/LUT):**

| | 이득 | LUT | /LUT |
|---|---|---|---|
| **FSM** (`ShapeshiftGemvLoop`) | 1.190× | **366** | **5.19e-4** |
| **S=2 접기** | 1.045~1.097× | **< ~700 (분해능 아래)** | > 6.4e-5 |
| **S=4 접기 (깊이 유지)** | ×1.253 | 12,536 | **2.02e-5** |
| 버스 64→256 | 1.215× | 16,166 | 1.33e-5 |
| S=4 접기 (깊이 안 지킴) | ×1.253 | 24,842 | 1.02e-5 |

**깊이를 지키기 전에는 접기가 버스보다 나빴고, 지키면 1.5 배 낫다** — 이 파일이 오래
적어 온 "접기는 버스보다 면적당 나쁘다" 는 **메모리 추론 사고를 포함한 값**이었다.

**권장은 두 단계다:**

    1. `max_segments = 2` (`...ms2f50`)  — **공짜**. `H % 32 == 0` 이면 1.045~1.097×
    2. `max_segments = 4` + `acc_kb = 128` (`...k128f50`)  — 그 위 1.253× 에
       +12,536 LUT / +128 RAMB18. `H % 64 == 0` 일 때만.

**S=2 는 왜 공짜인가 (E839b/E840).** 접기는 출력 폭을 S 배로 늘리므로 **세그먼트마다
누산기 뱅크가 하나씩** 필요하다 (`ExecuteController.scala:198` 의 require — 보수적인
게 아니라 접기의 정의다). stock 의 `acc_banks` 기본값이 **2** 라 S=2 는 **이미 지불돼
있다**. "접기는 S=2 까지 공짜" 는 접기의 성질이 아니라 **기준 하드웨어의 성질**이다.

**S=2 수치의 이식은 12 셀 격자로 확인됐다 (E838 -> E842).** shape {0,1} × T {9,17} ×
M {1,4,8,12} 를 maxSeg 2 와 4 에서 돌리면 **10 쌍 전부 `doneCyc` 가 동일**하고 전 셀이
`accBad=0` 이다. M 범위가 계획기의 shape 선택 구간(`M<=12 -> S=4`)을 덮으므로
**실제로 shape 1 이 쓰이는 곳에서 두 하드웨어가 구분되지 않는다.**
**shape 2(S=4)도 이번 RTL 상태에서 확인했다 (E845)**: maxSeg=4 에서 T{9,17}×M{1,4}
4 셀 `accBad=0`, 그리고 세 모양 열 `0,1,2`(pp 블록 8/4/2)가 `bad=0`·`checked=1536`.
음성 대조는 `bad=4`(주입한 마지막 단계만)이고 앞 두 단계의 끝 사이클이 정상판과 동일해
단계 간 오염이 없다. **접기 이득도 시뮬에서 재현된다** — S1/S4 가 1.63~2.09× 이고 T 와
함께 커진다(고정비 희석). ⚠ 이 배수를 corpus 의 상주 2.7~3.0× 와 같은 양으로 읽지 말
것: `doneCyc` 는 적재·설정을 포함한 전체 실행이다.

**다중 pp 블록도 확인했다 (E843)**: `SSExecSeqShapes` 로 shape 열 `0,1`·N=128 (pp 블록
8 개와 4 개)을 돌리면 `bad=0`, `cycles=502`, 단계별 끝 사이클(300/502)까지 동일하다.
그 격자가 단일 pp 블록만 봤던 것이 실은 **가장 갈릴 만한 축**(E632 의 pp 당 비용
`p = 8.34·(S−1)`)을 빠뜨린 것이었다. 음성 대조(`strideIsaOv` 불일치)는 양쪽 하드웨어에서 `accBad=1` 인데
**`doneCyc` 는 정상판과 똑같다** — 사이클만 봤다면 오답을 못 걸렀다는 뜻이다.

(원 관측) **S=2 수치의 이식은 시뮬로 확인됐다 (E838).** `SSExecRS` 를 maxSeg=2 와 maxSeg=4 로
두 판 지어 shape 1 을 돌리면 `doneCyc` 447 과 `accBad` 0 이 **완전히 같다**(음성 대조
`accBad=1`). mesh RTL 이 다른데도(12,771 대 22,411 줄) shape 1 의 데이터패스는 같다.
FSM 은 **byte-identical 로 `maxSeg` 무관함이 증명됐다 (E846)** — maxSeg 1·2·4 인
ms1·ms2·k128 의 `ShapeshiftGemvLoop` Verilog 가 같은 해시다. E836d 의 "366 대 370 LUT"
차이는 `maxSeg` 가 아니라 **누산기 뱅크 깊이**(512 대 256)가 세그먼트 오프셋 상수를
`10'h200`/`9'h100` 로 가른 것이고, 잡음이 아니다.

**모듈 해시를 비교할 때는 `@[file line:col]` 주석을 먼저 제거할 것 (E848a).**
Chisel 이 붙이는 소스 위치 주석 때문에, **주석 한 줄만 추가해도** 모듈 해시가 바뀌어
하드웨어가 달라진 것처럼 보인다. 실제로 잃어버린 패치를 재구성한 뒤 대조할 때
`eaed7e01` 대 `c62a640a` 로 "다름" 이 나왔는데, 주석을 떼자 양쪽 다 `22e4d004` 로
**byte-identical** 이었다. (소스가 동일한 빌드끼리의 비교는 영향 없다.)

**생성물이 같은지 먼저 보고, 다르면 그때 시뮬로 등가를 보일 것 (E846).** byte-identity
는 모든 입력에 대해 같음을 보이지만 시뮬은 돌린 자극에 대해서만 보인다. 반대로 mesh
(`ShapeshiftMeshWithDelays`)는 maxSeg 에 따라 12,771 대 22,411 줄로 **실제로 다르므로**
E842/E843/E845 의 시뮬이 꼭 필요했다.

**⚠ `max_segments` 하나가 세 가지를 켠다 (E836b/c):** ① 접힌 mesh, ② `acc_banks`,
③ GEMV FSM 의 존재. 그래서 "FSM 만" 이나 "S=2 만" 구성은 안 지어 본 것이 아니라
**지을 수 없었다**. `shapeshift_gemv_fsm` 과 `acc_banks`/`acc_kb` 인자로 분리했고,
기본값은 기존 동작을 보존한다. **결합된 파라미터는 답을 틀리게 만드는 것이 아니라
질문을 사라지게 만든다** — 축을 분리한 뒤 처음 던진 질문이 곧바로 권장 구성을 바꿨다.

**⚠ `max_segments=2` 보드는 소프트웨어를 바꿔야 한다 (E837c).** mesh 의 shape 포트가
`log2Up(segsSupported.size)` 비트라(생성 Verilog 로 확인: ms1 포트 없음 / ms2 1 비트 /
ss 2 비트) **shape 2 는 aliasing 되어 조용히 틀린 답**을 낸다 (hang 이 아니다 — E587).
그런데 `grt_ss_plan_ex` 는 `M<=12` 면 무조건 shape 2 를 고른다. `grt_ss_plan_hw(...,
max_sh)` 를 쓸 것 — 상한이 **두 곳**(M 규칙과 E638 의 "안 들어가면 더 접는다" 루프)에
필요하다. 하네스는 `SS_MAX_SH` 환경변수로 받는다.

**측정 방법에서 남길 것 (이 갈래가 같은 실수를 네 번 했다):**
- **분자와 분모가 같은 척도인가.** OOC 와 설계 수준을 섞으면 비가 3~13 배 틀린다.
  OOC 면적은 축소된 추정치로, **OOC 타이밍은 2~3 배 부풀려** 읽을 것 (통제를 더할수록
  31배→3.5배→2.6배로 줄었다).
- **하네스가 "포함하지 않는 것" 을 grep 하라.** `SSExecArea` 는 Scratchpad 를 포함하지
  않는다고 저널에 적혀 있었는데(E512) 안 읽고 300 항목 뒤에 "발견" 했다.
- **LUT 총계만 보지 말고 `LUTRAM`·`BRAM` 열을 같이 읽어라.** 두 자원이 반대로 움직이면
  메모리 추론을 의심할 것.
- **검사기가 실행됐다는 증거를 출력에 요구하라 (E838a).** `SSExecRS` 의 검사는
  `cyc === chkAt` 한 사이클에만 돌아, `doneCyc` 에서 멈추면 `accBad=0` 이 리셋값이다.
  **음성 대조는 이 함정을 못 잡는다** — 판별력을 시험하지 실행 여부를 시험하지 않는다.
- **그리고 음성 대조가 실제로 무언가를 바꿨다는 증거도 요구하라 (E843a).** "리셋을
  늦게 풀어 계산을 미완료로 만든다" 는 주입이 **아무 것도 안 바꿨다** — DUT 의 검사
  카운터가 자기 리셋 기준이라 계산과 검사가 통째로 밀렸기 때문이다. 결과가 정상판과
  **완전히** 같았던 것(사이클까지)이 단서였다. **음성 대조가 아무 것도 안 바꾸면
  "검사기가 관대하다" 가 아니라 "주입이 안 됐다" 를 먼저 의심할 것.** 밖에서
  clock·reset 만으로는 동기 설계에 데이터 오류를 못 넣고, 그래서 하네스에 `sab` 같은
  주입 파라미터가 있다 (`sab=2` = 마지막 단계에 이전 shape 을 보냄; `bad=2`, 502→638).

**넓힐 수 없다 — 네 방향에서 확인.** 배치(M=1/4/8 에서 1.005/1.005/0.999, E798),
N 패딩(적재가 1.27 배로 늘어 순손해, E804), L2 확대(GPT-2 small 이 54 MB, E753),
`K·N <= 147.5Ki` wedge 상한(H<=274 로 조건 ① 보다 3.8 배 느슨, 안 걸림 — E797).

**주의 셋.** ① 널리 인용돼 온 "큰 모델 1.065×" 는 작업집합 **레이어 두 개 분량**에서 잰
값이다 (E786). ② **접기 이득이라는 _비_ 자체가 바이너리에 따라 12 % 움직이고, 여덟 후보를
기각한 뒤에도 원인이 미상이다** (E785) — 절대값뿐 아니라 비도 범위로 말할 것.
③ 단일 셀은 **독립 배치 3 회 이상**, 레이어끼리의 _비교_ 도 마찬가지다.

**적용 범위는 넓힐 수 없다 (E798).** 배치(M)로 적재를 상환하면 계산 비중이 올라 접기가
살아날 것 같지만, 큰 모델에서 M = 1/4/8 이 **1.005 / 1.005 / 0.999×** 다 (3 배치).
이유가 같은 자료에서 닫힌다: M 을 8 배로 키워도 레이어 시간이 **1.015 배**밖에 안 늘어
노출된 계산이 전체의 **1.7 %** 뿐이고, 접기가 지울 수 있는 상한이 **1.013×** 다.
`M>=16` 은 계획기가 접기를 끄므로 **접기가 켜지는 M 구간 전체가 이 상태**다.
(배치 자체는 여전히 큰 모델의 지렛대다 — 토큰당 7.85 배. 접기의 지렛대가 아닐 뿐이다.)

**SSGemm 대 기본 Gemmini (E644, 같은 31.25 MHz, 같은 바이너리, 5 회 최소값):**
`Rocket64b1gem16` 대 `Rocket64b1gem16ss8b` — **LUT +8.7 % (+1.96 %p), 타이밍 손실 없음**
(WNS +9.182 대 +9.225, 엔드포인트는 44 % 많은데 임계 경로가 양쪽 공통인 `AccumulatorScale`),
**기본 경로 손실 없음** (tile·ws 두 경로가 두 보드에서 평균 0.22 %, 최대 0.47 % 차이 —
"shapeshift 는 stock 경로를 건드리지 않는다" 가 실측으로 확인됐고, 따라서 이 트랙의 상대
이득에 보정이 필요 없다). **주의: 이 트랙은 LLM 을 돌린 적이 없다.** 잰 것은 `[K×N]` GEMV 여섯 개를 transformer
decode 단계 **모양으로** 쌓은 것이고 attention·softmax·KV 캐시·실제 가중치가 없다. 그리고
H=128/256 은 실제 모델보다 훨씬 작다 (GPT-2 small 이 H=768, 레이어 가중치 6.75 MB =
scratchpad 의 **27 배**). 스트리밍 측정도 **K·N = 16~64 KB** 안이고 H=768 의 FFN 은
2.25 MB(36 배 밖)다 — **그 밖은 "범위 밖" 이다.**

**그리고 그 1.3× 마저 소프트웨어가 적재·계산을 겹치면 사라진다 (E658): 0.89~1.07×, 다섯 칸
중 셋에서 기본보다 느리다.** 겹침의 이득이 기본 1.30~1.49× 대 SSGemm 1.02~1.13× 인데,
**접기가 계산을 줄여 놓아 숨길 것이 없기** 때문이다(기본은 계산이 32~40 %, SSGemm 은 14~16 %).
**입도 탓이 아님이 통제 실험으로 확인됐다 (E659)**: 기본을 SSGemm 과 같은 입도로 거칠게
만들어도 기본은 겹침으로 1.12~1.47× 를 얻고 SSGemm 은 1.02~1.13× 에 머문다 (SSGemm 이득
0.95~1.19×, 중앙값 1.07×). 입도 격차가 가장 컸던 [512×128] 한 셀만 예외다.
**FSM 인터페이스를 고쳐도 달라지지 않는다.** **그리고 그 범위조차 실제로는 닿지 않는다 (E662): 상주 이득은 레이어 하나가 아니라 **모델
전체**가 256 KB 에 들어가야 성립한다** — decode 한 토큰이 모든 레이어를 지나므로. 역산하면
12 레이어 모델은 **H <= 42 (0.26 M 파라미터)** 여야 하고, 그건 어떤 실제 모델보다도 작다.
다층 모델의 decode 는 토큰마다 전 레이어를 재적재하므로 **스트리밍 체제 = 0.95~1.19×** 다.
**(철회 — E700~E736 을 볼 것.)** 이 결론은 **계획기가 스트리밍에서 잘못된 shape(S=4)을 고르고
있던 상태**에서 나온 것이다. 체제별 shape 규칙을 고치면 차가운 다층 decode 에서 SSGemm 은
**레이어 1.436×**(M=1; M=4/8 에서 1.50×), 서빙 전체 **1.27~1.43×** 다. 아래 문단들은 그
정정 이전의 기록으로 남긴다. 2.98× 가 레이어 하나를 반복해 돌린 상주 조건의 값이라는 점은
그대로 유효하다. 차가운 가중치(회전, 다층 decode 와 같은 조건) + 겹침 +
**양쪽 동일한 재배열**로 재면 SSGemm 은 **1.00× / 0.94×** 다 (E664).

**주의 (E667): 그 1.4× 는 라이브러리를 이긴 것이 아니다.** 차가운 조건에서 라이브러리
`loop_ws` 는 12,622~14,997 cyc 인데 내 행-major 손코드는 17,400 이었다 — **재배열은 내
기준선을 라이브러리 수준으로 끌어올린 것**이고, `loop_ws` 대비로는 **1.01~1.20×** 다.
(뜨거울 때 `loop_ws` 가 tile 보다 3.3 배 느렸던 것(E644)은 tile 이 적재를 안 했기 때문이고,
둘 다 적재하는 차가운 조건에서는 `loop_ws` 의 FSM 적재가 손코드보다 잘 파이프라인된다.)
**(철회)** "이 트랙에서 기본 Gemmini 를 실제로 앞선 것은 클럭 하나다" 는 잘못된 shape 으로
잰 상태의 결론이었다 — 고친 뒤에는 **decode 레이어 1.436×, 서빙 1.27~1.43×** 이고 클럭
1.67× 와 **곱해진다** (E700~E736).
**차가운 스트리밍의 최종 수치 (E676, 다섯 변형을 한 바이너리 안에서, 5 회 최소값)**:
64 KB 짜리 decode 단계가 다섯 변형 전부 **12,300~12,900 cyc = 5.2 B/cycle** 근처에 모인다.
차가운 decode 는 **대역 바운드**이고, 그 위에서 각 축의 값은:

| 축 | 이득 | 필요한 것 |
|---|---|---|
| 타일-major 오프라인 재배열 (`loop_ws` -> 손코드) | **1.17×** | 소프트웨어만, 기본 Gemmini 그대로 |
| 4-타일 wide mvin | 1.00~1.04× | 없음 — 이미 대역 바운드 |
| **접기 (양쪽 동일 소프트웨어)** | **1.02×** | +1.96 %p LUT |

즉 **(그 상태에서는) 접기가 2 % 다** — 그러나 그 다섯 변형은 모두 계획기가 고른 S=4 를
썼고, S=2 로 고치면 같은 단계가 **1.52×** 다 (E700/E701). 아래는 정정 이전 기록이다.
원래 문장: E668 의 1.03×, E674 의 0.75×, E675 의 1.17× 는 전부 **한쪽 경로만
좋은 소프트웨어를 받은 비교**였고 철회한다.

**그 2 % 의 이유는 E677 이 분해했다 — 그리고 그것은 메모리가 아니다.** 적재와 계산을 갈라
재면 계산은 1,368 cyc(전체의 11 %)뿐이고 그중 57~97 % 는 이미 적재 뒤에 숨는다. 그러니
접기가 계산을 0 으로 만들어도 상한이 **1.05×** 다. 그런데 남는 89 % 의 "적재" 는 DRAM 이
아니다: **L2 에 이미 있는 데이터를 실어도 6.38 B/cycle 이고, 차가운 경우의 5.57 과 1.14~1.20×
차이뿐이다.** 1024 바이트 wide mvin 하나가 160~184 cyc 인데 E205 가 잰 4-타일 mvin 은
~76 cyc 였다. 그러므로 **더 빠른 메모리도 더 넓은 시스템 버스도 이 시간을 못 건드린다** —
E619 의 "스트리밍은 적재가 85 %" 는 맞지만 원인이 메모리라는 읽기는 틀렸다.
**병목은 시스템 버스이고, 이 트랙은 내내 64-bit 에서 돌고 있었다 (E681).** shapeshift 설정
`WithGemminiShapeshift(16, **64**, ...)` 의 둘째 인자가 `bus_bits` 이고 `SystemBusKey.beatBytes`
가 그것/8 = **8 B** 다. scratchpad 행은 DIM×1 B = 16 B 이므로 **행 하나에 beat 두 개 = 행당
2 사이클**, 천장 **8 B/cycle**. 측정한 열 적재 7.91~7.96 은 그 천장의 **98.9~99.4 %** 다.
요청 크기가 무관한 것(E680), 명령 수가 무관한 것(E678), 열/냉이 1.5 배뿐인 것이 전부 버스
바운드의 증상이다. **`w256` 접미사가 없는 설정은 64-bit 다** — 다중 가속기 트랙의 128/256 과
헷갈리지 말 것. (E678 이 "버스를 넓히는 것은 답이 아니다" 라고 적은 것은 버스를 128 로
**기억으로** 쓴 오류이고 철회한다. 파라미터는 읽어서 확인할 것 — 이 프로젝트의 "비트스트림
서명을 찍어라" 와 같은 종류의 실수다.)

**버스를 넓히면 실제로 그만큼 나온다 — 그리고 바닥은 scratchpad 쓰기 포트다 (E685~E688).**
`Rocket64b1gem16ss8bsu16w256f50` 을 지어 같은 바이너리로 재면 (bit16, md5 2556067d):

| 순수 적재 cyc/행 | bus64 폭1 | bus64 폭4 | bus256 폭1 | bus256 폭4 |
|---|---|---|---|---|
| 열 (L2) | 2.017 | 2.622 | 1.163 | **1.008** |
| 냉 (DRAM) | 3.056 | 3.010 | 2.918 | **2.255** |

- **열의 바닥 1.008 cyc/행은 `spad_w`(=DIM×elem=16 B)와 `BeatMerger.io.out`(사이클당 한 행)
  이 정한다** — 버스만 보면 bus256 은 0.5 를 허용하는데 1.008 에 앉았다(예측 대비 +0.8 %).
  그러므로 **mvin 에는 128-bit 면 충분하고 그 위는 이 경로에 낭비다.**
- **냉의 바닥 2.26 cyc/행(7.1 B/cycle)은 DRAM 이고, 냉은 처음부터 대체로 DRAM 바운드였다** —
  버스는 열에서만 물렸다.
- 냉 decode 전체로는 **1.33 배** (12,372 -> 9,285), 대가는 **+1.70 %p LUT** (그중 버스 논리가
  +1.24 %p, 나머지는 BRAM -134 타일과 LUTRAM 의 맞바꿈), 타이밍 WNS +1.413 ns 로 여유.
- **면적당으로는 버스가 shapeshift 보다 1.8 배 낫다 (E712)**: shapeshift RTL 은 bus256 에서
  FSM 1.22 × 접기 1.23 = **+50.1 % / +1.96 %p = 0.256 /%p**, 버스는 **+58.6 % / +1.24 %p =
  0.473 /%p**. bus64 에서는 접기가 0 이라 shapeshift 가 0.112 /%p 로 떨어져 격차가 4.2 배다.
  **⚠ 이 줄의 shapeshift 값(0.256 /%p)은 폐기된 1.50× 에 근거한다** — E754 가 작업집합을
  통일해 그것을 **1.074×** 로 정정했고, E786 이 실제 모델 규모에서 **1.004×** 로 다시
  정정했다. 따라서 **큰 모델의 shapeshift 는 0.002 /%p** 이고 버스가 압도한다.
  **그러나 순서 권고는 체제 조건부다**: 모델 전체가 L2 에 드는 작은 모델에서는
  shapeshift 가 **0.31~0.42 /%p** 로 버스(0.173)를 **앞선다** (E777/E787).
  정리하면 **큰 모델 -> 버스만, 작은 모델 -> 접기가 먼저**이고,
  "버스 없이는 접기가 0" 이라는 곱셈 관계는 두 체제 모두에서 유효하다. 주의: 성능은 FSM 과 접기로 나뉘지만 **면적은 나뉘지 않는다** — `SS_GEMV` 도
  같은 패치 안에 있으므로 둘을 같이 세야 한다.
- **차가운 스트리밍(= 실제 서빙)의 최종 수치 (E745, 작업집합 통일)**:
  decode 레이어 **1.082×**, 배치 decode(M=8) **1.036×**, prefill(M=16) **1.007×**,
  서빙 전체 **1.044× (seq 1024/gen 64) ~ 1.082× (64/1024)**. 대가는 +1.96 %p LUT 이므로
  **면적당 값어치가 낮다**. **⚠ 이 서빙 수치는 decode 1.082 에서 파생된 것이고, 그
  1.082 는 작업집합 2×L2 의 값이다 — 실제 12 레이어 모델(12×L2)은 decode 1.004 이므로
  서빙도 ~1.00× 다 (E786/E798).** **기전 (E746): 차가운 스트리밍에서 노출된 계산이 15~322 사이클,
  곧 전체의 0.2~3.4 % 뿐이다** — 계산이 적재 뒤에 거의 완전히 숨으므로 접기가 줄이는 것이
  보이지 않는다. shape 순위는 배치 간 최소값으로 보면 **S=4 < S=2 < S=1** 로 상주와 같지만
  마진이 1.5 %/5.4 % 로 작다.
  **접기의 값어치는 가중치가 L2 에 맞는가로 결정된다 (E749).** 회전 작업집합만 쓸면
  ([256×256], 양쪽 팔 동일):

  | 작업집합 | 128 KB | 256 KB | 512 KB(경계) | 1024 KB |
  |---|---|---|---|---|
  | [256×256] | 1.518× | 1.534× | 1.455× | **1.078×** |
  | [256×512] | — | 1.521× | 1.468× | **1.096×** |

  | [256×384] | — | 1.506×(480 KB) | — | **1.110×**(960 KB) |

  **세 모양 · 사본 64/96/128 KB · nrot 2~16 에서 확인됐고, 세 번째는 사전등록 표본 밖
  시험이었다 (E750/E752)**: L2 안 1.506~1.534×, L2 밖 1.078~1.110×.

  (원문) **두 모양이 0.8~1.7 % 안에서 겹친다 (E750)** — 같은 작업집합에 **다른 nrot** 으로 도달하는데
  (4/8/16 대 2/4/8) 곡선이 같으므로, 변수는 사본 수도 K·N 도 아닌 **L2 대비 작업집합**이다.

  **전이가 가파르고, 무너지는 쪽은 접힌 팔이다** (6,055 -> 8,965 = 1.48 배 느려짐; 기본 팔은
  1.10 배). 접기는 계산을 싸게 만들어 시간이 적재에 지배되므로 DRAM 으로 넘어가면 타격을
  온전히 받는다. **한 줄: 가중치가 L2 에 맞으면 약 1.5×, 2×L2 를 넘으면 약 1.08×**
  — **단 이 곡선의 모양은 폭에 달렸다 (E773/E774)**. 작업집합 0.5/1/2/3×L2 에서
  폭 1 은 1.35 / 1.05 / 1.00 / 1.04 로 **L2 를 넘는 순간 끝나고**, 폭 4 는
  1.39~1.84 / 1.38~1.50 / 1.16 / **1.040** 으로 **비탈**이다 (1.5×L2 는 폭 1 **1.029** /
  폭 4 **1.343**, 표본 밖 예측 3/3 적중 — E775). **3×L2 에서는 폭도 접기도 죽는다**
  (폭 1 1.014, 폭 4 1.040, 폭의 값 1.026 — E781 이 E774 의 1.038/1.112/1.072 를 정정.
  원인은 `FORCE_NROT` 뒤에 있던 조용한 clamp 였다). **위 곡선(1.5× / 1.08×)은 폭 4 의
  것이다** (E753 의 768 KB 값 1.230 이 폭 4 밴드 안, 폭 1 밴드 밖 — E775 로 역추정). "절벽" 은 폭 1 의 그림이고
  권장 설정에서는 비탈이다. 아래 E758 의 H 경계(H≈64~73)도 **폭 1 에서 잰 것**이며,
  폭 4 로 재현하면 3×L2 의 바닥이 1.00 이 아니라 **1.04** 다 (E781; E774 의 1.11 은
  작업집합 혼입이었다) — 즉 **폭은 L2 밖을 구제하지 못한다.** 결론(작은 모델용 가속기)이
  오히려 강해진다. — 전이는 급격하지 않고
  **L2~2×L2 의 띠**에서 완만하다 (512/640/768/1024 KB 에서 1.497/1.457/1.230/1.071×, E753).
  **거꾸로, L2 에 들어가는 작은 모델에서는 접기가 크게 산다 (E755/E756)**: H=64·FF=2H·12
  레이어(총 384 KB)를 384 KB 작업집합으로 재면 decode 레이어 **1.83×** (E755 의 1.836 은
  `[64x64]` 칸이 ss 팔만 256 KB 로 돌아 약 0.3 % 부풀어 있었다 — E781/E782) (단계별 1.790 /
  1.948 / 1.835×) — 곡선의 "L2 안 1.5×" 보다도 높다. 작은 모양일수록 계산 비중이 커지기
  때문이다. **면적당으로 보면 체제가 답을 뒤집는다**: L2 밖 **0.002 /%p**(실제 모델 규모,
  E786; 2×L2 작업집합으로 재면 0.038) 대 버스 0.173 /%p, **L2 안 0.31~0.42 /%p**
  (decode 기준; 서빙 기준은 0.20~0.30 — 버스 0.173 보다 낫지만 격차는 2.5 배가 아니라
  **1.2~1.7 배**다, E787).
  **shapeshift 의 적용 조건은 "모델이 L2 에 들어가는가" 이고, 그 하나로 값어치가 11 배 갈린다.**

  | | 큰 모델 (L2 밖) | 작은 모델 (L2 안) |
  |---|---|---|
  | decode 레이어 | **1.004×** (12×L2, E786) · 1.082× (2×L2) | **1.60~1.82×** |
  | prefill 레이어 | 1.007× (2×L2) | **1.24~1.36×** |
  | 서빙 전체 | **~1.00×** (실제 모델) | **1.40~1.59×** (출하 경로) |

  **왼쪽 열의 1.08 / 1.007 은 작업집합 1024 KB = _레이어 두 개 분량_ 에서 잰 값이다
  (E786).** 실제 12 레이어 H=256 모델은 6 MB = 12×L2 이고 거기서는 **1.004×, 즉 없다.**
  큰 모델 쪽 수치를 인용할 때는 **모델 전체인지 레이어 몇 개분인지** 먼저 확인할 것 —
  decode 는 토큰마다 전 레이어를 지나므로 후자는 실제 서빙을 대표하지 않는다 (E662).

  **규약 적용 확정값 (E764/E765/E777/E786)**: 큰 모델 — **실제 12 레이어 H=256 모델
  (6 MB = 12×L2)에서는 1.004×, 즉 없다** (E786). 널리 인용돼 온 **1.065× 는 작업집합
  1024 KB = 레이어 두 개 분량**에서 잰 값이고 모델 전체가 아니다 (E662 와 같은 형태의
  실수 — decode 는 토큰마다 전 레이어를 지난다). 작은 모델
  **1.60~1.82×** — 범위인 이유는 바이너리 효과다(E626): 같은 12 레이어 H=64 모델이
  `sspair_SM` 에서 1.823, `sspair_V3` 에서 **1.601** 이다(각각 3 배치). **흔들리는 것은 절대값이 아니라 _비_ 다** — 접힌 팔만 12 % 갈리고 기본 팔은 0.4~0.9 %
  로 같으므로 **접기 이득이라는 비 자체가 1.60~1.82 로 움직인다** (E785). 이 프로젝트의
  "비를 인용하라" 가 통하지 않는 사례다. 기각된 후보 다섯: 작업집합 불일치(고침, 0.3 %),
  배열 크기(0.3 %), argv(<1 %), 정적 배치(0.28 %), 계획기 선택(동일). 세션도 아니다
  (한 배치 안에서 재현). **원인 미상이고 범위는 줄지 않았다.** **그 차이는 접힌 팔에만 있다 (E778; 축이 바이너리 하나뿐이라 잠정)** — argv 는 교란원이
  아니다(E784: 여섯 조합 전부 배치 간 최소값 기준 3 % 이내, 양성 대조 포함). **주의:
  같은 자료의 _가공 전_ 폭은 11~49 % 이고 팔을 가르지도 못한다 — 최소값 규약이 적용된
  수치와 안 된 수치를 섞어 읽지 말 것 (E776 의 "argv 76 %" 는 그 혼동이었고 철회).** —
  같은 셀·같은 작업집합에서 기본 팔은 두 바이너리가 0.4~0.9 % 로 같고 접힌 팔만
  11~14 % 갈린다. **정적 배열 크기는 원인이 아니다** (4 배 바꿔도 0.3 %). 그러므로
  분모(기본 경로)는 바이너리를 넘어 믿을 수 있고, 흔들리는 것은 `grt_ss_gemv`/FSM 쪽이다. 그리고 **출하 경로(`plan --pw`, shape·FSM·
  stride·폭을 전부 계획기가 결정)가 손 튜닝을 0.08 % 로 재현한다** (E777). 레이어 수치는 배치 간 폭이 1.8 % 로 견고하고(6 단계 합이 이상치를 희석),
  단일 셀은 8.1 % 로 이봉이다.

  **분석은 `experiments/tests/analyze.py` 로 한다 (E803)** — 두 팔의 작업집합 불일치,
  의도한 크기와의 차이(`--expect-kb`), 그리고 **그 셀이 접기 가능한 모양인지**(`N % 16S`)를
  자동으로 찍는다. 이 세션에서 그 셋을 각각 놓쳐 실험을 셋 버렸다.

  **주의 (E763/E764)**: 단일 셀 수치는 **독립 배치 3 회 이상의 최소값이 필수**다. 8 배치를
  재면 접힘이 **이봉**이고(빠른 6 개 8998~9158, 느린 2 개 9665~9725, 사이 5.5 % 가 비어 있다)
  기본도 1/8 이 이상치다 — **둘 다 같은 형태**이고 접힌 쪽이 더 자주(25 % 대 12.5 %) 나올 뿐이다.
  배치 하나만 보면 같은 셀이 **0.994× ~ 1.125×** (13 % 폭, 부호까지 바뀐다). 느린 모드가 25 %
  이므로 3 배치가 모두 느릴 확률은 1.6 % 다. **8 배치 최소값 기준 최종값은 1.065×.** 재부팅 후 한 배치만
  보면 접힘이 느린 모드에 떨어져 **0.986×(접기가 지는 것처럼)** 나온다. **레이어 수치(6 단계
  합)는 1.8 % 로 견고하다** — 이상치가 희석되기 때문이다.
  **그러나 그 견고함은 _레이어 두 개의 차이_ 로 넘어가지 않는다 (E768/E769).** 레이어 하나가
  1.8 % 로 재현되면 두 레이어의 **차이**는 약 2.5 %p 의 잡음을 진다. 이 트랙이 그 구멍으로
  빠졌다 — 한 배치에서 잰 **4.8 %** 차이를 결론(조건 ② 철회의 근거)으로 삼았는데 3 배치를
  돌리자 **0.3 %(동률)** 이 되어 그 방향 주장을 철회했다. **단일 셀에 배치 3 회를 요구하는
  규칙은 레이어끼리의 _비교_ 에도 그대로 걸린다.**
  **그리고 예측·판정 기준의 정밀도를 잴 수 있는 것보다 높게 잡지 말 것 (E776).** 두 경로가
  같음을 보이려고 "1 % 안" 을 걸었는데, 그 셀은 **같은 코드의 세 호출이 76 % 를 오가는**
  칸이었다(E693 의 argv 효과). 등가 시험은 **그 셀의 잡음 바닥을 먼저 재고** 바를 세워야
  하고, 별칭-최소값(E694)이 그 용도다. 레이어 합이면 2 % 가 적당하다.
  다중 가속기 쪽에 이미 같은 문장이 있다
  ("두 최소값의 차이는 ~0.9 %p 의 잡음을 진다") — 축만 다르고 형태가 같다.
  | 면적당 (+1.96 %p LUT) | **0.002 /%p** (실제 모델; 2×L2 로 재면 0.038) | **0.31~0.42 /%p** (버스는 0.173) |

  **두 효과의 대칭 분해 (E754/E762)**: FSM 1.029× / 접기 1.044× (**작업집합 2×L2**,
  실제 큰 모델이 아니다 — E786) 대
  FSM **1.374×** / 접기 **1.322×** (작은 모델) — **두 효과가 체제와 함께 같이 커지고 어느
  하나가 지배하지 않는다.** 둘 다 "명령/계산이 시간의 몫을 차지할 때만 산다" 는 같은 조건에
  걸려 있다. 곱(1.816×)이 레이어 측정과 **정확히 일치**한다 — **단 그 레이어 측정은
  옛 바이너리(`sspair_SM` 계열)의 것이다.** 출하 경로·검증된 조건에서는 같은 모델이
  **1.60×** 이므로(E777), 이 분해의 두 인자도 그만큼 낮을 것이다 — **재측정 안 했으므로
  곱만 인용하고 인자는 인용하지 말 것.** (비가 바이너리에 12 % 흔들리는 이유는 E785.)
  **shapeshift 는 작은 모델 전용 가속기다 (E761).** prefill 조차 1.357× 인데, M>=16 이면 접기는
  꺼지고 FSM 만 남는데도 그렇다 — 작은 모양에서는 타일 경로가 `ch·nt` 개의 tile-op 를 발행하는
  반면 FSM 은 명령 하나이므로, **명령당 고정비가 지배하는 작은 모양에서 FSM 이 크게 산다**
  (2×L2 작업집합에서는 1.007×; 실제 12×L2 모델은 미측정이나 decode 가 1.004 이므로 그 이하로
  볼 것). E759 의 폭 비대칭과 같은 원리다.
  **조건은 H 가 아니라 모델 전체 크기다 (E766)**: 같은 H=128 이 12 레이어(3×L2)에서는
  **1.000×** 인데 4 레이어(1.0×L2, 폭 4)에서는 **1.730×** 다. 진짜 조건은

      ① `L·8·H² <= L2` (4 레이어 H<=128, 6 -> 104, 12 -> 73, 24 -> 52)
      ② ~~`H % 64 == 0`~~ — **철회 (E768)**. 폭 4 자체는 실재하고 같은 셀에서 통제해 재면
         이득을 1.339× 키우지만(1.730 -> 1.292, E767), **실제 모델에서는 무관하다**:
         64 의 배수가 아닌 H 는 대개 더 작고, 작은 모양은 계산 비중이 커서 접기 이득이
         크다 — 두 효과가 상쇄한다. **독립 배치 3 회의 배치 간 최소값**으로 4 레이어
         H=96(폭 1 이 세 단계 중 둘) **1.504×** 와 4 레이어 H=128(전부 폭 4) **1.499×**
         가 **동률(0.3 %)** 이다. 조건 ② 는 34 % 의 격차를 예측했으므로 **큰 효과로서는
         반증**이고, 몇 % 의 잔여 효과가 있는지는 미확정이다. (한 배치만 보면 1.532 대
         1.462 로 5 % 차가 보이는데 그것은 이상치다 — E769.)
         **한 축만 통제해 얻은 크기를, 그 축이 다른 축과 묶여 있는 실제 조건에 옮기지 말 것.**
         각 모델 안의 단계 순위도 폭이 아니라 **K** 를 따른다 (K 가 클수록 이득이 작다).
         **절대값은 바이너리 조건부다** — 같은 H=128 셀이 두 바이너리에서 1.730× 와
         1.462× 다 (E626 현상). 비를 인용할 때 어느 바이너리인지 같이 적을 것.

  **모양 크기 자체는 거의 무관**하다 — H=64 에서 1.823×, H=128 에서 1.730× 로 5 % 차이다.
  **단 단계별로는 K 가 접기 이득을 가른다 (E768~E771)**: 작업집합과 폭을 고정하고 K 만
  쓸면 이득이 **단조로 준다** — 폭 1·N=128·~500 KB 에서 K = 96/128/160/192/224/256 이
  1.262 / 1.142 / 1.146 / 1.067 / 1.091 / 1.048× 로, **K=256 이면 접기가 사실상 0 이다**.
  `K·N` 도 `N` 도 `nt = N/(16S)` 도 못 가르고 **K 만 가른다** (K·N 이 1.8 배 달라도 K 가
  같으면 이득이 같고, `nt=2` 에 최고와 최저가 같이 들어 있다).
  **계단인지 매끈한 추세인지는 이 잡음으로 가릴 수 없다** — 점마다 배치 간 폭이 최대
  10 % 인데 무리 사이가 12 % 다. 답을 고르지 말 것.
  **그리고 K 효과는 한 가지가 아니다 (E794)**: 같은 K 축을 **상주**로 재면 비가 3.168 /
  3.042 / 2.908 / 2.979 (K=96/128/192/256)로 **8.9 % 만** 움직이고 비단조다. 스트리밍의
  20.4 % 중 일부는 계산 쪽인데, **그 계산 쪽 K 의존은 접힌 경로가 아니라 기준선(타일
  경로)에 있다 (E794b)** — 접힌 팔은 E632 모델이 0.4~3.0 % 로 맞히는 선형이고, 타일
  팔의 기울기가 K>192 에서 22 % 꺾인다. 나머지가 적재·겹침이다.
  실제 워크로드는 스트리밍이므로 **인용할 수는 20 %**.
  **그리고 그 20 % 는 접기가 나빠지는 것이 아니라 기준선이 좋아지는 것이다 (E794c)**:
  K 로 정규화하면 기본 팔이 26.93 -> 21.98 cyc/K (**1.225 배** 빨라짐)인 동안 접힌 팔은
  21.34 -> 20.98 (**1.017 배**)뿐이고, **1.225/1.017 = 1.204** 가 비의 하락
  (1.262/1.048 = 1.204)과 소수점 셋째 자리까지 같다. 접힌 경로는 작은 K 에서 이미 효율적이고, 큰 K 에서는
  기준선이 스스로 따라온다. **큰 K 에서 접기를 피할 이유는 없다 — 손해가 아니라 이득이
  없을 뿐이다.** **단 그 "K 불변" 은 K >= 96 에서만 참이다 (E795)**: 아래로 나가면 접힌
  팔도 21.45 -> 22.73 -> 23.78 cyc/K 로 올라 이득이 **K = 48~96 에서 1.25× 천장**을 친다
  (1.265 / 1.174 / 1.243, 비단조 — 3/3 배치에서 재현되는 **구조적** 골이다). 여섯 점이
  일렬이라고 법칙은 아니었다. **그 골도 기준선 탓이다 (E795b)**: 기본 팔의 cyc/K 개선이
  48->64 에서 1.127 배였다가 64->96 에서 **1.000 배로 멈추는** 반면 접힌 팔은 전 구간
  매끄럽다. **비가 이상한 모양이면 두 팔을 따로 정규화해서 볼 것.**
  **폭과 K 는 직교하는 두 곱셈 인자다 (E772, 2×3 격자, 사전등록 적중)**:

      접기 이득 ~= f(K) x w      f(128)=1.14, f(192)=1.07, f(256)=1.05
                                 w = 1.00 (폭 1) / **1.34** (폭 4)

  폭의 값이 세 K 에서 1.388/1.324/1.319 로 5.3 % 안이다. **단 작업집합과는 직교하지
  않는다 (E773)** — K=256 에서 폭의 값이 작업집합 256/512/1024 KB 에 1.032 / 1.312 /
  1.156 로 **L2 경계에서 봉우리**를 이룬다 (L2 안이면 적재가 이미 싸서 고칠 것이 없고,
  2×L2 면 대역 바운드라 명령을 줄여도 덜 듣는다). **그래도 결정은 안 바뀐다** — 폭의
  값이 어디서도 1.0 미만이 아니므로 `use_wide` 는 `K % 64 == 0` 하나로 충분하고
  계획기에 작업집합 인자가 필요 없다. 그러므로 **`K % 64 == 0` 이면
  반드시 폭 4 를 쓸 것** — K 가 커서 접기가 거의 죽는 K=256 에서도(폭 1 은 1.048) 폭 4 면
  **1.382×** 로 살아난다. **큰 K 에서 접기를 살리는 것은 폭이다.**
  작업집합 축에서도 같다 — 폭 1 에서 접기가 1.346 -> 1.051 -> **1.002** 로 무너지는
  구간에서 폭 4 는 1.389 -> 1.379 -> 1.158 로 **512 KB 까지 전혀 안 무너진다**. 이 파일이
  "작업집합이 지배 변수" 라고 적은 것은 **폭 1 조건에서 훨씬 극적**이다. 이것은 위의 "H 가 64 의
  배수인지는 무관" 과 모순이 아니다 — **모양을 고정하면 폭이 중요하고, 모델을 바꾸면 K 가
  같이 움직여 상쇄한다.**
  적재는 여기에 무관하고(종횡비 R 비 1.006, 3 배치) 차이는 전적으로 **접힌 팔의 노출된
  계산**에 있다 — 즉 겹침의 문제다. 기전 미상.
  **H 축으로 실측한 경계 (E758, 12 레이어 모델 전체를 작업집합으로, 폭 1 통일)**:
  H=64(0.75×L2) **1.477×**, H=96(1.69×L2) **1.120×**, H=128(3.00×L2) **1.000×** —
  E753 의 산수 `H<=73` 이 확인된다. **H>=128 에서는 접기가 정확히 아무 것도 못 한다.**
  (H=64 는 폭 4 로 재면 1.836× 이므로 절대값은 폭에 달렸고, 경계의 위치는 안 달렸다.)
  **긍정 체제에서는 반드시 폭 4 로 실을 것 (E759)**: 폭 4 는 기본 팔을 1.16× 빠르게 하는데
  접힌 팔은 **1.40~1.50×** 빠르게 한다 — 접힌 배치는 세그먼트마다 따로 실어 **명령 수가
  S 배**이므로 명령을 4 배 줄이는 효과가 S 배 크다. 접기 이득이 1.46~1.52× -> **1.75~1.96×**.
  단 `rpb % 64 == 0` 이어야 하며(E715), `[96×96]` 처럼 안 맞는 모양은 폭 1 로 떨어져 이득의
  3 분의 1 을 잃는다.
  **그리고 L2 를 키워서 해결할 수 없다**: 12 레이어 transformer(FF=2H, INT8)가 `96·H²` 바이트
  이므로 L2 2 MB 라야 H<=147, 8 MB 라야 H<=295 이고 GPT-2 small(H=768)은 54 MB 다.
  H=256·FF=2H 한 레이어가 약 512 KB 로 정확히 경계에 있어, 한 레이어 반복 벤치마크는 1.5× 를
  실제 12 레이어 decode 는 1.08× 를 본다.

  **지배 변수는 "적재가 L2 에 맞는가" 다 (E747/E748)**: 같은 [256×256] 에서 매 토큰 DRAM
  재적재 **1.082×**, 적재 1 회(뜨거움)+1 토큰 **1.644×**, +64 토큰 2.927×, 완전 상주 3.04×.
  **T 를 64 배 늘려 얻는 것이 1.78 배인데 온도 하나가 1.5 배다.** 한 레이어의 가중치(약
  512 KB)가 L2 에 딱 맞으므로 **한 레이어를 반복하는 벤치마크는 뜨겁고** 12 레이어를 순회하는
  실제 decode 는 매번 차갑다 — 이 트랙의 옛 2.9~3.2× 가 전부 전자의 조건이었다.
  **측정 규칙이 한 단계 더 올라간다: 재실행 최소값 -> 별칭 최소값 -> _배치 간_ 최소값.**
  접힌 shape 은 네 배치 중 한 번 꼴로 7.7 % 높은 이상치를 내고, 한 배치만 보면 가짜 순위
  역전을 본다 (E744 가 그렇게 읽었다). 상주 조건의 2.98× 는 유효하나 모델 전체가 256 KB 에 들어가야
  하고(H<=42, E662) 실제 모델은 해당하지 않는다.
- **⚠ 이 절의 옛 스트리밍 수치는 작업집합 혼입으로 과대평가였다 (E742/E743).**
  두 비교 팔이 회전 사본 수를 각자 정해 **접힌 팔이 절반 작업집합(512 KB = L2 경계)** 에서
  돌았다. 양쪽을 1024 KB 로 통일해 다시 재면 **레이어 접기 이득 1.517× -> 1.080×** 다
  (단계별 1.074 / 1.095 / 1.079×). E700~E736 의 스트리밍 규칙·수치는 전부 그 조건에서 나온
  것이므로 **재도출 대상**이다. 아래 수치들은 정정 이전 기록으로 남긴다.
- (정정 전) 서빙 수준 수치 (E735/E736):
  decode 레이어 **1.436×** (M=1; M=4/8 에서도 1.50× 로 평평), prefill 레이어 **1.140×**
  (M=16, 접기 0 이므로 FSM 만). 서빙 전체는 **1.269× (seq 1024/gen 64) ~ 1.434× (64/1024)**.
  배치는 그보다 큰 지렛대다 (토큰당 M=1 54,252 -> M=8 6,558 cyc, **8.3×**, 레이어 시간이
  M 에 평평하므로 사실상 공짜).
- **공정 조건의 분해 (E754): FSM 1.029× × 접기 1.044× = 1.074×** — E710 의 "FSM 1.214 ×
  접기 1.248" 은 작업집합 혼입이었다. 곱이 E743 의 레이어 이득과 정확히 일치한다.
  **면적당(작업집합 2×L2 기준): shapeshift +7.4 %/+1.96 %p = 0.038 /%p, 버스
  +21.5 %/+1.24 %p = 0.173 /%p -> 버스가 4.6 배 낫고, 소프트웨어 재배열(1.160×, 면적 0)이
  가장 낫다.** **실제 모델 규모(12×L2)에서는 shapeshift 가 0.002 /%p 로 더 떨어지고
  (E786), 반대로 모델이 L2 에 들면 0.31~0.42 로 버스를 앞선다 (E777/E787).**
  (재배열 수치는 혼입 없음 — `ws`/`tile` 이 둘 다 Wrot/Wtm 을 같은 nrot 으로 쓴다.)
- (정정 전) **접기 고유의 몫은 1.248× 이고, 나머지는 FSM 의 몫이다 (E710).** 세 shape 을 강제해
  분해하면:

  | | bus64 | bus256 |
  |---|---|---|
  | FSM 효과 (tile -> FSM S=1) | 1.219× | 1.214× |
  | **접기 효과** (S=1 -> 최적) | **1.000×** (S=1 이 최적!) | **1.248×** |
  | 합 | 1.219× | 1.515× |

  **FSM 효과는 버스에 무관하고(1.214~1.219×), 접기 효과만 버스에 달렸다.** bus64 스트리밍
  에서는 **안 접는 것(S=1)이 최적**이라 접기가 0 이고, bus256 에서 비로소 1.248× 가 드러난다 —
  적재가 지배하는 동안에는 계산을 줄여도 숨을 뿐이기 때문이다.
  **두 하드웨어를 비교할 때는 각 하드웨어의 최적 설정끼리 비교할 것**: 그렇게 하면 버스
  64->256 이 **1.586×** 이고, E690 이 적은 1.309× 는 양쪽 다 S=4(각 보드에서 최악에 가까운
  shape)로 잰 값이었다.
- **그 위에서 접기는 1.02× -> 1.517× 로 오른다 — 단 shape 을 S=2 로 골라야 한다
  (E700/E701/E709). 이 1.517 은 FSM 1.214 × 접기 1.248 이다.** 출하된 계획기를 그대로 따른 최종 측정(세 모양 한 배치, 별칭 대조군
  0.997~1.033, 음성 대조 3/3 FAIL): 단계별 1.522 / 1.542 / 1.484×, 레이어 **1.517×**
  (50 MHz 기준 1.607 -> 1.059 ms). 사슬 전체는
  `loop_ws` 93,138 -> 재배열 80,338 (1.160×) -> 접기 52,955 (1.517×) = **1.759×**.
  `grt_ss_plan_ex(..., streaming=1)` 이 그 규칙을 담고 있고, 강제 최적값과 **±1.1 %** 로
  일치한다. **폭도 계획기가 정한다 (E776)** — `use_wide` 는 오랫동안 계산만 되고 아무도
  안 쓰는 죽은 필드였고(측정은 `--wide` 를 손으로 줬다), 이제 `sspair --pw` 로 배선돼
  손으로 준 것과 폭 4 셀에서 **0.6 % 안**으로 같다. 규칙을 고칠 때는 **독립적으로 강제한 최적값과 대조할 것** — 계획기가 세 군데서
  불려 한 곳만 고쳤을 때 숫자가 전혀 안 움직였고, 강제값이 없었다면 "체제는 상관없다" 로
  잘못 결론지었을 것이다 (E703).
  **`grt_ss_plan` 의 M 규칙(`M<=12 -> S=4`)은 상주 전용이고 차가운 스트리밍에서는 틀리다**:
  강제 측정에서 [256×256] M=1 이 S=2 6,520 / S=1 8,137 / **S=4 9,200** 으로, 계획기가 고르는
  S=4 가 셋 중 가장 나쁘다 (M=8 도 같다). CLAUDE.md 에 이미 "스트리밍에서 M=8 의 최적은
  S=2 로 상주와 반대"(E605)가 적혀 있었는데 스트리밍 측정에 계획기를 그대로 쓴 것이 원인이다 —
  **측정 도구가 어느 체제의 규칙을 담고 있는지 먼저 확인할 것.** 계획기를 따라 잰
  1.07~1.10× 값들(E689/E694/E697/E699)은 **잘못된 shape 의 하한**으로 읽을 것. decode 레이어 전체(H=256, FF=2H,
  Q/K/V/O + FFN1 + FFN2)를 조립해 재면 **82,792 -> 75,002 cyc = 1.104×** (50 MHz 기준
  1.656 -> 1.500 ms), 단계별로는 1.116 / 1.107 / 1.077× 다 (E689). 접힌 계산 1,365 는
  완전히 숨고 기본 계산 4,122 는 27 % 가 노출되는 것이 기전이다. **접기의 값어치는 적재를
  얼마나 고쳤는지에 비례한다** — 적재를 1.33 배 줄이자 접기 몫이 2 % -> 10 % 로 커졌다.
  상주 조건의 2.98× 와는 여전히 멀고, 그 차이는 전부 "적재가 시간의 대부분" 에서 온다.
- **레이어 수준 버스 이득은 경로마다 다르다 (E690, 두 비트스트림·한 바이너리·폭 4 통일)**:
  **접힘 1.309×, 기본 1.215×**. 접힘은 사실상 적재 그 자체이고(적재+22 / +47), 기본은
  고정된 계산 4,122 중 노출분이 적재가 줄수록 **커지기** 때문이다(적재+287 -> +1,122).
  버스는 적재 바운드인 쪽을 더 돕는다.
- **면적당으로는 버스가 접기보다 약 4 배 낫다**: 버스 1.215~1.309× 에 +1.24 %p, 접기
  1.104× 에 +1.96 %p. 둘은 곱해져 **100,567 -> 75,002 cyc = 1.341×** (+3.2 %p LUT) 가
  이 트랙이 차가운 decode 레이어에서 얻은 전부다.
- **`mvin` 폭의 최적은 버스 폭에 뒤집힌다**: bus64 에서는 폭 1 이 옳고(폭 4 가 열에서 25~30 %
  손해), **bus256 에서는 폭 4 가 옳다**(열 15 %, 냉 23 % 이득). 한 축의 최적을 다른 축에서
  그대로 쓰지 말 것.
- **비트스트림 서명**: 디바이스 트리의 `sifive,mshr-count` 가 bus64 는 **7**, bus256 은 **22**
  다. `/proc/device-tree/cpus/timebase-frequency` 는 클럭×100 이므로 둘을 같이 읽으면
  클럭과 버스 폭이 한 번에 찍힌다 — 알려진 워크로드로 서명 찍는 것보다 값싸고 모호하지 않다. E205 의 "`MAX_BLOCK_LEN` 만큼 실어라" 도
**명령 바운드일 때만** 유효하다는 것이 여기서 확인된다 — 이미 연속으로 읽고 있으면 명령을
4 배 줄여도 0 이다.

**측정 규칙이 하나 늘었다 (E674 -> E676): 최소값을 재실행 축뿐 아니라 *바이너리 축*에도
적용하라.** 같은 `ss` 경로가 무관한 코드를 더한 것만으로 22,092 와 12,862 사이를 오간다
(**1.72×**). E674 는 `-O1/-O2/-O3/-Os` 네 판이 0.3 % 안에서 일치하는 것을 근거로 느린 쪽을
채택했는데, **같은 소스에서 나온 넷은 독립 증거가 아니다** — 일치는 정확성이 아니라 결정성만
말한다. 교란은 한 방향이므로(E312) 최소값이 옳고, 버렸던 바이너리가 나중에 만든 바이너리와
0.6 % 안에서 일치하며 그것을 확인해 줬다. **다수결로 고르지 말 것.**
유일하게 3 %p 를 넘는 [512×128](1.13×)조차 접기의 공이 아니다: 같은 셀에서 손코드+재배열이
12,487 로 SSGemm 13,272 보다 **빠르므로**, 그 1.13× 는 `loop_ws` 가 tall 한 모양에서 약한 것이다.
**그 약점을 종횡비 64 배 스윕으로 확인했다 (E669)**: K·N 고정에서 `loop_ws` 는 K/N=0.5 의
5,547 에서 K/N=8 의 6,252 로 **+12.7 %** 오르는 반면 손코드+재배열은 평평하다(5.9 %).
즉 `loop_ws` 는 wide 에서 손코드보다 3 % 빠르고 **tall 에서 4~10 % 느리다**(차가운 가중치).
(그 셀들도 E674 의 정정 대상이다 — 같은 소스 상태에서 나온 값이 아니다.)

**차가운 decode 레이어의 확정 수치 (E699, 세 모양 × 6 모드 × 5 회를 한 배치, 모양마다
음성 대조, bus256 @50 MHz)**: `loop_ws` -> 타일-major 재배열 **1.160×**, 재배열 -> 접기
**1.080×**, 합 **1.253×**. 단계별 재배열은 1.086~1.197× 로 **모양에 따라 10 %p 움직인다**
("run 길이 문제라 크기 의존이 약하다" 는 예측은 틀렸다).

**bimodal 인 것은 (바이너리, 모양) 조합이지 경로가 아니다.** 이 트랙에서 관측된 네 번의
6~7 % 갈림이 **전부 [256×256] 한 셀**에서 나왔고(FFN 두 모양은 0.0~1.4 %), 한 번은 `tile`,
한 번은 `plan` 쪽이었다. 그러므로 "경로 X 가 흔들린다" 로 일반화하지 말 것 — E312 의
"예측 불가" 가 맞다. 대응책은 **별칭-최소값**이고, 비(比)는 그것으로 배치에 강건해진다
(배치를 섞은 조립과 한 배치 측정이 **1 % 안에서 일치**).
주의: **별칭 최소값이 비의 어느 쪽을 건드리느냐에 따라 결론의 부호가 반대다** — 분모를
내리면 이득이 오르고(재배열), 분자를 내리면 내려간다(접기 1.104 -> 1.080).

**참고로 손코드끼리 비교하면 타일-major 재배열이 1.4× 다** (E664/E665). 면적 0, RTL 변경 0, 순수 소프트웨어이고
**기본 Gemmini 도 그대로 쓸 수 있다.** 기전은 좁은 run 의 고정비(~5.3 cyc)다 — `stride N`
으로 16 바이트씩 흩어 읽는 대신 타일을 연속으로 읽는다.

**기본 비트스트림에서 직접 확인됨 (E666)**: 1.39/1.42/1.39×, SSGemm 보드 값과 0.5 % 이내.

**단, 크기의 함수가 아니라 "DRAM 에서 오는가" 의 함수다 (E665)**: 작업집합이 L2 를 넘으면
1.39~1.42×, L2 안이면 **0.94~1.09× 로 이득이 없거나 손해**다 (재배열 경로가 `config_ld` 를
더 부른다). **L2 에 들어가는 벤치마크로는 이 이득이 안 보이고, "차가움" 을 만들 때는
작업집합이 실제로 L2 를 넘는지 계산해야 한다** — 회전 사본 수만 정해 놓으면 여전히 뜨겁다. 그 범위는 실무적으로 닿는다
(E661, 실측): **적재를 포함해도 첫 토큰부터 1.28×** 로 앞서므로 상환을 기다릴 필요가 없고,
8 토큰에 2.10×, 64 토큰에 2.85× 로 상한(2.98×)에 접근한다. (합성으로 예측한 비율은
6 개 T 값 전부 3 % 이내로 맞았지만 **절대 시간은 최대 20 % 빗나갔다** — 합성이 쓴
"스트리밍 − 상주" 적재값은 덥혀진 측정이라 차가운 1 회 적재보다 28 % 싸다.)

**스트리밍 체제에서 소프트웨어가 겹치지 않을 때의 이득은 1.3× 다 (E657).** **K·N 을 4 배 늘려도
1.26~1.34× 로 평평**하다
([128×128] 1.26, [192×192] 1.32, [256×256] 1.26, [128×512] 1.34, [512×128] 1.30). 적재가
시간의 60~68 %(기본) / 84~86 %(SSGemm) 를 먹고 그 비중이 크기와 함께 안 변하기 때문이다.
**아래의 2.89~3.17× 는 레이어가 온칩에 상주할 때의 값이고, 그것은 H<=128·FF=4H(192 KB) 급
에서만 성립한다** — 실제 LLM 은 projection 하나가 4 MB 라 해당되지 않는다. 면적 +1.96 %p 에
30 % 는 이 보드의 다른 축(버스 +1.10 %p 에 8~10 %)과 견줄 만하다.

**헤드라인에는 조건이 있다 (E654): 레이어 가중치가 scratchpad(256 KB)에 들어가야 한다.**
H=128·FF=4H 는 192 KB 라 들어가서 **2.89×** 지만, H=256·FF=2H 는 512 KB 라 단계마다 다시
실어야 하고 그러면 **1.30×** 로 떨어진다 (적재가 시간의 66 %/86 % 를 먹는다). 담을 수 없는
크기에서는 **접기보다 적재 대역을 먼저 봐야 한다.** 참고로 **오프라인 재배열을 해 두면
접힌 배치의 적재 비용이 기본과 같다**(−0.3 %) — E603 의 "mvin 4 배" 는 행-major 를 그대로
실을 때의 이야기다. 아래 수치는 모두 **상주 가정**이다:

**decode 레이어 순이득은 모델과 함께 오른다**: H=128·FF=4H **2.89×** (12,765 → 4,412 cyc,
0.41 → 0.14 ms), H=256·FF=2H **3.17×** (34,467 → 10,863 cyc, 1.10 → 0.35 ms) — 접기의 이론
상한 3.4× 의 **93 %** 다 (E646/E649). 단일 GEMV 는 2.79× (K=N=128) ~ 3.20× (256) (E644).
기본 쪽 값이 독립 프로그램(`ssdecs3`, E626)과 0.3~1.2 % 안에서 교차 확인된다.
**M 축 (K=N=128, E647/E648):** 이득이 M=1 2.79× → M=8 1.57× → M=12 1.06× → M=16 **0.93×** 로
떨어진다. M=16 의 회귀는 **하드웨어 비용이 아니다** — shapeshift 는 M=1~16 어디서도 기본
타일 경로를 건드리지 않는다(두 보드 차이 **0.0~0.1 %**, 다섯 M 값에서 확인). 원인은 **경로
선택**이다: M=16 이면 계획기가 shape 0 + FSM 을 쓰는데 FSM 이 타일 경로보다 7 % 느리다.
**그러므로 `M >= 16` 이면 FSM 을 버리고 타일 경로를 쓰면 되고**(계획기의 `use_fsm` 이 그것을
말한다), 그러면 이득은 없지만 손해도 없다. **끝단으로 검증됨 (E651)**: 계획기를 그대로
따르는 `plan` 경로는 M=1/4/8/12/16 에서 각각 **2.79 / 2.58 / 1.57 / 1.06 / 1.00×** 로,
**기본보다 느려지는 M 이 없다** (M=16 은 타일로 떨어져 1113.3 대 1113.3).

**서빙 전체 시간 (E652/E653, 측정값만 — ⚠ 이 문단 전체가 위의 _상주_ 가정 아래 있다.
차가운 스트리밍의 서빙 수치는 절 머리 요약을 볼 것: 1.40~1.59× / ~1.00×)**:
H=256 은 모든 칸에서 H=128 보다 이득이 크다
(seq/gen = 128/512 에서 **3.07×**, 128/128 2.81×, 512/128 2.20×, 1024/64 1.51×) — 레이어
이득 자체가 2.89 → 3.17× 로 오르기 때문이다. **prefill 레이어(35,357 cyc)가 decode 레이어
(34,467)와 거의 같다** — 16 토큰을 1 토큰과 같은 시간에 처리하므로 토큰당 16 배 효율이고,
그것이 접기가 decode 에만 값어치가 있는 이유다. H=128 기준으로는: prefill(M=16)은 1.00×, decode(M=1)은
2.89× 이므로 **전체 이득은 생성 길이가 지배한다** — `seq/gen` = 64/1024 에서 **2.87×**,
128/128 에서 2.59×, 512/128 에서 2.08×, 1024/64 에서 **1.47×**. 즉 **긴 생성(챗봇·에이전트)
2.8×, 긴 문맥·짧은 생성(요약·분류) 1.5×, 어느 쪽도 손해 없음.** M=16 레이어는 실측
13,235 cyc 이고, GEMV 비로 근사하면 4.1 % 높게 나온다. (E647 이 "shapeshift RTL 이 M 이 클 때 비용을
낸다" 고 쓴 것은 오염된 부팅 세션 한 번에서 나온 것이고 **철회**했다 — E648.)
자원별로는 **LUT +8.7 %, Reg +9.1 %, BRAM −16.9 %(!), DSP +2.2 %** 다 — 뱅크를 8 로 나누면
BRAM 패킹이 달라져 64 타일이 **줄어든다**. OOC 로 분해하면 **LUT 증가의 94.9 % 가
shapeshift 논리**이고 뱅크 배증은 0.3 % 뿐이다(E645), 그러니 +8.7 % 는 사실상 shapeshift 의 값이다.
**"기본의 최선" 은 `loop_ws` 가 아니라 상주 타일 경로다** — `loop_ws` 대비 9.2× 는 그쪽이
매 호출마다 DRAM 에서 B 를 다시 싣기 때문이지 접기의 이득이 아니다. 교란 요인: SSGemm 쪽은
`sp_banks=8` 을 함께 지므로 위 면적에 뱅크 배증이 포함된다.

**⚠ 62.5 MHz 판이 둘 지어졌고 둘 다 타이밍이 닫힌다 (E851f/E854a).** `pipe_split=2`
(E851, `AccScalePipe` 조합 사슬 분할, bit-exact 검증됨)가 62.5 MHz 를 열었다:

| config | LUT | CLB | Rocket WNS | 클럭 | 접기 |
|---|---|---|---|---|---|
| `...ms2ps2f62` | 319,954 (24.54 %) | 58,898 (36.14 %) | **+0.536** | 62.5 | S<=2 |
| `...k128ps2f62` | 331,857 (25.46 %) | 63,030 (38.68 %) | +0.236 | 62.5 | **S<=4** |
| (참고) `...ms2f50` | 321,247 (24.64 %) | — | +1.809 | 50 | S<=2 |
| (참고) `...k128f50` | 333,811 (25.61 %) | — | +1.571 | 50 | S<=4 |

**`k128ps2f62` 는 `k128f50` 을 지배한다** — 같은 접기 능력에 클럭 1.25 배, 면적은
1,954 LUT **적다**. `k128f50` 을 쓸 이유가 없다.

**그러나 부팅은 확인 안 됐다.** E642 가 62.5 MHz 에서 "정적 타이밍은 닫혔는데 보드가
안 켜진다" 를 기록했고 그때 여유가 주기의 1.0 % 였다. `ms2ps2f62` 는 **3.4 %**,
`k128ps2f62` 는 **1.5 %** 다 — 후자는 E642 와 같은 자릿수다. **타이밍 폐쇄는
충분조건이 아니므로**, 아래 50 MHz 권장을 그대로 두고 62.5 판은 **보드 확인 대기
선택지**로 기록한다. 확인되면 `H % 64 == 0` 워크로드는 `k128ps2f62`, 그 밖은
`ms2ps2f62` 가 새 권장이 된다.

**권장 구성은 `Rocket64b1gem16ss8bsu16w256ms2f50` 이다 (E837).** SSGemm +
`num_scale_units=16` + 256-bit 버스 @50 MHz 에 **`max_segments=2`, `acc_banks=2`**.
S=4 를 포기하는 대가로 **24,227 LUT 과 0.396 ns 를 아끼는데 두 축 모두 공짜였다**
(S=1 대비 LUT −436 / WNS +0.020). 지어져 있다: bit md5 `5e0f609d`, 321,247 LUT
(24.64 %), WNS +1.809, failing 0. 비교용 S=1 판은 `...ms1f50`, md5 `40d9db21`.
**⚠ 그 md5 들은 E851 이전 트리의 것이다 (E856).** 지금 트리로 다시 지으면 md5 가
달라지는데 **논리는 같다** — 줄번호를 벗긴 모듈 해시로 366/366 일치를 확인했다.
Chisel 이 `@[file line:col]` 을 생성 Verilog 에 심고 assertion 문자열에도 줄번호를
넣으므로 **논리와 무관한 소스 편집이 비트스트림 서명을 무효화한다.** md5 는
**한 트리 상태 안에서만** 서명이다; 달라졌으면 먼저
**`experiments/logic-equiv.py A.v B.v`** 로 논리 동일성부터 볼 것 (`--which` 로 다른
모듈 이름을 찍는다). 그 도구는 줄번호를 벗기고 모듈 본문 해시의 다중집합을 비교한다.
**단 기준 산출물이 현재 RTL 과 _다른 패치 상태_ 에서 지어졌으면 전부 "다름" 으로 나온다**
(E850j) — 같은 RTL 상태의 두 판을 비교할 때만 유효하다.
`H % 64 == 0` 이면서 24 k LUT 여유가 있으면 S=4 판(`...w256f50`)을 쓴다.
**소프트웨어는 `grt_ss_plan_hw(..., max_sh=1)` 를 써야 한다** — 안 그러면 조용히
틀린다 (E837c).

(옛 권장) `Rocket64b1gem16ss8bsu16f50` (SSGemm + `num_scale_units=16` @50 MHz).
기본(`Rocket64b1gem16su16f50`) 대비 LUT +8.7 %, BRAM −16.9 %, 타이밍 손실 없음.

**⚠ 여기 적힌 decode 레이어 2.98×(12,581 → 4,221 cyc)는 _상주_ 조건이다** — 가중치가
이미 scratchpad 에 있고 적재가 타이밍 밖이다. **실제 서빙은 차가운 스트리밍이고 그
수치는 절 머리의 요약 블록을 볼 것**: 모델 전체가 L2 에 들면 decode 레이어 **1.5~1.6×**,
아니면 **1.00×** 다. 상주 조건은 **모델 전체가 256 KB scratchpad 에 들어야** 성립하고
(H<=42, 12 레이어 — E662), 실제 모델은 해당되지 않는다. 쌍의 값이 실측이라는 것과
E655 의 방법은 그대로 유효하다.

**클럭: 50 MHz 가 검증된 값이다 (E650).** `num_scale_units=16`(+`patches/gemmini-accscale-norm0.patch`)
으로 **기본·SSGemm 둘 다 50 MHz 를 닫고 부팅한다** (WNS +1.401 / +1.600, 주기의 7~8 %).
**사이클 수는 유지되거나 오히려 −5~7 % 준다** — 공유 유닛 scale 경로가 mvout 이 많은 큰 N 에서
이득이다. 벽시계로 **1.67×** (SSGemm decode 레이어 0.141 → **0.084 ms**). 같은 클럭 비교
이득은 2.89 → **2.98×** 로 거의 불변이다. 비트스트림: `ssgemm-bit14-f50.bit`(md5 14104a61),
`stock-gem16-f50.bit`(abac1e97).

**62.5 MHz 는 쓰지 말 것 (E642).** 62.5 MHz 는 `num_scale_units=16`
(`patches/gemmini-accscale-norm0.patch` 필요)으로 **정적 타이밍을 닫지만**
(WNS +0.159 / WHS +0.010 / failing 0 of 344,326) **보드가 부팅하지 않는다** — ping 없음,
시리얼 콘솔 0 바이트(115200·230400 양쪽), 같은 절차로 bit12 는 60 초 만에 정상 부팅.
그 +0.159 ns 는 16 ns 주기의 **1 %** 다. **타이밍 폐쇄는 충분조건이 아니다 — 보드가
부팅해야 닫힌 것이고, WNS 가 주기의 1 % 수준이면 "닫혔다" 고 쓰지 말 것.** (이것은
"느슨한 목표의 slack 으로 fmax 를 추정하지 말 것" 의 짝이다.) 임계 경로는 22.482 →
15.485 ns 로 줄었으므로 **50 MHz(여유 +4.5 ns)** 가 다음 후보이고, 면적 대가는 +0.47 %p LUT
(레지스터 +5,220 — `scale_func` 앞에 파이프를 넣어 경로를 쪼갠 것이다).

16×16 mesh 를 실행 중에 8×32 / 4×64 로 접는다. `config_ex` rs1 의 spacer `[12:10]` 3 비트
(`ShapeshiftISA`)로 shape 를 고르고, `Rocket64b1gem16ss` 가 그 빌드다. 전체 실험은
`experiments/JOURNAL.md` E427~E588. **모든 shapeshift 변경은 `patches/gemmini-shapeshift.patch`
(6,000 줄) 안에 있고, stock 경로는 `if (shapeshift_max_segments > 1)` 로 문자 그대로 보존된다.**

**무엇을 위한 것인가 — decode 가 표적이고, 소배치까지 걸친다.** 16×16 배열은 M=1 GEMV 에서
행 슬롯 16 개 중 15 개를 버리고, 접기는 그 낭비를 `h = 16/S` 로 줄인다.

**권장 빌드는 `Rocket64b1gem16ss8b` (sp_banks = 8) 이다.** 접힌 D 는 세그먼트마다 뱅크
하나를 같은 사이클에 읽으므로 `S = sp_banks` 이면 A 가 반드시 D 와 뱅크를 다투고, 그 겹침이
M>1 에서 행마다 직렬화를 부른다. 뱅크를 8 로 두면 S=4 도 A(뱅크 0)와 D(뱅크 1..4)가 갈린다.
**+0.11 %p LUT 로 decode 27 %, 소배치 56 % 개선** (E596) — 256-bit 버스(+1.10 %p 로 8~10 %)나
세 번째 가속기(+12.70 %p 로 0~15 %)와 비교가 안 되는 거래다. 소프트웨어는 `SP_BANKS=8`
환경변수로 따라간다 (뱅크당 엔트리 2048, `bBase = SP_ENT/2`).

**그 비용이 시뮬로 정량화됐다 (E853f/E853g).** `reps` 차분으로 적재를 걷어내고
h·S·sp_banks 를 독립으로 흔든 13 개 칸의 **측정표**다 (전 셀 `accBad=0`, 음성 대조 동반).
**모델이 아니라 표로 인용할 것** — 다섯 번 식을 세웠다가 다섯 번 다 빗나갔다.

| h | S | banks | 여분/op | op 당 비용 |
|---|---|---|---|---|
| 16 | 1·2·4 | 8 | 0.000 | 16.000 |
| 8 | 2·4 | 8 | 0.000 | 8.000 |
| 8 | **8** | **8** | 3.000 | **11.000** |
| 8 | 8 | 16 | 0.000 | 8.000 |
| 4 | **4** | **4** | 3.000 | **7.000** |
| 4 | 4 | 8·16 | 0.938 | 4.938 |
| 4 | **8** | **8** | 3.000 | **7.000** |
| 2 | 8 | 8·16 | 2.562 | 4.562 |

**두 항이 분리된다.** ① 뱅크 충돌이 없는 칸(`S != banks`)만 모으면 여분이 h 로 깨끗이
정렬된다 — **0 / 0 / 0.938 / 2.562** (h = 16/8/4/2), 즉 **`h <= 4` 에서 op 당 4.6~4.9
사이클의 바닥**이 있다 (평평하지 않으므로 `max(h,F)` 로 쓰지 말 것). ② **`S == sp_banks`
는 비용을 `h + 3` 으로 만든다** — 세 칸에서 정확히 그렇다. **단 h=2 에서는 적용되지
않고**(뱅크 8·16 이 같은 값) 이유는 모른다.

**보드로 옮길 수 있는 것은 둘이다:**

① **`sp_banks > S` 를 지킬 것** — 같으면 op 당 3 사이클. `ss8b`(sp_banks=8)는 S<=4 에서
이미 옳고, **16 으로 더 올려도 아무 것도 안 얻는다**(0.938 불변). E596 의 판단이
독립 확인되고, 더 올릴 이유가 없다는 것도 같이 나온다.

② **`h` 를 4 아래로 내리지 말 것.** h=2 는 op 당 4.56 으로 h=4 의 4.94 와 거의 같다 —
**접기를 두 배 더 해도 계산이 안 빨라진다.** 그러므로 접기의 이론 상한
`(16+M)/(16/S+M)` 은 **S=8 에서 의미를 잃는다**(바닥에 부딪힌다). E631 이 S=8 을 면적
(+20.22 %p LUT)으로 기각한 것과 **독립적으로, 성능만 봐도 살 것이 없다.**

**측정 못 하는 구간이 있다: `h=5`·`h=6` 은 존재하지 않는다** —
`I_TILE_BYTE_WIDTH is not power of 2` 로 `rows` 가 2 의 거듭제곱이어야 하므로 h 도 그렇다.
바닥의 정확한 모양(4 와 8 사이)은 이 설계에서 **원리적으로** 측정 불가다.

**⚠ 이 갈래에서 판단이 다섯 번 뒤집혔다.** 넷은 격자가 성겨서였고, 하나는 **통제하지
않은 상수**(`sp_banks` 를 8 로 고정한 채 `S` 를 쓸었다) 때문이었다. **한 축을 스윕할 때
그 축과 같은 단위의 상수가 설정에 있는지 먼저 볼 것**, 그리고 **축이 셋인데 칸이 열 몇
개면 식을 세우지 말 것** — 이 파일이 다중 가속기 쪽에 적어 둔 "그 regime 은 모델이 아니라
측정표다" 가 그대로 적용된다.

**⚠ 이 갈래에서 판단이 네 번 뒤집혔고 마지막 한 번은 _통제하지 않은 상수_ 탓이었다.**
`S` 를 쓸면서 `sp_banks` 를 8 로 고정해 두고, 둘이 만나는 칸(S=8)에서 나온 값을 "S 의
성질" 로 읽었다. **한 축을 스윕할 때 그 축과 같은 이름·같은 단위의 상수가 설정에 있는지
먼저 볼 것** — 이 저장소의 "비트스트림 서명을 먼저 찍어라" 의 시뮬 판이다.

| M | S=1 | S=2 | S=4 | 최적 | 이득 |
|---|---|---|---|---|---|
| 1 | 1084.8 | 601.8 | **402.0** | S=4 | **2.70×** |
| 4 | 1082.6 | 625.8 | **395.8** | S=4 | **2.74×** |
| 8 | 1082.2 | 695.6 | **693.0** | S=4 (동률) | 1.56× |
| 12 | 1163.2 | **1002.6** | 1024.2 | S=2 (동률) | 1.16× |
| 16 | **1242.2** | 1347.8 | 1332.8 | S=1 | 1.00× |

    (bit10)  M <= 8  -> shape 2 (4x64)      M = 12 -> shape 1 (8x32)
             M >= 16 -> shape 0 (16x16)

**규칙의 M 임계값은 K·N 과 함께 올라간다 (E627).** `M<=8 -> S=4` 와 `M>=16 -> S=1` 은
K·N 4 배(128² → 256²)에 걸쳐 그대로지만, **경계인 M=12 는 크기 의존**이다: 128² 에서는
세 shape 이 2.7 % 안의 삼자 동률(접기 무가치)이고 256² 에서는 접기가 S=1 을 **1.20 배**
앞선다. **접기가 이득인 M 의 상한이 8 → 12 로 올라간다.** M=16 이 S=1 인 것은 두 크기
모두 같지만 그 마진이 17.7 % → 4.1 % 로 줄어드니, 더 큰 K·N 에서는 그 경계도 움직일 수
있다. 반면 **M=1 의 접기 이득 자체는 크기 불변**(3.01× vs 3.00×, K·N 4 배에 0.3 % 차) —
E626 이 레이어에서 본 상승은 matmul 이 아니라 **shape 과 무관한 단계 고정비**(config·
mvout·fence)가 큰 모델에서 희석된 것이다.

**S=2 는 8 뱅크 보드에서 더 이상 최적이 아니다 (E627).** 12 개 (M, 크기) 칸 어디에서도
단독으로 이기지 않는다 — M<=12 는 S=4, M>=16 은 S=1 이다. 뱅크를 8 로 늘린 변화는 S=4 를
빠르게 만든 데 그치지 않고 **S=2 를 불필요하게** 만들었고, 런타임 선택기는 S=4 / S=1
이지선다로 줄어든다. (아래 bit9 = sp_banks 4 표는 S=2 가 최적인데, 그것은 그 보드의 성질이다.)
다만 **8 뱅크는 상주 가중치 상한을 반으로 줄인다** (뱅크당 2048 행) — K=N=256 은 S=1 로
안 들어간다. 이것이 뱅크를 늘린 대가다.

**접기는 M=12 까지 이득이 있고 M=16 에서 처음 진다.** 위 표는 `-DSPB=8` 상수판 기준이며,
런타임판으로 재면 **접힌 shape 만 15~20 % 오염되어 M=12 의 순위가 뒤집힌다**(E599) —
도구 오염은 구성마다 다르게 붙으므로 **순위도 최적 도구로 확인할 것**.

**최적 shape 은 배치 M 에 따라 바뀐다** (bit9 = sp_banks 4, FSM 경로, K=N=128, cyc/step):

| M | S=1 (16×16) | S=2 (8×32) | S=4 (4×64) | 최적 |
|---|---|---|---|---|
| 1 | 1210.8 | 595.6 | **524.6** | S=4 (2.31×) |
| 2 | 1083.4 | **601.6** | 657.8 | S=2 |
| 4 | 1091.2 | **625.4** | 1036.6 | S=2 (1.74×) |
| 8 | 1081.4 | **718.8** | 1326.8 | S=2 |
| 12 | 1155.0 | **1024.6** | — | S=2 |
| 16 | **1243.0** | 1342.6 | 1960.0 | S=1 |

소프트웨어 규칙 (측정으로 확정, E593): **`M == 1 → shape 2`, `2 <= M <= 12 → shape 1`,
`M >= 16 → shape 0`**. **S=4 는 M=1 에서만 최적**이고 M=2 에서 이미 S=2 에 진다. **소배치에서 덜 접는 편이 빠른 이유는 구조적이다** — S=4 는
`S = sp_banks` 라 A 와 접힌 D 가 반드시 같은 뱅크를 쓰고, 그래서 아래의 뱅크-겹침 직렬화를
피하지 못한다. 접기의 이득(행 감소)과 뱅크 겹침의 비용(직렬화)이 반대로 움직인다.

**M 의존 모델은 체제마다 다르다 — 하나를 하드웨어의 성질로 일반화하지 말 것.** 행-바운드
비트스트림(bit7)에서는 `max(a_rows, bd_rows)` 가 맞아 `M·S <= 16` 밖에서 이득이 1.0 으로
소멸했고(E578/E579, 판별 실험에서 0.3 % 오차), 행이 빨라진 bit8 에서는 **반증**되어 A 행 하나가
2.11 사이클을 먹는 선형 모델(op 당 `2.11·M + 16.8`, 오차 0.4 %)이 된다(E589). 같은 법칙이
두 번 확인되고 두 번 반증됐으며, 매번 원인은 **무엇이 바인딩인지가 바뀐 것**이었다
(행 → 발행 → 행+A행). 절대 시간은 bit7→bit8 에서 여덟 셀 전부 2.3~5.4 배 개선됐다 — 퇴행은 없다.

**기전: 접기는 계산을 1/S 로 줄이는 대신 적재를 S 와 함께 늘린다 (E713).** [256×256] bus256
에서 순수 적재 4160 / 5425 / 9865 (S=1/2/4, 즉 1.00 / 1.30 / **2.37×**), 상주 계산
4157 / 2185 / 1367 (이론 상한대로). **최적은 그 곱이 최소인 곳**이고, 그래서 세 체제의 답이
하나의 상충으로 통일된다:

- **상주**: 적재 0 -> 계산만 -> **S=4**, 이론 상한 3.04× 달성
- **bus256 스트리밍**: 적재 바닥 4160 이 계산과 비슷 -> **S=2**
- **bus64 스트리밍**: 적재 바닥이 2 배 -> 적재 지배 -> **S=1**(안 접는 것)

"접기의 값어치" 를 하나의 수로 말할 수 없는 이유가 이것이다. (측정 주의: `--noexec` 의 S=4
적재값은 적재+계산보다 크게 나오므로 **상대 비교용**이다.)

**그 적재 벌금의 기전은 (소스 흩어짐 + 목적지 뱅크 순환)의 결합이다 (E726~E728).** `--noexec`
로 정확성을 포기하고 변수를 하나씩 통일해 좁혔다: 목적지 뱅크를 통일해도(2.128×), 목적지
순서까지 순차로 해도(2.142×) 안 변하는데, **소스를 한 연속 영역으로 만들면 8,887 -> 4,940
(1.80× 개선), 비가 2.14× -> 1.18×** 가 된다. `Bpre[0..3]` 이 256 KB 씩 떨어진 네 영역이라
DMA 의 TLB/페이지 국소성이 깨지는 것이다. **그러나 실제 경로에는 못 쓴다**: 소스 연속과
목적지 순차를 **동시에** 만족해야 빠른데(E721 의 `--cat` 은 소스만 연속으로 만들어 오히려
느렸다), FSM 규약이 세그먼트를 서로 다른 뱅크에 요구하므로 계산과 겹치려면 목적지가 순환할
수밖에 없다. **그리고 그것은 고칠 수 없다 (E730).** 대리 측정으로는 "세그먼트를 한 뱅크의 연속 구간에
두면 적재가 1.80× 빨라지고 접기 이득이 1.248 -> 1.558× 가 된다" 가 나오지만, `ssWide` 가
**한 사이클에 S 개 뱅크를 동시에 읽어** `meshColumns*S` 폭 피연산자 행을 조립하므로 그 배치로는
접힌 계산 자체가 S 사이클로 늘어난다 — 접기가 사는 이득을 정확히 상쇄한다. **뱅크 분산은
임의의 주소 규약이 아니라 접기의 전제이고, 적재 벌금은 접기의 구조적 비용이다.**
(교훈: 하드웨어 변경을 권고하기 전에 그 자원이 **다른 경로에서 어떻게 쓰이는지** 코드로 읽을 것.
쓰기 쪽만 보고 규약이라 가정했는데 읽기 쪽에서는 병렬성 그 자체였다.)

**(이전 기록) 세 가설이 기각됐다 (E720~E722).** wide S=1 과 S=4 는
명령 수·명령 모양·바이트·목적지 뱅크 수·DRAM stride 가 **전부 같은데** 적재가 2.16× 다.
① 명령이 뱅크를 넘나드는 비용(`block_mvin_stride`=뱅크크기) **0.88× 로 더 느림**,
② DRAM 스트림 수(연속 배치로 1 개) **0.86× 로 더 느림**,
③ 뱅크 전환 빈도(세그먼트를 바깥 루프로) **효과 없음(1.00~1.04×)** — 셋 다 정확성 게이트를
통과한 구현이다. 남은 후보는 접힌 shape 에서만 켜지는 RTL 경로이고 보드에서는 격리 불가다.
**회전 사본을 같은 값으로 두면 주소 버그를 정확성 검사가 못 잡을 수 있다** — `--cat` 의
사본 인덱스 오류가 S=4 에서만 우연히 PASS 했다. 회전 사본은 서로 다른 값으로 채울 것.

**그 적재 벌금은 소프트웨어로 못 없앤다 (E720).** `config_ld` 의 `block_mvin_stride` 를 뱅크
크기로 주면 폭 `S*16` mvin 하나가 세그먼트 S 개를 **DRAM 연속 읽기 한 번**으로 채울 수 있고
(정확성 검증됨), 명령 수도 바이트도 같다. 그런데 **0.88× 로 더 느리다** — DRAM 쪽이 더 좋아도
진다. 즉 벌금의 원인은 DRAM 스트림 수가 아니라 **scratchpad 쓰기가 뱅크를 넘나드는 것**이다:
한 명령이 *한 뱅크의 연속 64 행*을 쓰는 편이 *S 개 뱅크에 16 행씩* 쓰는 것보다 빠르다.
고치려면 RTL(뱅크별 쓰기 포트) 변경이 필요하다.

**최적 shape 은 체제에 따라 뒤집힌다 — 같은 보드·같은 바이너리로 확인 (E705).**
[256×256], M=1, bus256, 5 회 최소값. 다른 것은 **적재가 타이밍 루프 안에 있는가** 하나다:

| shape | 상주 | 차가운 스트리밍 |
|---|---|---|
| S=1 (16×16) | 4157.5 | 8137.5 |
| S=2 (8×32) | 2185.0 | **6520.0** ← 최적 |
| S=4 (4×64) | **1367.5** ← 최적 | 9200.0 ← **꼴찌** |

**S=4 는 최선(S=1 대비 3.04×)에서 최악(S=2 대비 0.71×)으로 바뀐다.** 그러므로 상주 수치
(2.7~3.0×)를 스트리밍 워크로드에 인용하면 숫자만이 아니라 **shape 선택까지 틀린다**.
`grt_ss_plan_ex(..., streaming)` 가 규칙을 모두 담는다:

    shape   : M >= 16  -> 16x16 (접지 않음),   M <= 12  -> 4x64
              (체제 구분 없음. 옛 "스트리밍 + K*N >= 48 KB -> 8x32" 절은 작업집합
               혼입에서 유도된 것이라 E744 에서 제거했고, E793 이 그 판정을 확인했다:
               통제하고 3 배치로 재면 세 셀 중 **하나만** S=2 를 원하고(2.0 %) 나머지
               둘은 동률이다 — 행동 기준 미만이다.
               **방향은 맞았고 크기가 20 배 부풀어 있었다** — 더 큰 K*N 에서는 다시
               커질 수 있으니 "틀린 규칙" 이 아니라 "무시할 크기" 로 읽을 것.)
    use_fsm : 상주 M >= 16 -> 타일,  스트리밍 -> **항상 FSM**

**작은 K·N 에서 스트리밍이 상주처럼 행동하는 것**(32 KB 에서 S=4 가 S=2 를 1.16× 이긴다)은
E713 의 기전 그대로다 — 적재가 짧으면 계산 비중이 커진다. 경계는 측정으로 **(32, 48] KB**,
48 KB 의 두 종횡비가 2.5 % 안에서 같으므로 변수는 K·N 이다. 검증: 스트리밍 M 축 4 점 +
크기 축 5 점 + **두 축의 교차 2 점**에서 강제 최적과 **±3 % 이내**(−1.9 ~ +2.3 %),
상주 M=1 에서 +0.0 %. **M 과 K·N 은 곱해지지 않고 각각 작용한다** — (32 KB, M=8) 은 S=4,
(72 KB, M=12) 는 S=2 로 각 조건이 독립적으로 맞는다 (E718).

**`use_fsm` 도 체제 의존이다 (E707/E708)** — 상주에서 M>=16 은 FSM 이 타일보다 7 % 느리지만
(E648), 차가운 스트리밍에서는 **FSM 이 빠르다** — 단일 셀로는 31 %(8,102 대 10,615)지만
**레이어 수준에서는 1.13~1.18×** 다 (E737). 그 단일 셀의 `tile` 값이 이 트랙에서 가장 자주
흔들리는 칸이라 상한 쪽으로 오염돼 있었다. 적재가 지배하면 FSM 의
적재 파이프라이닝이 이긴다. 고친 뒤 계획기는 스트리밍 전 M 구간에서 강제 최적과 **+2.3 %
이내**이고 상주 경로에는 회귀가 없다.
(하네스 주의: 상주 경로에 `--wide` 를 주면 `load_b()` 가 원래 순서를 가정해 **bad=256** 으로
전부 틀린다 — `--wide` 는 `--ovl` 전용이다. 두 번 밟았다.)

**적용 범위 — 가중치 재사용이 전제다 (E602/E603).** 이 트랙의 수치는 대부분 가중치가
scratchpad 에 상주하는 조건이다. 토큰마다 DRAM 에서 스트리밍하면 그림이 달라진다:

| 체제 | 접기 이득 |
|---|---|
| 가중치 상주 | **2.70×** |
| 스트리밍 + 행-major 가중치 | **0.75× (손해)** |
| 스트리밍 + 오프라인 재배열 | 1.08~1.15× (M 과 함께 감소) |

**스트리밍에서는 배치가 접기보다 7~8 배 중요하다** (E605; bus256·정정된 규칙에서 재확인 —
토큰당 54,252 -> 6,558 cyc = **8.3×**, 그 위에 접기 1.5×, E735. 레이어 시간이 M 에 평평하므로
**배치는 사실상 공짜**다. 그리고 접기 이득은 M 과 함께 **안 떨어진다**: 1.436/1.507/1.497×
at M=1/4/8 — 이론 상한 `(16+M)/(8+M)` 은 떨어지지만 달성률이 76 -> 90 -> **99.8 %** 로 올라
상쇄한다): M=1→8 로 배치하면 시퀀스당
2838.7 → 391.2 (**7.3×**)인 반면 접기는 1.08~1.14× 다. 우선순위는 ① 배치 ② 가중치
재배열(1.55×) ③ 접기 순이다. 또 스트리밍에서는 **M=8 의 최적이 S=2** 로, 상주 체제
(M=8 도 S=4)와 **반대**다 — 적재가 상수라 계산부 비중이 커지는데 S=4 의 계산이 M=8 에서
급증하기 때문. 스트리밍 성능은 **"shape 별 적재 상수 + 상주 계산"** 합성으로 6 % 안에
예측된다(적재와 계산이 중첩되지 않는다 — 중첩이 남은 개선 여지).

행-major 에서 손해인 이유는 접힌 배치가 세그먼트마다 다른 뱅크를 쓰므로 **같은 바이트에
mvin 명령이 4 배**(256 vs 64) 들기 때문이다 — "mvin 은 폭과 무관하게 명령당 비용이 같다"가
그대로 물린다. **가중치를 오프라인에서 뱅크별 연속으로 재배열하면** 명령 수가 같아지고
(−35.6 %) 접기가 다시 앞선다. 스트리밍에서 1.15× 에 그치는 것은 적재가 시간의 87 % 라
접기가 건드릴 수 있는 부분이 13 % 뿐이기 때문이다. **스트리밍 decode 의 병목은 접기가
아니라 가중치 적재이며, 그것은 shapeshift 와 직교하는 축이다.** 그 적재는 **대역 바운드**로
**6.8~7.7 B/cycle** 이다 — mvin 명령을 4 배 더 줄여도(wide mvin, cols=64) 시간이 안 줄고
오히려 2.5 % 늘었다(E604). 즉 corpus 의 **"`MAX_BLOCK_LEN` 만큼 실어라"(E205)는 명령
바운드일 때만 유효**하며, 여기서는 재배열만으로 이미 대역에 닿는다.

**스트리밍의 천장 두 개 (E604/E608)**: ① 가중치 적재가 **대역 바운드**(6.8~7.7 B/cycle)
라 mvin 명령을 4 배 줄여도 안 줄고, ② 적재와 계산이 **겹치지 않는다**. ②의 원인은
스크래치패드 포트가 아니라 **발행 순서**이며, **입도가 유일한 변수**다 (뱅크 배치는 2.3 %):

| 입도 | 중첩률 | 계산 회수 |
|---|---|---|
| 섞지 않음 | 0 % | 0 % |
| 16 타일 | 28.3 % | 57 % |
| **4 타일** | **40.4 %** | **81 %** |
| 1 타일 | **−15.8 %** (역전) | — |

**U 자 곡선**이다 — 굵으면 RS 가 한쪽으로 차고, 잘면 발행 루프의 CPU 일이 지배한다
(E205 의 반대편 벽). **4 타일 묶음으로 번갈아 발행**하면 스트리밍 스텝이 1.37× 빨라진다.
"자원이 아니라 발행이 병목"인 이 트랙의 세 번째 사례다(E166 의 타일 인터리브, E205 의
빈 발행 루프에 이어). **FSM 이 GEMV 전체를 한 번에 돌리는 것이 여기서는 약점**이었고,
`SS_GEMV` 의 rs2 로 **블록 단위 부분 실행**을 넣어 해결했다 (`grt_ss_gemv_blk`,
rs2 = ppCount[31:16] | ppStart[15:0], 0 이면 전체라 하위 호환; LUT +86 개).
블록마다 부르고 사이에 다음 가중치를 실으면 스트리밍이 **1.12~1.19×** 빨라진다
(격리 측정 기준 — 긴 벤치마크 안에서는 1.23~1.47× 로 보이지만 그중 절반은 위치 편향,
E614). **접기와 중첩이 블록 수를
놓고 경쟁하는 것은 블록이 적을 때만**이다 — N 을 256 으로 키워 ntb 가 16/8/4 가 되면
shape 간 차이가 **0.1 %** 로 사라진다(E612).

**같은 바이너리라도 argv 가 다르면 다른 성능 모드에 앉는다 (E693).** M=16 에서 `plan` 은
`use_fsm=false` 로 `tile` 과 **문자 그대로 같은 코드**를 도는데, 두 호출이 9,980 대 10,650
(**6.7 %**)로 갈리고 각자 자기 값을 5 회 재실행 내내 유지한다(`plan` 은 두 순서에서 0.05 %
재현). 그러므로 아래의 "두 경로를 한 바이너리에 넣고 런타임 플래그로 전환하라" 는 **충분
조건이 아니다** — **동일-코드 대조군을 A/B 안에 함께 넣어 잡음 바닥을 재라.** 여기서는 M=16
이 우연히 그 역할을 했고, 없었다면 0.939× 를 "접기가 진다" 로 읽었을 것이다.
**처방은 별칭-최소값 방법이다 (E694)**: A/B 각 팔에 **완전한 별칭 모드**를 하나 두고(조건식에
`||` 한 번), 팔마다 별칭 최소값을 쓰고 별칭 비를 대조군으로 함께 보고한다. 비용은 코드 두
글자와 측정 시간 2 배이고, 잡음 바닥을 **같은 실행 안에서** 재면서 모드 편향의 절반을 없앤다.
실측 대조군은 여섯 중 다섯이 1 % 안이었고(최대 2.9 %), 이 방법으로 레이어 이득이 1.104 ->
1.072 로 3 %p 내려앉았다 — **대조군 없는 A/B 는 낙관 편향을 가진다.**

**바이너리를 바꾸면 같은 측정이 2.9 배 달라진다 (E616/E626).** 측정과 무관한 소스 편집
(가드 추가, `strstr` 사용, 반복 루프)만으로 `blk shape=2` 가 2300 → 5450 이 된다 —
같은 보드·세션·비트스트림·주소에서. corpus 의 E312/E595 현상이 12 % 가 아니라 **2.4 배**
규모로 나타난 것이며, **비율조차 바이너리 의존**이다 (같은 비교가 한 바이너리에서 1.47×,
다른 바이너리에서 1.19×). 따라서 **개선을 주장하려면 두 경로를 같은 바이너리에 넣고
런타임 플래그로 전환**해야 한다 — 재컴파일 후 비교는 무효다. (E613 의 "문맥 2.45 배"는
이것의 오진이었고 철회됐다.) 같은 `blk` 측정이 20 블록짜리 프로그램의
뒤쪽에서는 2215, 격리하면 **5433** 이다 — 앞 블록들이 뒤 측정을 **빠르게** 만든다
(워밍업으로 보이나 기전 미확인). 그래서 **절대 수치는 격리 측정으로 내고, 비교는 같은
프로그램 안에서** 한다. 긴 벤치마크의 뒤쪽 수치는 낙관적으로 오염돼 있다.
(E612 의 "scratchpad 배치가 5 배" 경고는 이것의 오진이었고 철회됐다 — 주소를 1024 행
옮겨도 격리 측정에서는 0.2 %, 시뮬에서는 0 % 다.) **가장 큰 사례는 E626 이다**: `HMAX`
상수 하나를 192→256 으로 올려 정적 배열만 키우면(H=192 실행에서는 의미적으로 아무 것도
바뀌지 않는다) decode 여섯 단계 중 **FFN2 하나만 4862 → 14286, 2.94 배** 느려지고 나머지
다섯은 2 % 안에서 일치한다. 바이트 수가 같은 FFN1 은 멀쩡하다. 그래서 **"한 바이너리 안에서
비교하라"는 비교뿐 아니라 절대값 보고에도 적용된다** — 두 바이너리가 1.64 배 다르면 어느
쪽도 "이 하드웨어의 성능"이 아니다. 이전 바이너리를 지우지 말 것: E626 의 결정적 대조는
E625 의 바이너리가 NFS 에 남아 있어서 가능했다. 스트리밍 소프트웨어 최적화(재배열 + 부분 실행)의 크기는 **같은 바이너리 안에서
1.1~1.5×** 로만 말할 수 있다 — 바이너리마다 1.23×(6720→5475)와 1.90×(4401→2312)로
갈리기 때문이다(E615/E616). **절대값 사슬은 신뢰할 수 없다.** 반면 **접기 자체의 2.70×
는 한 바이너리 안에서 shape 만 런타임 인자로 바꿔 잰 것이라 온전하다.**

**서빙 정책 — 크기에 따라 답이 뒤집힌다 (E628/E629).** prefill(M=16)은 S=1 을, decode(M=1)은
S=4 를 원하는데(E627) 전환은 B 재적재 **4707~5018 사이클**이 든다. 세 정책을 K=N=128 에서
재면: **두 배치를 미리 깔아 두는 이중상주가 다섯 칸 모두 최속**(2 위 대비 4.6~12.3 %,
`bBase` 행 오프셋으로 분리, 대가는 가중치 저장 2 배)이고, 그것이 안 들어가면 **P < 38
청크(seq < ~600 토큰)에서는 그냥 전부 S=4** 가 적응형보다 빠르다 — 정상 상태 서빙은
시퀀스마다 **두 번** 재적재하기 때문이다(다음 prefill 이 다시 S=1 배치를 요구한다).
손익분기 실측 P=37.9 가 산술 예측 37.5 와 1 % 안에서 맞았다. **전부 S=1 은 어느 칸에서도
이기지 않는다** — 접지 않는 선택은 서빙에서 항상 틀린다. 이중상주 시간은 E627 의 셀별
최소값으로 −1.3~+2.3 % 안에 조립된다.

**하지만 크기가 커지면 이중상주도 값어치를 잃는다 (E629).** K=N=256 에서도 이중상주는
되지만(`bBase ≥ sp_bank_entries` 를 주면 **기준 뱅크가 옮겨간다** — FSM 에 뱅크 필드를
추가할 필요가 없다, `bBase4 = ceil(rows1/SP_ENT)·SP_ENT`) 마진이 **4.6~12.3 % → 1.2~3.9 %**
로 준다. 이중상주가 "전부 S=4" 를 이기는 폭은 **prefill 의 shape 이득 그 자체**인데,
M=16 에서 S=1 의 우위가 128 에서 15 %, 256 에서 4 % 뿐이기 때문이다. 전환비는 B 바이트에
비례해 4707 → 21500 사이클/회로 오르고 재적재 손익분기는 **38 → 202 청크**로 밀려난다.
그래서 **트랜스포머 크기에서는 shape 하나(S=4)로 전부 돌리는 것이 옳다** — 전환 없음,
이중 저장 없음. 작은 K·N 에서만 이중상주가 가중치 2 배를 낼 값어치가 있다.

**런타임 shape 전환은 검증됐고, 비용은 B 재배치가 전부다 (E600).** 한 프로세스에서
2→0→1→2→1→0 으로 바꿔가며 6/6 PASS. 비용은 `config_ex` + FSM config 만이면 **+70 사이클**
(0.2 스텝, 사실상 공짜)이고, B 를 새 세그먼트 배치로 다시 깔면 **+3500 사이클**
(decode 스텝 8.7 개; 재배치 실효 대역 4.7 B/cycle, mvin 명령당 13.7 사이클로 명령-바운드).
따라서 **토큰마다 바꾸지 말고 레이어·단계 경계에서 바꿀 것.** 같은 가중치를 두 배치로
미리 깔아두면 전환이 공짜가 된다 (scratchpad 두 배 vs 전환 비용의 거래).

**게이트는 "뱅크가 겹칠 때만" 걸 것 (E590/E591/E592).** 교착을 막으려고 넣은 직렬화
게이트들이 "피연산자가 둘 다 실재할 때"라는 넓은 조건으로 걸려 있었다. 진짜 조건은
**뱅크 집합이 실제로 겹치는가**(`ssSel2`)다. 좁힌 결과 보드에서 sh=0 M=4 −31.5 %,
sh=0 M=16 −61.3 %, sh=1 M=4 −46.0 % 인 반면 **S=4 세 셀은 ±1.2 % 로 불변**(항상 겹치므로
게이트가 그대로 걸린다) — 이 대조가 판정을 봉인한다. M=1 벤치마크만 보던 때는 이 비용이
보이지 않았다. **안전을 위해 넓게 건 게이트는 대가가 보이는 축에서 재기 전까지 공짜처럼
보인다.** 시뮬(SSExecSP)은 방향과 대조군에는 옳았지만 크기는 10~22 % 로 과소평가했다.

**decode 레이어 (Q/K/V/O + FFN 4H, H=128, M=1, bit11, 전 shape 상주): 16×16 12910 cyc
→ **4×64 4421 cyc (141 µs), 2.92×** (E621). 스트리밍이면 이득이 1.24× 로 준다(적재가
85 %, E619).

이득은 크기에 유지된다: H=192 에서 **3.01×** (E622).

**접기는 상주 용량을 오히려 줄인다.** 총 행 수는 shape 무관하게 `K·N/16` 이지만,
접힌 shape 은 세그먼트 s 가 뱅크 `(1+s)` 에 **갇혀** 넘치면 옆 세그먼트를 침범한다.
접지 않은 shape 은 세그먼트가 하나라 옆 뱅크로 자연히 이어진다. 뱅크당 행은
`K·N/(16·S)` 이므로 **한 행렬의 상주 한계**는:

    16×16 → 256 KB (스크래치패드 전체)   8×32 → 64 KB   4×64 → 128 KB

**접힌 shape 은 `K·N <= 32 KB × S` 를 넘기지 말 것** — 넘기면 조용히 틀린다.
(E620 의 "접기가 용량을 S 배로 늘린다"는 정반대였고 E621/E622 에서 철회·정정했다.)

**그 한계는 `stride` 로 없앤다 (E624/E625).** `config_ex` 의 spacer 비트로 **세그먼트 뱅크
간격**을 주면(`ssBank = base + s·stride`) 세그먼트마다 넘칠 여지가 생겨 크기 상한이
사라진다. H=192·FF=4H 에서 4×64 는 stride 1 에서 60245 cyc(분할), **stride 2 에서
9823 cyc(상주) — 10.9 배, 이득 0.27× → 2.94×** 로 접기 이득이 크기와 무관해진다
(+240 LUT). **배치 규칙:**

    stride = 1; while (stride·2048 < 뱅크당_필요행 && stride < sp_banks/S) stride *= 2
    세그먼트 뱅크 = base + s·stride (mod sp_banks)
    넘침은 **다음 뱅크의 낮은 행**으로 간다 — A 등 다른 상주 데이터는 **높은 행**에 둘 것
    (어기면 보드가 wedge 된다 — E625 첫 시도가 그랬다)

**손으로 고르지 말고 계산하게 하라** — `ssdecode.c:calc_stride()` 가 위 식이고, E625 에서
손으로 고른 값을 그대로 낸다(E626). **단계마다** 고르는 것이 맞다: 같은 레이어에서도
Q/K/V/O 는 stride 1 로 충분한데 FFN 은 2~4 가 필요하다. S=1 은 세그먼트가 하나라 stride
가 무의미하므로 1 로 고정한다. 상한을 넘겨 주면(S=2 에 stride 8) 세그먼트가 감겨 서로를
덮어 **전 단계 FAIL**, 모자라게 주면 분할로 떨어져 **10.7 배** 느려진다 — 양쪽 다 E626 에서
음성 대조로 확인했다.

stride 를 워크로드에 맞게 골라야 한다: 같은 레이어에서 S=4 는 stride 2 로 충분하지만
S=2 는 4608 행이라 **stride 4** 가 필요하다 (stride 2 면 여전히 분할돼 0.49×).

**그리고 stride 를 쓰면 접기 이득이 크기와 함께 *커진다* (E626).** decode 레이어를 세
크기로 재면 shape 2 의 이득이 **2.836× (H=128) → 2.958× (H=192) → 3.056× (H=256)** 로
오른다. 접기의 이론 상한은 `(16+M)/(16/S+M) = 3.4×` (M=1, S=4)이고 측정치는 그 상한의
83 → 87 → **90 %** 로 다가간다 — K 가 커지면 패스당 청크가 늘어 패스 고정비가 희석되기
때문이다. **stride 가 없앤 것은 용량 절벽이지 크기 의존성 전체가 아니며**, 남은 의존성은
부호가 반대다(클수록 유리). 새 상한은 뱅크가 아니라 스크래치패드 전체 16384 행
(`K·N ≤ 256 KB`)이고, A 자리를 빼면 H=256·FF=2H 가 들어가고 H=256·FF=4H 는 원리적으로
불가능하다.

**decode 단일 GEMV 헤드라인: 441.1 cyc/step** (bit10 = `Rocket64b1gem16ss8b`, `ssgemv f 2 128`,
`-DSPB=8` 상수판). 접지 않은 이전 최고(1172.5) 대비 **2.66×**. 궤적은
1172.5 → 575.6 (E577) → 462.8 (뱅크 8) → 441.1 (상수판).

**측정 도구 주의 (E595/E598): 절대 수치는 `-DSPB=<n>` 컴파일 타임 상수판으로 낸다.**
`SP_BANKS` 를 런타임 변수로 두면 발행 루프의 주소 계산이 접히지 않아 tile 경로가 10 %,
FSM 경로가 4.7 % 느려진다 — CLAUDE.md 의 "발행 루프는 비어 있어야 한다"가 그대로 물린다.
런타임 판은 **두 비트스트림을 한 바이너리로 비교할 때만** 쓴다 (바이너리를 바꾸면
셀이 안정적으로 12 % 옮겨가므로 비교는 반드시 같은 바이너리로 — E595).

**측정된 이득 (bit8, 무펜스, `chrt -f 99`, H=128):**

| shape | tile 경로 | FSM 경로 (funct 23/24) |
|---|---|---|
| 16×16 | 1351.7 cyc/step | 1249.1 |
| 8×32 | 1269.2 | 769.5 |
| 4×64 | 1234.3 | **575.6** |

**명령 입도가 접기의 값어치를 결정한다.** 같은 하드웨어에서 tile 경로는 1.10×, FSM 경로는
**2.17×** — 차이는 오직 "명령 하나가 몇 행을 모는가"다. tile 경로는 명령 수가 shape 와 무관하게
고정(64 쌍)이라 접어도 명령 바닥(~9~10 cyc/명령)에 걸린다. **접기를 쓰려면 coarse-grained
명령이 반드시 있어야 한다.**

**남은 격차와 그 정체.** FSM 의 2.17× 는 이상적 4× 의 54 %. op 당 비용을 재면
`0.912 cyc/row × h + 4.23 cyc` 이었고 그 고정비가 h=4 에서 op 의 절반을 먹었다 — **그러나
bit12 에서 다시 재면 고정비는 0 이다** (E630): 비용이 `1.000 · rows − 0.12` 로, 세 shape 이
2.3 % 안에서 맞고 시간이 op 수에 선형이다(잔차 0.2~2.6 %). 뱅크를 8 개로 늘린 것(A 가 뱅크 0,
접힌 D 가 1~4 — 더 이상 공유 안 함)과 게이트를 뱅크 겹침 조건으로 좁힌 것(E590~E592)이 그것을
걷어냈다. **둘 다 다른 목적으로 한 변경이다.** 그래서 접기의 이론 상한
`(16+M)/(16/S+M)` 이 곧 실효 상한이고(M=1, S=4 → 3.4×), E626 의 3.056× 는 그 **89.9 %** 다.
남은 10 % 는 op 경계가 아니라 **pp 블록당 비용**이다 (E632, E630 의 "호출당 상수" 지목은
오진): K 와 N 을 **모두** 쓸어 `t = a·rows + p·nt + c` 를 26 점에 적합하면 행당 a 는 세 shape
모두 1.00 인데 **pp 당 p 가 0.5 / 6.6 / 25.6 (S=1/2/4)** 으로 S 와 함께 약 S² 로 뛴다. 이
모델이 E626 의 H=256 레이어를 shape 0 **0.3 %**, shape 2 **2.5 %** 로 예측한다. 호출당 상수는
따로 재면 S=1 에서 0, S=2/4 에서 30~43 사이클이고 레이어의 2~5 % 뿐이며, **호출이 pp 2 개
이상을 덮으면 가려진다**. 그러니 개선 축은 **pp 마다 드레인되는 세그먼트 mvout 을 다음 pp 의
계산과 겹치는 쪽**이고, 고치면 H=256 레이어에서 최대 6.3 % 다. 주의: N 만 쓸면 `rows ∝ nt` 가
완전 공선이라 pp 항이 **식별되지 않는다** — E630 이 K 만 쓸어 이 항을 놓쳤다. **S=8 은 다시 재도 열리지 않는다 (E631)**:
현재 RTL 에서 reps 차분으로 적재를 걷어내고 재면 S=1/2/4 가 상한을 **정확히 100 %** 달성
(1.00 / 1.89 / 3.40×, 행당 1.0547 cyc — 실리콘의 1.000 과 5.5 % 일치)하는 반면 **S=8 만
op 당 +1.73 사이클의 벌금**이 붙어 **3.66× (상한의 65 %)** 에 그치고, 절대 시간마저 S=4 보다
나쁘다(237 vs 201). 값 검사(`SSExecRS`)로 S=8 이 **정확함**을 확인했으니 깨진 설정의 수치가
아니다. 그러니 **16×16 은 4×64 까지만 접는다** — S=4 는 상한을 다 쓰고, S=8 은 65 % 에
+20.22 %p LUT 와 2 배의 가중치 적재 명령을 얹는다. 벌금의 기전은 미상(뱅크 겹침도 정확성도
아님; 세그먼트 8 개의 `ssSel` 리덕션 깊이가 후보).
**병목의 위치는 하드웨어가 바뀔 때마다 다시 재라** — 여기서는 개선 계획 자체가 무효화됐다.

**면적 (OOC, `SSExecArea` 16×16, `accBanks = maxSeg`):** S=1 127,502 LUT → S=2 +0.29 %p →
S=4 +7.44 %p → S=8 +20.22 %p. **S=2 는 사실상 공짜, S=4 는 합리적, S=8 은 한계적**
(1 %p LUT 당 속도가 313 % → 11.6 % → 5.9 %). Scratchpad 는 이 수치에 안 들어 있고,
S=8 은 `sp_banks >= 8` 도 요구한다.

**하지 말 것 (측정으로 확인된 것들):**
- **`mesh_cntl_signals_q` 를 깊게 하지 말 것.** +1(stock) 2180 → +2 2264 → +4 2435 사이클로
  **단조로 느려진다** (E587). op 경계의 `cntl_ready=0` 은 원인이 아니라 증상이고, 큐에 여유를
  주면 발행이 mesh 보다 앞서 나가 뱅크 역압을 키운다. **공유 자원 앞에서는 역압이 스케줄러다.**
- **범위 밖 shape 를 발행하지 말 것.** ISA 필드는 3 비트인데 mesh 의 shape 포트는
  `log2Up(segsSupported.size)` 비트라, `shapeIdx >= 4` 는 유효한 shape 로 **aliasing** 된다.
  hang 은 아니고 오답이다. clamp 하지 않는 것이 의도적 선택 — 조용한 강등보다 오답이 잡힌다.
- **`OUTPUT_STATIONARY` 와 마찬가지로, 값 검사가 없는 하네스를 믿지 말 것.** `SSExecSP` 는
  완료 계수 하네스라 주소 버그를 숨긴다. 값은 R-grid(`SSExecRS`)·`SSExecSeqShapes` 로 본다.

**도구.** 시뮬은 **Verilator 5.029**(`scratchpad/oss-cad-suite/bin`) + VCD 파싱이 표준이다 —
xsim 의 XMR `$display` 서사는 창-덤프 아티팩트를 냈고 Verilator 가 그것을 판정했다(E574d).
xsim 을 쓸 때는 `-d RANDOMIZE_REG_INIT -d RANDOMIZE_MEM_INIT -d RANDOM=0` 이 필수다.
보드 측정용 프로그램은 `experiments/tests/ssgemv.c`(GEMV, `--noldx`/`--fence`)와
`ssmm.c`(M>1 matmul, `--nostore`/`--neg`).

**shape 선택은 `grt_ss_plan()` 에 구현돼 있다 (E638).** shape(M 규칙) + stride + N 분할을
한 함수로 묶었고, 규칙을 뽑은 격자 밖의 11 개 종횡비 셀에서 **평균 손해 0.56 %, 8 셀 정확**
(전 셀 PASS, 음성 대조 검출). 두 가지 주의:
**① 용량이 M 규칙을 덮는다** — 뱅크당 필요 행은 `N·K/(16·S)` 라 **접기는 용량 기제이기도
하며**, `M>=16` 이 원하는 shape 0 은 세그먼트가 하나라 stride 를 못 써서 큰 K·N 에서 상주
불가다. 그럴 때는 **더 접어야** 한다 (계획기가 그렇게 한다; 이 검사가 없어 처음엔 상주
불가능한 shape 을 고르고 `nsplit=1` 이라 보고하는 버그가 있었다).
**적용 범위: 단계당 `K·N <= ~256 KB` (E740).** `grt_ss_gemv` 는 B 가 scratchpad(256 KB)에
**상주**해야 하므로 그보다 큰 단계에는 적용 자체가 안 된다 — `[384x768]`(288 KB)은 S=2 에
최대 stride 4 를 줘도 8,192 행 < 필요 9,216 행이라 상주 불가다. 실제 H>=384 의 FFN 을 하려면
K/J 분할이 필요하고 그러면 적재가 분할마다 반복된다. (참고: 이 저장소의 `tile` 기준선은
`BFLAT=4096` 부터 B 를 통째로 깔아 **192 KB** 에서 먼저 막힌다.)

**검증된 동작 범위는 `K·N <= 147.5Ki` (384x384) 다 (E641/E643) — 단 그 안에서도 안전하지 않다.** E670 에서 `K·N = 128 KB` 짜리 셀이 `--ovl --rot` 모드로 wedge 했다. 즉 상한은 **모드에도 달렸고** "이 범위는 검증됐다" 는 그때 돌린 모드에 한한 말이다. 그 위에서 보드가 wedge 하고,
**범인은 접힌 shape 2 (4x64) 로 확정됐다** — 같은 셀에서 shape 1 은 6095 cyc PASS 다 (E643,
`ONLY_SHAPE` 로 갈랐다). wedge 는 **wedge 하는 셀에 대해서는 결정적**이지만 (같은 셀을 다시
돌리면 다시 죽는다) **정상 셀도 낮은 확률로 wedge 한다** — 이 세션에서 수백 회 중 2 회,
직전까지 수십 번 정상이던 `[256×256]` 이 첫 실행에서 죽고 재프로그램 후 다시 정상이었다
(E734). "이 셀은 안전하다" 는 영구 보증이 아니다. 그리고 **계산 가능한 어떤 배치 변수로도 예측되지 않는다**: 여섯 사례가 전부
뱅크 0 에 닿는데 wedge 는 둘뿐이고, B 배치가 같고 M 만 다른 두 짝의 방향이 서로 반대다
(672 블록 M=1 OK/M=2 wedge, 784 블록 M=1 wedge/M=8 OK). `grt_ss_plan` 이 `risky` 로
표시만 한다 — M<=4 에서는 shape 2 가 38~53 % 빠르므로 무조건 막지 않는다.
큰 K·N 을 보드에서 쓸 때는 **셀 하나씩 돌리고 사이에 ping 을 볼 것** — wedge 한 번이
재프로그램 + sshd 대기로 20 분이고, 스윕으로 묶으면 뒤쪽 데이터를 통째로 잃는다.

**② 경계는 (M, K·N) 결합이다 (E639/E640)** — `M >= 8 && K·N > 128 KiB` 면 **8x32** 가 4x64 를
이기고, **마진이 K·N 과 함께 가파르게 커진다**: 144Ki 11.8 %, 168Ki 25.4~26.0 %,
192~196Ki **36.1~37.1 %** (E640, 한 바이너리 안의 추세). 임계 바로 위만 보면 5 % 짜리
규칙처럼 보이지만 조금만 더 가면 **37 %** 다. K·N 을 맞춘 짝(448x384 vs 384x448 등)이
0.6~0.8 점 안에서 일치하므로 변수는 K·N 이고 종횡비가 아니다. K·N 이 20/20 셀을 깨끗이 가르고 `N/K`·`K`·`N` 은 못 가른다;
경계는 측정으로 (122.9k, 147.5k]. **M 조건이 필수** — 같은 K·N 에서 M<=4 는 4x64 가
**30~50 %** 이긴다. (E638 이 "M=8 은 종횡비로 뒤집히는 동률" 이라고 적은 것은 세 셀의
측정 오염이었고 철회했다.) **A 자리는 넘침과 같이 봐야 한다**: A 발자국은 FSM 규약상 `ch·M` 행이고, 세그먼트 넘침이
뱅크 0 으로 **감기면** 그 낮은 행들을 차지한다. 둘이 겹치면 A 가 B 를 덮어 **보드가 wedge
된다** (E639). `grt_ss_plan` 이 `a_rows`/`spill_rows`/`a_base` 를 계산하니 **`a_base < 0`
이면 그 조합을 쓰지 말 것** (E640). A 는 뱅크 위쪽에 둔다.

**GEMV 시간 모델 (bit12, M=1, 상주) — 측정 없이 shape 을 고를 수 있다 (E632/E633).**

    t(shape, K, N) = a·rows + p·nt + c
      h = 16/S,  nt = N/(16S),  ops = nt·(K/h),  rows = ops·(h+1)
      16×16: a=1.0005 p= 0.5 c=−35.5 |  8×32: a=0.9390 p= 6.6 c=+1.6 |  4×64: a=1.0159 p=25.6 c=−14.4

적합 26 점 + **표본 밖 6 점에서 평균 1.23 % / 최악 4.29 %** (사전등록 5 %/10 % 통과).
행당 비용은 세 shape 모두 1.00 이고 **pp 블록당 비용만 `p = 8.34·(S−1)`** 로 뛴다 (E634) —
그것이 E626 의 "이론 상한의 89.9 %" 에서 빠진 10 % 이고, 없애면 최대 6.3 % 다. **S=4 는 배치 크기에 견고한 설계점이다 (E637).** M=1, 2, 4 세 값에서 모두 S=4 가 최적이고
S=8 은 어디서도 이기지 않는다. 기전은 **D 행 항** `c_D·h` 가 S=4 에서 최소이고 S=8 에서
되오르는 것 (16.8 / 8.7 / 4.96 / **6.14**) — 접기는 D 행 수를 `16/S` 로 줄이지만 **행당 비용이
오르고** S=8 에서 곱이 뒤집힌다. 왜 오르는지는 미상이다. (E636 이 세운
`op 당 = max(h+M, 4)` 바닥 법칙은 M=1 세 점에 맞춘 것이었고 **M 을 바꾼 첫 시험에서
반증됐다** — S=8 이 M=4 에서 S=4 를 이길 것이라 예측했으나 지지 않았다. 철회.)

S 의존은 **초선형**으로, 시뮬에서 블록당 초과가
S=1/2/4/8 에 0 / 5.1 / 28.7 / **123.2** 이다 (E635; 실리콘의 0.5/6.6/25.6 과 일치 — 두 플랫폼이
같은 양을 잰다). 지수는 1.6~1.7 이고, 이것이 **E631 에서 S=8 이 S=4 보다 느렸던 이유**다.
**mvout 은 범인이 아니다** — store 완료가 2 사이클 간격으로 나오고 버스트 전체가 0/1/6/14
사이클뿐이다(E635 가 E634 의 "store 당 8.34" 기전을 반증). 초과분은 **계산 구간의 블록
경계**에 있다: tt=0 의 비누산 preload, mesh 드레인, 누산기 쓰기 포트가 남은 후보이고
다음 탐침은 compute 발행 간격의 VCD 다. 지금까지의
`M<=8 -> S=4` 규칙은 **M 축**, 이 모델은 **K·N 축**이다.

**계측기 — 어느 경로로 재는지가 무엇을 재는지를 정한다.** 소프트웨어 타일 경로는 스텝당
128 RoCC 명령을 발행해 **발행 바닥에 묶인다**: bit8 에서 ssmm S=4 가 M=1,4,8 전부
8.77 cyc/명령으로 평평하고, ssgemv tile 의 10.56 과 소수점 둘째 자리까지 같은 구조다(E588).
계산을 재려면 **FSM 경로(`ssmm --fsm`)를 쓸 것** — `ShapeshiftGemvLoop` 는 이름과 달리
처음부터 `cfgM` 을 지원한다(preload c_rows, compute a_rows, aRow/accRow). E588 이 "M>1
계측기가 없다"고 쓴 것은 **코드를 읽지 않고 이름으로 가정한** 오류이고 E589 에서 정정했다.
교훈은 그대로 남는다: **E577 이 하드웨어를 4.9 배 빠르게 만들면서 기존 측정 도구를 무효화했다**
— 같은 바이너리가 두 비트스트림에서 다른 것을 잰 가장 선명한 사례.
