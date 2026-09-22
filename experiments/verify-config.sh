#!/bin/bash
# E850f: 재구성한 config 가 보존된 원본 Verilog 와 같은지 본다.
#
# 두 축을 **둘 다** 봐야 한다:
#   ① 모듈 본문   — Gemmini / Gemmini_1 / Gemmini_2 의 내용 (파라미터가 맞는가)
#   ② 배치        — 각 RocketTile 안에 Gemmini 가 몇 개인가 (어느 타일에 붙는가)
# ②를 빼먹으면 2 코어 다중 가속기 config 가 **통과한다** — 모듈 본문은 같고 인스턴스
# 위치만 다르기 때문이다. E850c 의 20 개는 전부 1 코어라 ②가 자명했고, 그래서 기준의
# 구멍이 안 드러났다.
#
# 사용: experiments/verify-config.sh <config> [orig-v 디렉터리]
#
# ⚠ 기준 디렉터리의 기본값이 `/tmp/sssim/orig-v` 인데 **/tmp 는 청소된다** (E857 에서
#   9 일 만에 45 개를 통째로 잃었다). 보존할 기준 산출물은 /tmp 밖에 둘 것.
# `@[file line:col]` 주석은 벗긴다 — Chisel 이 주석 한 줄만 넣어도 전부 renumber 한다.
set -u
c="$1"
orig="${2:-/tmp/sssim/orig-v}/$c.v"
new="${VERIFY_NEW:-workspace/$c/system-u280.v}"   # VERIFY_NEW 는 음성 대조용

[ -f "$orig" ] || { echo "  $c: 원본 없음 — 검증 불가"; exit 2; }
[ -f "$new" ]  || { echo "  $c: 생성물 없음"; exit 2; }

strip() { sed 's|// @\[.*\]||'; }
body()  { awk "/^module $2\(/,/^endmodule/" "$1" | strip; }

tot=0; det=""
for m in Gemmini Gemmini_1 Gemmini_2 Gemmini_3; do
  grep -q "^module $m(\$" "$orig" || continue
  if ! grep -q "^module $m(\$" "$new"; then det="$det $m=없음"; tot=$((tot+1)); continue; fi
  # Controller.scala: 의 assertion 문자열은 줄번호를 담고 있어 주석 제거로 안 없어진다
  d=$(diff <(body "$orig" $m) <(body "$new" $m) | grep '^[<>]' | grep -cv 'Controller.scala:')
  tot=$((tot+d)); det="$det $m=$d"
done

# ② 배치: 타일마다 Gemmini 인스턴스 수
place() {
  for t in RocketTile RocketTile_1 RocketTile_2 RocketTile_3; do
    grep -q "^module $t(\$" "$1" || continue
    n=$(awk "/^module $t\(/{f=1} f&&/^endmodule/{exit} f" "$1" | grep -cE '^  Gemmini(_[0-9]+)? ')
    printf "%s:%s " $t $n
  done
}
p_o=$(place "$orig"); p_n=$(place "$new")
pl="일치"; [ "$p_o" != "$p_n" ] && { pl="**배치다름** 원본[$p_o] 신규[$p_n]"; tot=$((tot+1)); }

printf "  %-28s %-10s (%s) 배치 %s\n" "$c" \
  "$([ "$tot" = "0" ] && echo 등가 || echo '**다름**')" "$det" "$pl"
exit $([ "$tot" = "0" ] && echo 0 || echo 1)
