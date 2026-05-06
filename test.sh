#!/usr/bin/env bash
# Интеграционные тесты для MD5-cracker API.
# Использование:
#   ./test.sh              — тестировать на localhost:8080 (docker-compose)
#   ./test.sh 30080        — тестировать на localhost:30080 (kubernetes)
#   ./test.sh 8080 10      — таймаут ожидания результата 10 секунд

set -euo pipefail

PORT="${1:-8080}"
TIMEOUT="${2:-60}"
BASE="http://localhost:${PORT}"

# ─── цвета ────────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
RESET='\033[0m'

# ─── счётчики ─────────────────────────────────────────────────────────────────
PASS=0
FAIL=0
SKIP=0

pass() {
  echo -e "  ${GREEN}✓${RESET} $1"
  PASS=$((PASS + 1))
}
fail() {
  echo -e "  ${RED}✗${RESET} $1"
  FAIL=$((FAIL + 1))
}
skip() {
  echo -e "  ${YELLOW}~${RESET} $1"
  SKIP=$((SKIP + 1))
}
section() { echo -e "\n${CYAN}${BOLD}▶ $1${RESET}"; }

# ─── хелперы ──────────────────────────────────────────────────────────────────

# POST /api/hash/crack, вернуть requestId или ""
crack() {
  local body="$1"
  curl -s -w '\n%{http_code}' -XPOST "${BASE}/api/hash/crack" \
    -H 'Content-Type: application/json' \
    -d "$body"
}

# GET /api/hash/status
status() {
  local rid="$1"
  curl -s -w '\n%{http_code}' "${BASE}/api/hash/status?requestId=${rid}"
}

# Ждать READY или ERROR, вернуть тело последнего ответа
wait_result() {
  local rid="$1"
  local deadline=$((SECONDS + TIMEOUT))
  while [ $SECONDS -lt $deadline ]; do
    local out
    out=$(curl -s "${BASE}/api/hash/status?requestId=${rid}")
    local st
    st=$(echo "$out" | python3 -c "import sys,json; print(json.load(sys.stdin).get('status',''))" 2>/dev/null || echo "")
    if [ "$st" = "READY" ] || [ "$st" = "ERROR" ]; then
      echo "$out"
      return 0
    fi
    sleep 2
  done
  echo '{"status":"TIMEOUT"}'
  return 1
}

# Извлечь поле из JSON
jq_field() {
  python3 -c "import sys,json; d=json.loads(sys.stdin.read()); print(d.get('$1',''))" 2>/dev/null || echo ""
}

# Проверить HTTP-статус и строку в теле
assert_http() {
  local label="$1" want_code="$2" want_body="$3"
  local response="$4"
  local body code
  body=$(echo "$response" | head -n -1)
  code=$(echo "$response" | tail -n 1)
  if [ "$code" != "$want_code" ]; then
    fail "${label}: ожидался HTTP ${want_code}, получен ${code} (body: ${body})"
    return
  fi
  if [ -n "$want_body" ] && ! echo "$body" | grep -q "$want_body"; then
    fail "${label}: HTTP ${code} OK, но в теле нет «${want_body}» (body: ${body})"
    return
  fi
  pass "$label"
}

# ─── ожидание готовности сервиса ──────────────────────────────────────────────
wait_ready() {
  echo -e "${YELLOW}Ожидание готовности ${BASE}/healthz ...${RESET}"
  local deadline=$((SECONDS + 30))
  while [ $SECONDS -lt $deadline ]; do
    if curl -sf "${BASE}/healthz" >/dev/null 2>&1; then
      echo -e "${GREEN}Сервис доступен.${RESET}"
      return 0
    fi
    sleep 2
  done
  echo -e "${RED}Сервис не ответил за 30 секунд. Прерываю.${RESET}"
  exit 1
}

# ══════════════════════════════════════════════════════════════════════════════
# ТЕСТЫ
# ══════════════════════════════════════════════════════════════════════════════

wait_ready

# ─── 1. Базовая доступность ───────────────────────────────────────────────────
section "1. Базовая доступность"

R=$(curl -s -w '\n%{http_code}' "${BASE}/healthz")
assert_http "/healthz возвращает 200 ok" "200" "ok" "$R"

R=$(curl -s -w '\n%{http_code}' "${BASE}/metrics")
assert_http "/metrics возвращает 200" "200" "" "$R"
METRICS_BODY=$(echo "$R" | head -n -1)
if echo "$METRICS_BODY" | grep -q "http_requests_total"; then
  pass "/metrics содержит http_requests_total"
