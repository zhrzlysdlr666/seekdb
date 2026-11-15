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

- 基础镜像 `python:3.12-slim`，**构建阶段不联网**：numpy 以固定版本 wheel 的形式放在 `vendor/`，用 `pip install --no-index --find-links=/app/vendor` 离线安装；装不上（或 import 失败）就自动回退标准库路径，构建与启动都不会因此失败。
- 向量距离的三条实现按环境自动选择：numpy 可导入时用「模长预计算 + 一次 float32 矩阵向量乘」选出前 K 候选，再用逐元素相减的 float64 算法对候选精确重算；否则用 `math.dist`（C 层单行距离计算）+ 堆选择；再退化为纯 Python 逐元素计算。三条路径结果一致（本地 400 组随机查询逐行比对无差异，另有 642 条 SQL 的针对并列/量级差距的逐字节比对）。模长公式在「距离远小于向量量级」时会丢精度，此时自动退回精确扫描（`rank_l2` 里的量级自检）。
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
- 落盘时向量写成 `{"~f32": "<base64>"}`（float32 原始字节的 base64），读回时兼容旧的十进制数组形式。 10k×128 的落盘体积从 25.6 MiB 降到 7.2 MiB，写入与重启都随之变快（见下）。

## 本地测量（10k 行 × 128 维，1000 次 TopK，本机 Windows/CPython 3.13）

| 项目 | 改动前 | 改动后 |
| --- | --- | --- |
| 批量写入（每批 100 行） | 2.52 s（0.252 ms/行） | 1.01 s（0.101 ms/行） |
| TopK 查询（1000 次，K=10，一半带过滤） | 0.685 ms/次 | 0.516~0.574 ms/次 |
| 重启恢复 | 0.87 s | 0.49 s |
| `/data` 体积 | 25.6 MiB | 7.2 MiB |

三处改动：字符串字面量改为按 `find` 跳到下一个引号/反斜杠后整段切片（原来逐字符 append，批量 INSERT 的向量字面量会把分词器压死）；向量字面量先用 `json.loads` 一次性解析，失败再回落到逐分量解析以保留精确错误信息；向量落盘改 base64 原始字节。公开用例 20/20、差分 400 项 0 差异、SIGKILL 持久性与 20k×256 内存上限均复测通过。

## 错误码（语义名）

`duplicate_key`、`value_too_long`、`out_of_range`、`type_mismatch`、`not_null_violation`、`unknown_table`、`unknown_column`、`unknown_index`、`table_exists`、`column_count_mismatch`、`dimension_mismatch`、`invalid_vector`、`float32_overflow`、`division_by_zero`、`transaction_active`、`syntax_error`、`unsupported_sql`、`invalid_request`、`unsupported_op`、`internal_error`。

## 本地自测

```bash
python3 protocol_check/run.py --command '["python3", "minivectordb.py"]' --output <结果目录>
```

Agent Prompt 学习材料：根目录 `conversation.json`（同时在 `agent/conversation.json` 保留同步副本）。
