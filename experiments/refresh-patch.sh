#!/bin/bash
# shapeshift 패치를 워킹 트리에서 다시 만든다.
#
# 왜 스크립트인가: `git diff` 는 **추적되지 않은 파일을 조용히 빠뜨린다**.
# shapeshift 는 대부분이 새 파일(Shapeshift*.scala 13 개)이라 그냥 `git diff` 로 만들면
# 내용의 80 % 가 사라진 패치가 나오고, `git apply --check -R` 은 그래도 통과한다
# (역적용 검증은 "패치에 든 것" 만 보지 "빠진 것" 은 못 본다). E490 참조.
set -e
G=generators/gemmini; P=$(pwd)/patches

# E848a: 이 스크립트는 **트리 -> 패치** 단방향이다. 트리를 되돌린 뒤(예:
# `git checkout <file>`) 이것을 돌리면 **빈 diff 가 패치를 덮어쓴다**. 그리고
# patches/gemmini-*.patch 는 대부분 git 추적 밖이라 되살릴 수단이 없다 —
# 실제로 gemmini-accscale-norm0.patch 를 124 -> 0 줄로 날렸고, 다른 패치에
# 우연히 사본이 있어 겨우 복구했다. 덮어쓰기 전에 백업하고, 크게 줄면 멈춘다.
B=$P/.backup; mkdir -p $B
for f in $P/gemmini-shapeshift.patch $P/gemmini-accscale-norm0.patch; do
  [ -f "$f" ] && cp "$f" "$B/$(basename $f).prev"
done

# E850: `src/main/scala/rocket.scala` 는 이 저장소에서 **미커밋 상태가 정상**이다
# (shapeshift 12 + 다중 가속기 10 개 config 와 헬퍼들). `git checkout` 한 번이면
# 전부 사라지고 git 에 staged 이력이 없어 복구 수단이 없다 — 실제로 그렇게 잃었다.
# 세대를 남겨 둔다: 최신 3 개를 돌려 보관한다.
R=src/main/scala/rocket.scala
if [ -f "$R" ]; then
  for i in 2 1; do [ -f "$B/rocket.scala.$i" ] && cp "$B/rocket.scala.$i" "$B/rocket.scala.$((i+1))"; done
  cp "$R" "$B/rocket.scala.1"
fi

cd $G
git add -N $(git ls-files --others --exclude-standard -- src/main/scala/gemmini)
# E851: VectorScalarMultiplier 는 `ScaleArguments` 의 **위치 기반 패턴 매칭**을 쓰므로
# 그 케이스 클래스에 필드를 더하면 같이 고쳐야 한다. AccScaleEquiv 는 새 하네스 파일이다.
# (둘 다 목록에 없어 첫 갱신이 "패치와 워킹 트리가 다르다" 로 걸렸다 — 목록이 하드코딩
#  이라는 것 자체가 이 스크립트의 약점이고, 파일을 더할 때마다 여기를 봐야 한다.)
git diff -- src/main/scala/gemmini/Shapeshift*.scala \
             src/main/scala/gemmini/AccScaleEquiv.scala \
             src/main/scala/gemmini/{Configs,Controller,ExecuteController,GemminiConfigs,GemminiISA,MeshWithDelays,ReservationStation,Scratchpad,VectorScalarMultiplier}.scala \
    > /tmp/ss_body.patch
git diff -- src/main/scala/gemmini/AccumulatorScale.scala > /tmp/acc_body.patch
git reset -q
cd - >/dev/null

# 새 패치가 이전보다 절반 아래로 줄면 멈춘다 — 되돌린 트리에서 재생성한 신호다.
for pair in "/tmp/acc_body.patch:$P/gemmini-accscale-norm0.patch" "/tmp/ss_body.patch:$P/gemmini-shapeshift.patch"; do
  new=${pair%%:*}; cur=${pair##*:}; prev=$B/$(basename $cur).prev
  if [ -f "$prev" ]; then
    n=$(wc -l < "$new"); p=$(wc -l < "$prev")
    if [ "$p" -gt 20 ] && [ "$n" -lt $((p / 2)) ]; then
      echo "중단 — $(basename $cur) 가 $p -> $n 줄로 급감했다."
      echo "  트리를 되돌린 뒤 재생성한 것은 아닌지 확인할 것 (E848a)."
      echo "  이전 판은 $prev 에 있다. 의도한 축소면 그 파일을 지우고 다시 실행."
      exit 1
    fi
  fi
done
cp /tmp/acc_body.patch $P/gemmini-accscale-norm0.patch

python3 -c "
h=open('$P/gemmini-shapeshift.patch').read().split('diff --git')[0]
open('$P/gemmini-shapeshift.patch','w').write(h+open('/tmp/ss_body.patch').read())"

# 검증은 왕복으로 한다: 깨끗한 트리에 적용해서 워킹 트리가 그대로 나오는지 본다.
cd $G; rm -rf /tmp/gem-verify; git worktree prune
git worktree add -q --detach /tmp/gem-verify HEAD
( cd /tmp/gem-verify && git apply $P/gemmini-shapeshift.patch && git apply $P/gemmini-accscale-norm0.patch )
if diff -r -q /tmp/gem-verify/src/main/scala/gemmini src/main/scala/gemmini; then
  echo "OK — 패치가 워킹 트리를 온전히 복원한다 ($(wc -l < $P/gemmini-shapeshift.patch) 줄, 새 파일 $(grep -c 'new file mode' $P/gemmini-shapeshift.patch) 개)"
else
  echo "실패 — 패치와 워킹 트리가 다르다"; exit 1
fi
git worktree remove --force /tmp/gem-verify; git worktree prune
