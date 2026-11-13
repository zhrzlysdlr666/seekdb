# 2026 OceanBase 数据库大赛 · AMD 赛题参赛说明

- 队伍：所有人保持积极向上
- 参赛者：张浩然（个人赛）
- 赛题：[800509] 使用 hipVS 加速 HNSW 索引构建
- 基线：`zihaomu/seekdb` 分支 `amd-oceanbase-official-contest-base-v5`
  冻结 commit `634888d9b037153fa143d2c913ba4b25503e1d62`

本队验证行（独立一行）：

OB-P:pB_DeUDSRSzy5hVUvR3tdw

## 提交说明

本提交是 V5 冻结基线的直系后代，用于按题面第 4 节的要求打通
「拉取仓库 → 后代校验 → 统一编译 → GPU 路径抽检 → 配对测试」的完整评测链路。

性能优化在后续提交中迭代，每一步都要求：以题面 3.2 节的固定命令
（`CMAKE_PREFIX_PATH=/opt/hipvs;/opt/rocm`，固定 CMake 参数）能在离线环境编译通过，
并通过 `lib=vsag` 的回归与 Recall 硬门槛。
