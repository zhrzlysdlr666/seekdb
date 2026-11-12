/**
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/basic/ob_file_scan_op.h"

#include <algorithm>
#include <cstring>
#include "sql/engine/ob_exec_context.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER((FileScanSpec, ObFunctionTableSpec), column_indexes_);

int FileScanOp::inner_open()
{
  int ret = OB_SUCCESS;
  scan_end_ = false;
  if (OB_FAIL(ObFunctionTableOp::inner_open())) {
  } else if (OB_ISNULL(file_scanner_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (MY_SPEC.column_indexes_.count() != MY_SPEC.column_exprs_.count()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("file scan projection and expression counts differ", K(ret),
             K(MY_SPEC.column_indexes_.count()), K(MY_SPEC.column_exprs_.count()));
  } else if (OB_FAIL(file_scanner_->set_projection(MY_SPEC.column_indexes_))) {
    LOG_WARN("failed to set file scan projection", K(ret), K(MY_SPEC.column_indexes_));
  }
  if (OB_FAIL(ret)) {
    // Base open owns scanner allocation; use its normal close path when
    // projection setup fails after the scanner has already been opened.
    (void)ObFunctionTableOp::inner_close();
  }
  return ret;
}

int FileScanOp::inner_rescan()
{
  scan_end_ = false;
  file_row_.reuse();
  return ObFunctionTableOp::inner_rescan();
}

int FileScanOp::read_next_row(bool &has_row)
{
  int ret = OB_SUCCESS;
  has_row = false;
  if (OB_FAIL(ctx_.check_status())) {
  } else if (OB_ISNULL(file_scanner_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (scan_end_) {
  } else if (OB_FAIL(file_scanner_->next_row(file_row_, has_row))) {
    LOG_WARN("failed to read projected file scan row", K(ret),
             K(file_scanner_->get_error_location().line_),
             K(file_scanner_->get_error_location().column_),
             K(file_scanner_->get_error_location().byte_offset_));
  } else if (!has_row) {
    scan_end_ = true;
  } else if (file_row_.count() != MY_SPEC.column_exprs_.count()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("file scan projected row and expression counts differ", K(ret),
             K(file_row_.count()), K(MY_SPEC.column_exprs_.count()));
  }
  return ret;
}

int FileScanOp::materialize_row()
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < file_row_.count(); ++i) {
    const ObObj &obj = file_row_.at(i);
    ObExpr *expr = MY_SPEC.column_exprs_.at(i);
    if (OB_ISNULL(expr)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
      if (obj.is_null()) {
        datum.set_null();
      } else if (OB_FAIL(datum.from_obj(obj, expr->obj_datum_map_))) {
      } else if (obj.is_string_type() && obj.get_string_len() > 0) {
        // Scanner objects refer to its current record. Each datum needs
        // expression-owned memory before the next record is read.
        char *buffer = expr->get_str_res_mem(
            eval_ctx_, obj.get_string_len(), expr->get_datum_idx(eval_ctx_));
        if (OB_ISNULL(buffer)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          std::memcpy(buffer, obj.get_string_ptr(), obj.get_string_len());
          datum.set_string(buffer, obj.get_string_len());
        }
      }
      if (OB_SUCC(ret)) {
        if (expr->is_batch_result()) {
          expr->get_evaluated_flags(eval_ctx_).set(eval_ctx_.get_batch_idx());
        }
        expr->set_evaluated_projected(eval_ctx_);
      }
    }
  }
  return ret;
}

int FileScanOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  bool has_row = false;
  clear_evaluated_flag();
  if (OB_FAIL(read_next_row(has_row))) {
  } else if (!has_row) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(materialize_row())) {
  }
  return ret;
}

int FileScanOp::inner_get_next_batch(const int64_t max_row_cnt)
{
  int ret = OB_SUCCESS;
  const int64_t batch_size = std::min(max_row_cnt, MY_SPEC.max_batch_size_);
  clear_evaluated_flag();
  brs_.size_ = 0;
  brs_.end_ = scan_end_;
  brs_.all_rows_active_ = true;
  if (batch_size <= 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(brs_.skip_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    brs_.skip_->reset(batch_size);
    for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.column_exprs_.count(); ++i) {
      ObExpr *expr = MY_SPEC.column_exprs_.at(i);
      if (OB_ISNULL(expr)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (expr->is_batch_result()) {
        expr->get_evaluated_flags(eval_ctx_).reset(batch_size);
      }
    }
    ObEvalCtx::BatchInfoScopeGuard batch_guard(eval_ctx_);
    batch_guard.set_batch_size(batch_size);
    for (int64_t idx = 0; OB_SUCC(ret) && !scan_end_ && idx < batch_size; ++idx) {
      batch_guard.set_batch_idx(idx);
      bool has_row = false;
      if (OB_FAIL(read_next_row(has_row))) {
      } else if (!has_row) {
        brs_.end_ = true;
      } else if (OB_FAIL(materialize_row())) {
      } else {
        // Zero projected columns still represent a row for COUNT(*).
        ++brs_.size_;
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
