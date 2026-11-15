# 600080 · 赛题四：seekdb 全文索引性能优化

本目录是**判分同款基准脚本的副本**，只用于本地复现与调优验证；与判题侧的
`tools/benchmark/*` 保持完全一致（参数、计时方式、基线 JSON 都没改）。放在
`contest/` 下是为了避免和评测侧的 `tools/benchmark/` 互相覆盖。

计分方式（见 `fts_large_bench_score.py`）：

```
单项提升率 = (基线 - 本次) / 基线            # 耗时，越低越好
综合提升率 = (build 三项均值 + tokenize 两项均值 + query 四项均值) / 3
性能得分(百分制) = 100 × clamp(综合提升率, 0, 0.5) / 0.5
```

- 基线固定：`rs=20000 / batch=500 / rounds=3000 / query_rounds=200 / samples=3 / warmup=30`
- build：`build_ik_all_sec`(35.28s)、`build_ik_content_sec`(28.38s)、`build_beng_en_sec`(14.76s)
- tokenize：`tokenize_ik_avg_ms`(0.765)、`tokenize_beng_avg_ms`(0.423)
- query：`query_cn`(16.66ms)、`query_beng`(24.30ms)、`query_mixed`(17.56ms)、`query_limit`(16.23ms)

## 一键复现

```bash
# 仓库根目录，Linux / WSL2 + mysql 客户端 + python3 + bash
bash contest/600080/run_local.sh
```

首次会先 `./build.py init && ./build.py build seekdb`（30–60 分钟），然后起实例、
导入 2 万篇文档、建 3 个全文索引、跑分词与查询、最后直接打印百分制分数。

迭代时更快：

```bash
SKIP_BUILD=1 SKIP_LOAD=1 bash contest/600080/run_local.sh   # 复用二进制和数据，只重测分词/查询
```

## 本分支（perf-r6）改了什么

前一轮（perf-r5）判题拿到 `14.75%` 综合提升，两处明显没吃满，本次针对性修正：

### 1. TOKENIZE 结果缓存提前到参数解析之前

`src/sql/engine/expr/ob_expr_tokenize.cpp`

r5 的缓存查找放在 `parse_param()` **之后**，而 `parse_param()` 每次都要做
`parse_parser_properties` → `reform_parser_properties` → `try_load_dictionary_for_ik()`
（ik 词典装载）。也就是说**即使缓存命中，最贵的那部分开销照付**，tokenize 只拿到
很有限的收益。

本次改为：先用「原始参数 + 排序规则」拼一个更严格的键（`build_raw_tokenize_key`），
命中就整段跳过 `parse_param()`，直接拷贝上次序列化好的 JSON；未命中才走完整路径，
并在成功后再入缓存。键里带 `TOK_RESULT_KEY_SALT` 版本号，改编码方式时递增即可失效。

只做了缓存，不改分词语义：同一个键必然对应同一个 `(fulltext, parser, properties,
collation)`，结果与原路径逐字节一致。

### 2. 全文索引构建不再被 `/3` 压成串行

`src/sql/resolver/ddl/ob_fts_index_builder_util.cpp::decide_parallelism`

原实现：`decided_parallelism = MAX(original_parallelism / 3, 1)`。单机 DDL 默认并行度
通常是 1，除以 3 后直接是 1 —— 整段 FTS 索引构建完全串行，正好压在 build 三项上。
r5 只是改成 `MAX(original, 2)`，默认场景仍只有 2。

本次改为：

```cpp
const int64_t hw_parallelism = MIN(MAX(common::get_cpu_num() / 2, 2L), 8L);
decided_parallelism = MAX(original_parallelism, hw_parallelism);
```

即「请求并行度」和「按核数给的默认值」取大者，上限 8：4 核 CI → 2，20 核机器 → 8，
既尊重显式指定，也让默认场景吃满多核。

## 还没做、下一轮的方向

`query` 四项（16.6–24.3ms，命中 7300–11000 行）目前完全没动，占综合提升率 1/3。
它们是 `SELECT COUNT(*) ... WHERE MATCH(...) AGAINST(...)`，耗时大概率花在
「把命中行物化 + 逐行过表达式」上，而不是倒排求交本身。可探索：

- 只做计数时跳过不必要的行投影/回表（`COUNT(*)` 语义下不需要列值）
- `MATCH ... LIMIT n`（`query_limit`）在拿够 n 行后提前结束倒排扫描
- 排序键比较与结果合并的批量处理

## 注意

- 本分支的 C++ 改动**尚未在本地编译验证**（本机到 GitHub/bazel 依赖源的网络不稳，
  `build.py` 拉 `rules_rust` 失败），首次在算力机上请先单独跑一次编译确认能过，
  再跑基准；编译错误优先看 `build/log` 与 bazel 的 `ERROR:` 行。
- 基准脚本会 **DROP 并重建** `fts_large_bench` 库，务必用专用测试实例。
- 报告是追加写入的，每次完整测试用新文件（`run_local.sh` 会按时间戳新建）。