else
  fail "/metrics не содержит http_requests_total"
fi

# ─── 2. Валидация входных данных ──────────────────────────────────────────────
section "2. Валидация — POST /api/hash/crack"

assert_http "пустое тело → 400" "400" "missing body" \
  "$(curl -s -w '\n%{http_code}' -XPOST "${BASE}/api/hash/crack")"

assert_http "невалидный JSON → 400" "400" "invalid json" \
  "$(crack "not json at all")"

assert_http "hash отсутствует → 400" "400" "invalid hash" \
  "$(crack '{"maxLength":3}')"

assert_http "hash не 32 символа → 400" "400" "invalid hash" \
  "$(crack '{"hash":"abc123","maxLength":3}')"

assert_http "hash содержит не hex → 400" "400" "invalid hash" \
  "$(crack '{"hash":"zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz","maxLength":3}')"

assert_http "maxLength = 0 → 400" "400" "invalid maxLength" \
  "$(crack '{"hash":"900150983cd24fb0d6963f7d28e17f72","maxLength":0}')"

assert_http "maxLength = 9 → 400" "400" "invalid maxLength" \
  "$(crack '{"hash":"900150983cd24fb0d6963f7d28e17f72","maxLength":9}')"

assert_http "maxLength отсутствует → 400" "400" "invalid maxLength" \
  "$(crack '{"hash":"900150983cd24fb0d6963f7d28e17f72"}')"

# ─── 3. Валидация — GET /api/hash/status ──────────────────────────────────────
section "3. Валидация — GET /api/hash/status"

assert_http "requestId отсутствует → 400" "400" "requestId required" \
  "$(curl -s -w '\n%{http_code}' "${BASE}/api/hash/status")"

assert_http "несуществующий requestId → 404" "404" "not found" \
  "$(curl -s -w '\n%{http_code}' "${BASE}/api/hash/status?requestId=00000000-0000-0000-0000-000000000000")"

# ─── 4. Функциональные тесты ─────────────────────────────────────────────────
section "4. Функциональные тесты — взлом хэша"

echo "  (ожидание результатов до ${TIMEOUT}с каждый)"

# md5("a") = 0cc175b9c0f1b6a831c399e269772661
echo -e "\n  Тест: md5(\"a\"), maxLength=1"
R=$(crack '{"hash":"0cc175b9c0f1b6a831c399e269772661","maxLength": 1}')
CODE=$(echo "$R" | tail -n 1)
RID=$(echo "$R" | head -n -1 | jq_field requestId)
if [ "$CODE" = "200" ] && [ -n "$RID" ]; then
  pass "POST вернул 200 и requestId=${RID}"
  OUT=$(wait_result "$RID")
  ST=$(echo "$OUT" | jq_field status)
  RES=$(python3 -c "import sys,json; d=json.loads(sys.stdin.read()); print(','.join(d.get('results',[])))" <<<"$OUT" 2>/dev/null || echo "")
  if [ "$ST" = "READY" ] && echo "$RES" | grep -q "a"; then
    pass "Результат: READY, results=[${RES}] (ожидалось «a»)"
  else
    fail "Результат: status=${ST}, results=[${RES}] (ожидалось READY + «a»)"
  fi
else
  fail "POST вернул HTTP ${CODE}"
  skip "Пропуск проверки результата"
fi

# md5("abc") = 900150983cd24fb0d6963f7d28e17f72
echo -e "\n  Тест: md5(\"abc\"), maxLength=3"
R=$(crack '{"hash":"900150983cd24fb0d6963f7d28e17f72","maxLength":3}')
CODE=$(echo "$R" | tail -n 1)
RID=$(echo "$R" | head -n -1 | jq_field requestId)
if [ "$CODE" = "200" ] && [ -n "$RID" ]; then
  pass "POST вернул 200 и requestId=${RID}"
  OUT=$(wait_result "$RID")
  ST=$(echo "$OUT" | jq_field status)
  RES=$(python3 -c "import sys,json; d=json.loads(sys.stdin.read()); print(','.join(d.get('results',[])))" <<<"$OUT" 2>/dev/null || echo "")
  if [ "$ST" = "READY" ] && echo "$RES" | grep -q "abc"; then
    pass "Результат: READY, results=[${RES}] (ожидалось «abc»)"
  else
    fail "Результат: status=${ST}, results=[${RES}] (ожидалось READY + «abc»)"
  fi
