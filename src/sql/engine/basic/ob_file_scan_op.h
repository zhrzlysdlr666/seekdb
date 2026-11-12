/**
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#pragma once

#include "sql/engine/basic/ob_function_table_op.h"

namespace oceanbase
{
namespace sql
{

class FileScanSpec : public ObFunctionTableSpec
{
  OB_UNIS_VERSION_V(1);
public:
  FileScanSpec(common::ObIAllocator &allocator, const ObPhyOperatorType type)
    : ObFunctionTableSpec(allocator, type), column_indexes_(allocator)
  {}

  common::ObFixedArray<int64_t, common::ObIAllocator> column_indexes_;
};

class FileScanOp : public ObFunctionTableOp
{
public:
  FileScanOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input)
    : ObFunctionTableOp(exec_ctx, spec, input), scan_end_(false)
  {}

  int inner_open() override;
  int inner_rescan() override;
  int inner_get_next_row() override;
  int inner_get_next_batch(const int64_t max_row_cnt) override;

private:
  int read_next_row(bool &has_row);
  int materialize_row();

private:
  bool scan_end_;
};

} // namespace sql
} // namespace oceanbase
