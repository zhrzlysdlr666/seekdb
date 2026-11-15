#!/usr/bin/env bash
# 600080（赛题四：全文索引调优）本地一键复现。
#
# 直接用仓库自带的评测脚本 tools/benchmark/fts_large_bench.sh（与判分侧同一份，
# 本分支没有改动过它），因此这里只负责：编译 → 起库 → 跑基准 → 算分。
#
# 用法（在仓库根目录）：
#   bash contest/600080/run_local.sh                 # 编译 + 完整跑（导入 2 万篇 + 建索引 + 分词 + 查询）
#   SKIP_BUILD=1 bash contest/600080/run_local.sh    # 复用已有二进制
#   SKIP_LOAD=1  SKIP_BUILD=1 bash contest/600080/run_local.sh  # 复用数据和索引，只重测分词/查询
#
# 环境变量：
#   PORT         实例端口，默认 2881（seekdb 默认端口）
#   LABEL        报告标签，默认 local
#   SKIP_LOAD    1 = 不重建表/索引（要求库里已有数据）
#   RUN_DIR      运行目录（也是 seekdb 的数据目录），默认 $HOME/seekdb-run
#   SEEKDB_ARGS  额外启动参数，仅在你确知支持时使用
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BENCH_DIR="${REPO_ROOT}/tools/benchmark"
PORT="${PORT:-2881}"
LABEL="${LABEL:-local}"
SKIP_LOAD="${SKIP_LOAD:-0}"
SKIP_BUILD="${SKIP_BUILD:-0}"
RUN_DIR="${RUN_DIR:-$HOME/seekdb-run}"

log() { echo "[600080] $*" >&2; }

find_bin() {
  for p in "${REPO_ROOT}/build/bin/src/observer/seekdb" \
           "${REPO_ROOT}/build_release/src/observer/seekdb" \
           "${REPO_ROOT}/build_release/src/observer/seekdb/seekdb"; do
    if [ -x "$p" ]; then echo "$p"; return 0; fi
  done
  find "${REPO_ROOT}" -maxdepth 5 -type f -name seekdb -perm -u+x 2>/dev/null | head -1
}

# 1) 编译
BIN="$(find_bin || true)"
if [ "${SKIP_BUILD}" != "1" ] && [ -z "${BIN}" ]; then
  cd "${REPO_ROOT}"
  log "编译 seekdb（首次约 30-60 分钟）..."
  ./build.sh release --init --make -j"$(nproc)"
  BIN="$(find_bin || true)"
fi
[ -n "${BIN}" ] || { log "找不到 seekdb 二进制，先去掉 SKIP_BUILD 编译一次"; exit 1; }
log "二进制：${BIN}"

# 2) 起一个干净的实例（seekdb 以「当前工作目录」为数据目录）
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
nohup "${RUN_DIR}/seekdb" ${SEEKDB_ARGS:-} > "${RUN_DIR}/run.log" 2>&1 &
for i in $(seq 1 240); do
  if mysql -h127.0.0.1 -P"${PORT}" -uroot -e 'SELECT 1' > /dev/null 2>&1; then
    log "实例已就绪（${i}s）"
    break
  fi
  sleep 1
  if [ "$i" = "240" ]; then
    log "实例 240s 内没起来，看 ${RUN_DIR}/run.log"
    tail -40 "${RUN_DIR}/run.log" >&2 || true
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
bash "${BENCH_DIR}/fts_large_bench.sh" > "${RUN_DIR}/bench.out" 2>&1

log "报告尾部："
tail -32 "${REPORT}" >&2 || true

log "算分："
python3 "${BENCH_DIR}/fts_large_bench_score.py" "${REPORT}" | tee "${RUN_DIR}/score.txt"

log "完成。报告：${REPORT}；分数：${RUN_DIR}/score.txt"
