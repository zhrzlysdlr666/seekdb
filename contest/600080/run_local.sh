#!/usr/bin/env bash
# 600080（赛题四：全文索引调优）本地一键复现。
#
# 用法（在仓库根目录）：
#   bash contest/600080/run_local.sh              # 完整跑：编译 + 起库 + 导入 2 万篇 + 建索引 + 分词/查询 + 算分
#   SKIP_BUILD=1 bash contest/600080/run_local.sh # 复用已有二进制，只重测
#   KEEP_DB=1    bash contest/600080/run_local.sh # 保留 fts_large_bench 库（第二次起配合 SKIP_LOAD=1 迭代更快）
#
# 环境变量：
#   PORT         seekdb 端口，默认 2881
#   LABEL        报告标签，默认 local
#   SKIP_LOAD    1 = 不重建表/索引，直接跑分词与查询（必须已有数据）
#   SEEKDB_ARGS  传给 seekdb 二进制的额外参数，默认空
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="${PORT:-2881}"
LABEL="${LABEL:-local}"
SKIP_LOAD="${SKIP_LOAD:-0}"
SKIP_BUILD="${SKIP_BUILD:-0}"
RUN_DIR="${RUN_DIR:-$HOME/seekdb-run}"

log() { echo "[600080] $*" >&2; }

# 1) 编译
if [ "${SKIP_BUILD}" != "1" ]; then
  cd "${REPO_ROOT}"
  if [ ! -x build/bin/src/observer/seekdb ]; then
    log "编译 seekdb（首次约 30-60 分钟）..."
    ./build.py init
    ./build.py build seekdb -j"$(nproc)"
  else
    log "已有二进制，跳过编译"
  fi
fi

BIN="${REPO_ROOT}/build/bin/src/observer/seekdb"
[ -x "${BIN}" ] || { log "找不到 ${BIN}，先去掉 SKIP_BUILD 编译一次"; exit 1; }

# 2) 起一个干净的实例（seekdb 以「当前工作目录」作为数据目录）
mkdir -p "${RUN_DIR}"
cp -f "${BIN}" "${RUN_DIR}/seekdb"
cd "${RUN_DIR}"
if pgrep -f "${RUN_DIR}/seekdb" > /dev/null 2>&1; then
  log "停掉已在跑的实例"
  pkill -f "${RUN_DIR}/seekdb" || true
  sleep 2
fi
log "启动 seekdb（port=${PORT}）..."
# shellcheck disable=SC2086
nohup "${RUN_DIR}/seekdb" --port "${PORT}" ${SEEKDB_ARGS:-} > "${RUN_DIR}/run.log" 2>&1 &
for i in $(seq 1 180); do
  if mysql -h127.0.0.1 -P"${PORT}" -uroot -e 'SELECT 1' > /dev/null 2>&1; then
    log "实例已就绪（${i}s）"
    break
  fi
  sleep 1
  if [ "$i" = "180" ]; then
    log "实例 180s 内没起来，看 ${RUN_DIR}/run.log"
    tail -30 "${RUN_DIR}/run.log" >&2 || true
    exit 1
  fi
done

# 3) 跑基准 + 算分
export MYSQL="mysql -h127.0.0.1 -P${PORT} -uroot -N -s -A --default-character-set=utf8mb4"
export MYSQL_VERBOSE="mysql -h127.0.0.1 -P${PORT} -uroot -A --default-character-set=utf8mb4"
REPORT="${RUN_DIR}/report-$(date +%Y%m%d-%H%M%S).txt"
log "开始基准（report=${REPORT}）..."
LABEL="${LABEL}" ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 \
SAMPLES=3 WARMUP=30 SKIP_LOAD="${SKIP_LOAD}" OUTPUT="${REPORT}" \
bash "${HERE}/fts_large_bench.sh" > "${RUN_DIR}/bench.out" 2>&1

log "报告尾部："
tail -30 "${REPORT}" >&2

log "算分："
python3 "${HERE}/fts_large_bench_score.py" "${REPORT}" | tee "${RUN_DIR}/score.txt"

log "完成。报告：${REPORT}；分数：${RUN_DIR}/score.txt"