else
  fail "POST вернул HTTP ${CODE}"
  skip "Пропуск проверки результата"
fi

# md5("z") = fbade9e36a3f36d3d676c1b808451dd7, maxLength=1 (однобуквенный)
echo -e "\n  Тест: md5(\"z\"), maxLength=1"
R=$(crack '{"hash":"fbade9e36a3f36d3d676c1b808451dd7","maxLength":1}')
CODE=$(echo "$R" | tail -n 1)
RID=$(echo "$R" | head -n -1 | jq_field requestId)
if [ "$CODE" = "200" ] && [ -n "$RID" ]; then
  pass "POST вернул 200 и requestId=${RID}"
  OUT=$(wait_result "$RID")
  ST=$(echo "$OUT" | jq_field status)
  RES=$(python3 -c "import sys,json; d=json.loads(sys.stdin.read()); print(','.join(d.get('results',[])))" <<<"$OUT" 2>/dev/null || echo "")
  if [ "$ST" = "READY" ] && echo "$RES" | grep -q "z"; then
    pass "Результат: READY, results=[${RES}] (ожидалось «z»)"
  else
    fail "Результат: status=${ST}, results=[${RES}] (ожидалось READY + «z»)"
  fi
else
  fail "POST вернул HTTP ${CODE}"
  skip "Пропуск проверки результата"
fi

# ─── 5. Hash не найден ────────────────────────────────────────────────────────
section "5. Hash не найден"

# Случайный хэш который точно не является md5 ни одного слова длиной ≤1
echo -e "\n  Тест: несуществующий хэш, maxLength=1"
R=$(crack '{"hash":"ffffffffffffffffffffffffffffffff","maxLength":1}')
CODE=$(echo "$R" | tail -n 1)
RID=$(echo "$R" | head -n -1 | jq_field requestId)
if [ "$CODE" = "200" ] && [ -n "$RID" ]; then
  pass "POST вернул 200 и requestId=${RID}"
  OUT=$(wait_result "$RID")
  ST=$(echo "$OUT" | jq_field status)
  N=$(python3 -c "import sys,json; d=json.loads(sys.stdin.read()); print(len(d.get('results',[])))" <<<"$OUT" 2>/dev/null || echo "-1")
  if [ "$ST" = "READY" ] && [ "$N" = "0" ]; then
    pass "Результат: READY, results=[] (пустой — хэш не найден)"
  else
    fail "Результат: status=${ST}, results_count=${N} (ожидалось READY + пустой массив)"
  fi
else
  fail "POST вернул HTTP ${CODE}"
  skip "Пропуск проверки результата"
fi

# ─── 6. Регистронезависимость хэша ───────────────────────────────────────────
section "6. Регистронезависимость хэша"

echo -e "\n  Тест: хэш в верхнем регистре → тот же результат"
R=$(crack '{"hash":"900150983CD24FB0D6963F7D28E17F72","maxLength":3}')
CODE=$(echo "$R" | tail -n 1)
RID=$(echo "$R" | head -n -1 | jq_field requestId)
if [ "$CODE" = "200" ] && [ -n "$RID" ]; then
  pass "POST с верхним регистром вернул 200, requestId=${RID}"
  OUT=$(wait_result "$RID")
  ST=$(echo "$OUT" | jq_field status)
  RES=$(python3 -c "import sys,json; d=json.loads(sys.stdin.read()); print(','.join(d.get('results',[])))" <<<"$OUT" 2>/dev/null || echo "")
  if [ "$ST" = "READY" ] && echo "$RES" | grep -q "abc"; then
    pass "Результат: READY, results=[${RES}] (регистр нормализован)"
  else
    fail "Результат: status=${ST}, results=[${RES}]"
  fi
else
  fail "POST вернул HTTP ${CODE}"
  skip "Пропуск проверки результата"
fi

# ─── 7. Повторный запрос того же хэша ────────────────────────────────────────
section "7. Повторный запрос того же хэша"

