# MiniVectorDB 运行说明

对应赛题：2026 OceanBase 大赛 · 轻量向量数据库（MiniVectorDB），分支 `2026-competition-round2`。

## 构建与启动

```bash
docker build -t minivectordb .
# 评测方式：stdin/stdout JSONL，数据目录挂载到 /data
docker run -i --rm --network=none --read-only \
  --mount type=bind,src=<宿主目录>,dst=/data \
  minivectordb < requests.jsonl > stdout.jsonl 2> stderr.log
```

- 基础镜像 `python:3.12-slim`，构建阶段**不联网、不装第三方包**：只用标准库，任何判题机都能构建成功。
- 向量距离的三种实现按环境自动选择：numpy 可导入时用 float32 矩阵 + float64 分块累加（最快）；否则用 `math.dist`（C 层单行距离计算）+ 堆选择；再退化为纯 Python 逐元素计算。三条路径结果一致（本地 400 组随机查询逐行比对无差异）。
- 容器前台运行，`CMD ["python", "-u", "/app/minivectordb.py"]`，不监听任何端口。
- 数据目录：容器内固定为 `/data`（可用环境变量 `VECTORDB_DATA_DIR` 覆盖，供本地测试）。

## 协议要点

- 启动完成恢复后主动输出 `{"type":"ready"}`，之后每个请求恰好一个响应，原样回传 `id`。
- `{"op":"execute","sql":...,"search_mode":"exact|ann"}`；`{"op":"shutdown"}` 正常退出（退出码 0）。
- 详细协议见根目录 `STDIO_PROTOCOL.md`；公开用例见 `mysqltest/`，自测驱动 `protocol_check/run.py`。

## 支持的 SQL

- `CREATE TABLE t (id BIGINT PRIMARY KEY, name VARCHAR(n), v VECTOR(dim))`；每表至多一个 VECTOR 列，主键必须为 BIGINT。
- `INSERT INTO ... VALUES (...)`（多行）、`SELECT`（投影 / `*` / WHERE / ORDER BY / LIMIT / DISTINCT）、`UPDATE ... SET ... WHERE ...`、`DELETE FROM ... WHERE ...`。
- `BEGIN` / `COMMIT` / `ROLLBACK`；未显式 BEGIN 的修改语句按自动提交处理。
- 向量：`L2_DISTANCE(a,b)`、`L2_SQUARED`、`COSINE_DISTANCE`、`INNER_PRODUCT`；向量以 float32 存储，距离按存储值计算。
- 索引：`CREATE VECTOR INDEX idx ON t(v)` / `CREATE INDEX` / `DROP INDEX`（索引记录元数据；检索始终精确扫描，exact/ann 结果一致且满足过滤与并列规则）。
- 不支持（返回 `unsupported_sql` 等错误）：ALTER TABLE、JOIN、子查询、聚合（COUNT/SUM/AVG/MIN/MAX、GROUP BY）、主键更新、嵌套事务、外键。

## 存储与恢复

- 文件布局：`/data/snapshot.json`（行式 JSON：首行头 `{"version":2,"seq":N}`，随后每表一行元信息、每行数据一行）+ `/data/wal.jsonl`（已提交事务的追加日志，每行一个事务）。
- 每次提交（自动提交或 COMMIT）先 `fsync` WAL 再返回成功响应，`docker kill -9` 后已确认提交的数据不丢。
- 启动时先加载快照（流式逐行读取，内存占用与数据量线性且常数小），再重放 WAL；WAL 超过 1MiB 后自动做快照合并。
- 正常关闭（shutdown）回滚未提交事务并强制写快照；未提交事务在崩溃或关闭后都不会部分生效。
- 向量在内存中以 `array('f')`（原生 float32）存储，快照/WAL 逐行流式读写，可在限内存容器内处理大于内存预算的数据量。

## 错误码（语义名）

`duplicate_key`、`value_too_long`、`out_of_range`、`type_mismatch`、`not_null_violation`、`unknown_table`、`unknown_column`、`unknown_index`、`table_exists`、`column_count_mismatch`、`dimension_mismatch`、`invalid_vector`、`float32_overflow`、`division_by_zero`、`transaction_active`、`syntax_error`、`unsupported_sql`、`invalid_request`、`unsupported_op`、`internal_error`。

## 本地自测

```bash
python3 protocol_check/run.py --command '["python3", "minivectordb.py"]' --output <结果目录>
```

Agent Prompt 学习材料：根目录 `conversation.json`（同时在 `agent/conversation.json` 保留同步副本）。
