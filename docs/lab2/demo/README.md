# Lab2 FILE_AS_SQL 实现与演示

实现位于 Ubuntu WSL2 的 `/home/wangjiayi/seekdb`，分支 `my-lab`。这项实验在 SeekDB 源码和本地数据库中完成；网页只用于查阅题目。

## 能力范围

文件表通过原生 `FILE_SCAN` 进入 SeekDB 的过滤、聚合、排序、JOIN 和 EXPLAIN 链路。CSV、JSONL 与受支持的 Parquet 平面列都有独立 scanner、Schema 推断、投影、批次读取和 rescan。文件表只读，并在查询执行时验证文件指纹。

CSV 支持命名选项、严格 CSV 解析、NULL/空字符串区别、表头规范化、UTF-8 校验和确定性类型推断。JSONL 支持扁平 JSON 对象、字段并集、缺失值、Unicode 转义和标量类型。Parquet 支持平面标量列、多 row group、PLAIN/dictionary、Snappy、v1/v2 页面、日期时间和列裁剪；嵌套、INT96、ZSTD 等未支持输入会明确报错。

`DESCRIBE SELECT` 可查看文件表推断出的列信息；`glob` 仅列出安全目录内普通文件。`COPY table/query TO CSV` 使用原生查询结果导出，区分 NULL 与空字符串、拒绝覆盖已有目标，并通过临时文件原子发布。

## CSV 演示

输入 `sales.csv`：

```csv
id,name,amount
1,Alice,10.5
2,Bob,
3,Chen,7.5
```

推断出的列为 `id BIGINT NOT NULL`、`name VARCHAR NOT NULL` 和 `amount DOUBLE NULL`。聚合得到 3 行、`SUM(amount)=18`。EXPLAIN 显示原生 `FILE_SCAN`，COPY 后重新读取保留了 Bob 的 NULL。

- [sql-interface.sql](sql-interface.sql)：演示 SQL 模板。
- [actual-sql.sql](actual-sql.sql)：实际运行 SQL。
- [actual-output.txt](actual-output.txt)：数据库输出。

## 最终验证

- Release 构建通过。
- 基础 SQL 合约 **88/88**；格式、边界和跨格式扩展合约 **81/81**。
- 原版官方 `file_sql_basic.test` 通过，返回码 0、输出 `ok`；官方测试、期望和输入数据未修改。
- 原生 C++ 测试 **38/38**。

报告和范围说明见 [Lab2 当前进度](../2026-11-09-status.md) 与 [evidence](../evidence/2026-11-10-final.json)。这不是全仓库测试报告，也不代表课程分数。扩展测试夹具只在生成时依赖 PyArrow，不是数据库运行依赖。

## 复现

```bash
cd /home/wangjiayi/seekdb
./build.sh release --make -j4
python3 unittest/sql/run_file_sql_contract.py \
  --work-dir /home/wangjiayi/.local/share/seekdb-lab2-audit-20261109 \
  --report docs/lab2/evidence/local-check.json \
  --race-library /home/wangjiayi/.local/share/seekdb-lab2-audit-20261109/copy-link-race.so \
  --extended-fixtures /home/wangjiayi/.local/share/seekdb-lab2-audit-20261109/extended-fixtures/retained-capacity-regression \
  --official --demo
python3 unittest/sql/run_file_sql_native_tests.py \
  --build-dir /tmp/file-sql-native-check
```

运行器启动并停止自己创建的临时实例；如果端口 10006 已被占用会退出，不会连接到已有实例。
