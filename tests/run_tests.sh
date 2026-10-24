#!/bin/sh
set -e
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"

fail() {
    echo "FAIL: $1" >&2
    exit 1
}

[ -x ./race ] || fail "нет бинарника race"
[ -x ./race_cond ] || fail "нет бинарника race_cond"

echo "== тест 1: короткая гонка (семафоры) =="
./race -c configs/short.conf -l tests/short.log -q > tests/short.out
grep -q "ИТОГОВАЯ КЛАССИФИКАЦИЯ МАШИН" tests/short.out || fail "нет классификации в stdout"
grep -q "КЛАССИФИКАЦИЯ КОМАНД" tests/short.out || fail "нет командной классификации"
grep -q "ИТОГОВАЯ КЛАССИФИКАЦИЯ МАШИН" tests/short.log || fail "нет классификации в журнале"
grep -q "Судья: гонка началась" tests/short.out || fail "нет старта"
if grep -E 'маршрут \(стоимость [0-9]+\):[[:space:]]*$' tests/short.out >/dev/null; then
    fail "пустой маршрут при ненулевой стоимости"
fi

echo "== тест 2: воспроизводимость seed =="
./race -n 2 -p 2 -k 1 -m 20 -s 7 -d 0 -q -l tests/seed1.log > tests/seed1.out
./race -n 2 -p 2 -k 1 -m 20 -s 7 -d 0 -q -l tests/seed2.log > tests/seed2.out
# Итоговые строки классификации должны совпасть
tail -n 12 tests/seed1.out > tests/seed1.tail
tail -n 12 tests/seed2.out > tests/seed2.tail
cmp tests/seed1.tail tests/seed2.tail || fail "одинаковый seed дал разный итог"

echo "== тест 3: условные переменные =="
./race_cond -c configs/short.conf -l tests/cond.log -q > tests/cond.out
grep -q "ИТОГОВАЯ КЛАССИФИКАЦИЯ МАШИН" tests/cond.out || fail "race_cond без классификации"

echo "== тест 4: справка =="
./race -h | grep -q "Использование" || fail "нет справки"

echo "Все тесты прошли."
