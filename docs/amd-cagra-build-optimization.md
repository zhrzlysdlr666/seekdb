# AMD 赛道：seekdb GPU CAGRA 建索引路径优化记录（题号 800509 / 600072）

本文件记录本队在 V5 冻结基线（`634888d9b037153fa143d2c913ba4b25503e1d62`）之上做的改动、
改动依据，以及如何在评测机上复现测量。**本文件不写任何自测跑分数字**——正式成绩只由组织方
自动化评测产生，自报数字不作为计分依据。

## 1. 基线在做什么

`CREATE VECTOR INDEX ... WITH (lib=cuvs)` 的离线 DDL 路径（V5）实际链路是：

1. DDL 扫描原表，把 250K 行向量批量交给 `obvectorutil::build_index`；
2. `build_index` 发现该 handle 被标记为 `lib=cuvs`，**不再构建 CPU VSAG HNSW**，直接走
   `ob_cuvs_register` → `seekdb_cuvs_build`（`tools/hipvs_bridge/seekdb_cuvs_bridge.c`）；
3. bridge 里 `cuvsCagraBuild` 在 GPU 上建 CAGRA 图；
4. 索引落盘时 `ob_cuvs_serialize` 把 CAGRA 图连同 ID、原始向量写进索引流；
5. 计时终点：第一条固定查询返回合法结果。

## 2. 时间花在哪：一个由题面参考值反推的模型

题面给出的 V5 参考值（同一张 W7900D，仅供参考）：

| 数据集 | 维度 | 构建（含首查） |
| --- | --- | --- |
| SIFT250K | 128 | 20–27 s（中位 23 s） |
| Hidden250K | 256 | 29–35 s（中位 30 s） |

两者点数相同、维度差 2 倍，但时间只差 1.3 倍。按 `T = a·dim + b` 拟合得
`a ≈ 0.055 s/dim`、`b ≈ 16 s`。也就是说**约 16 秒与维度无关**，不可能来自逐对距离计算
（那部分正比于 dim），只能来自 CAGRA 把 `intermediate_graph_degree` 的候选图剪成
`graph_degree` 最终图的那一步：该步在候选表的**已存距离**上做支配性（detour）判断，
复杂度正比于 `n · igd²`，与维度无关。

在 250K 点、`igd = 256`、`niter = 100` 这组参数下，16 s 的固定项与 7 s / 14 s 的
维度相关项都与观测吻合。

## 3. 改动

### 3.1 建图参数（`tools/hipvs_bridge/seekdb_cuvs_bridge.c`）

| 参数 | 基线 | 本分支 | 理由 |
| --- | --- | --- | --- |
| `graph_degree` | 128 | **128（不动）** | 查询路径实际游走的图宽，直接决定 Recall。硬门槛要求 Recall 相比基线下降不超过 0.005，这一项刻意保持原值。 |
| `intermediate_graph_degree` | 256 | 192 | 只影响候选池大小。剪枝代价 ∝ igd²，从 256 降到 192 即去掉约 44% 的固定项；192 仍是 `graph_degree` 的 1.5 倍，保留了剪枝所需的余量。 |
| `nn_descent_niter` | 100 | 50 | NN-descent 在 250K 点规模上远早于 100 轮就收敛；50 也是上游默认值。该项线性影响维度相关的那部分时间。 |

三者都写成 `#ifndef` 宏，可以用 `-DSEEKDB_CUVS_INTERMEDIATE_DEGREE=...` 覆盖，便于 A/B。

### 3.2 快照中转不再落盘（`src/oblib/lib/vector/ob_vsag_adaptor.cpp`）

cuVS 的 C API 只能序列化到**路径**，所以基线把整张图写进 `tmpfile()`，再读回来追加到索引流。
这个中转文件是纯暂存，却要多付一次「写几百 MB + 读几百 MB」的磁盘 I/O，且发生在计时区间内。

本分支把中转文件换成匿名内存文件（`syscall(SYS_memfd_create)`，仍是 `/proc/self/fd/N` 路径，
cuVS 侧完全无感），并把写回索引流的分块从 64 KB 放大到 8 MB。内核不提供 memfd 时自动回落到
原来的 `tmpfile()`。

## 4. 如何复现

编译（与评测机相同的固定命令，仓根目录执行）见题面 3.2 节；本分支没有新增任何仓库外依赖，
`tools/hipvs_bridge/build.sh` 也未改动，因此原命令即可产出 `build_release/src/observer/seekdb`
与 bridge 动态库。

测量口径与评测一致：先 `LOAD DATA` 导入原始向量表且不建索引，再计时执行
`CREATE VECTOR INDEX`（`type=hnsw, lib=cuvs, distance=l2`）到「第一条固定查询返回合法结果」为止；
两个数据集各跑一轮并与同轮基线配对。查询侧参数与评测保持默认，不做调优，避免触碰
`query_regression` 与 Recall 门槛。

## 5. 已知风险

- Recall 门槛是「相比同轮基线不得下降超过 0.005」，Hidden250K 基线 recall@100 ≈ 0.914，
  余量只有 0.909。若判题回传 recall gate 失败，第一个要回退的是 `intermediate_graph_degree`
  （先回 224，再回 256），`nn_descent_niter = 50` 是上游默认值、可最后再动。
- 中转文件改到内存后，峰值内存多出约一张图的体积（250K × 128 × 4 B ≈ 128 MB），远低于
  96 GB 的主机内存配额，且不计入显存。