echo -e "\n  Тест: два одинаковых запроса → разные requestId, оба READY"
R1=$(crack '{"hash":"0cc175b9c0f1b6a831c399e269772661","maxLength":1}')
R2=$(crack '{"hash":"0cc175b9c0f1b6a831c399e269772661","maxLength":1}')
RID1=$(echo "$R1" | head -n -1 | jq_field requestId)
RID2=$(echo "$R2" | head -n -1 | jq_field requestId)
if [ -n "$RID1" ] && [ -n "$RID2" ] && [ "$RID1" != "$RID2" ]; then
  pass "Два запроса дали разные requestId (${RID1} ≠ ${RID2})"
  OUT1=$(wait_result "$RID1")
  OUT2=$(wait_result "$RID2")
  ST1=$(echo "$OUT1" | jq_field status)
  ST2=$(echo "$OUT2" | jq_field status)
  if [ "$ST1" = "READY" ] && [ "$ST2" = "READY" ]; then
    pass "Оба запроса завершились READY"
  else
    fail "Статусы: ${ST1}, ${ST2} (ожидалось READY, READY)"
  fi
else
  fail "Не удалось получить два разных requestId (rid1=${RID1}, rid2=${RID2})"
  skip "Пропуск проверки результатов"
fi

# ─── 8. Параллельные запросы ─────────────────────────────────────────────────
section "8. Параллельные запросы"

echo -e "\n  Тест: 3 параллельных запроса → все завершаются"
HASHES=(
  '{"hash":"0cc175b9c0f1b6a831c399e269772661","maxLength":1}' # md5(a)
  '{"hash":"92eb5ffee6ae2fec3ad71c777531578f","maxLength":1}' # md5(b)
  '{"hash":"4a8a08f09d37b73795649038408b5f33","maxLength":1}' # md5(c)
)
RIDS=()
for body in "${HASHES[@]}"; do
  R=$(crack "$body")
  RID=$(echo "$R" | head -n -1 | jq_field requestId)
  RIDS+=("$RID")
done

ALL_OK=1
for i in 0 1 2; do
  RID="${RIDS[$i]}"
  if [ -z "$RID" ]; then
    fail "Параллельный запрос $((i + 1)): не получен requestId"
    ALL_OK=0
    continue
  fi
  OUT=$(wait_result "$RID")
  ST=$(echo "$OUT" | jq_field status)
  RES=$(python3 -c "import sys,json; d=json.loads(sys.stdin.read()); print(','.join(d.get('results',[])))" <<<"$OUT" 2>/dev/null || echo "")
  if [ "$ST" = "READY" ]; then
    pass "Запрос $((i + 1)) (${RID}): READY, results=[${RES}]"
  else
    fail "Запрос $((i + 1)) (${RID}): status=${ST}"
    ALL_OK=0
  fi
done

# ─── 9. Метрики после нагрузки ───────────────────────────────────────────────
section "9. Метрики"

M=$(curl -s "${BASE}/metrics")
REQS=$(echo "$M" | grep '^http_requests_total' | head -1)
if [ -n "$REQS" ]; then
  pass "http_requests_total присутствует: ${REQS}"
else
  fail "http_requests_total не найден в /metrics"
fi

CREATED=$(echo "$M" | grep '^crack_requests_created_total' | head -1)
if [ -n "$CREATED" ]; then
  pass "crack_requests_created_total присутствует: ${CREATED}"
else
  fail "crack_requests_created_total не найден в /metrics"
fi

DONE=$(echo "$M" | grep '^crack_tasks_completed_total' | head -1)
if [ -n "$DONE" ]; then
  pass "crack_tasks_completed_total присутствует: ${DONE}"
else
  fail "crack_tasks_completed_total не найден в /metrics"
fi

# ─── 10. Некорректные HTTP методы ────────────────────────────────────────────
section "10. Некорректные HTTP методы"

assert_http "GET /api/hash/crack → 405" "405" "" \
  "$(curl -s -w '\n%{http_code}' "${BASE}/api/hash/crack")"

assert_http "POST /api/hash/status → 405" "405" "" \
  "$(curl -s -w '\n%{http_code}' -XPOST "${BASE}/api/hash/status")"

assert_http "GET /nonexistent → 404" "404" "" \
  "$(curl -s -w '\n%{http_code}' "${BASE}/nonexistent")"

# ─── Итог ─────────────────────────────────────────────────────────────────────
echo -e "\n${BOLD}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RESET}"
TOTAL=$((PASS + FAIL + SKIP))
echo -e "${BOLD}Итог: ${TOTAL} тестов | ${GREEN}${PASS} passed${RESET} | ${RED}${FAIL} failed${RESET} | ${YELLOW}${SKIP} skipped${RESET}"
echo -e "${BOLD}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RESET}"

[ $FAIL -eq 0 ]
