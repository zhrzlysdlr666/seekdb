/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_function_table_op.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "sql/session/ob_basic_session_info.h"
#include "sql/engine/basic/ob_glob_file_scanner.h"
#include "sql/engine/basic/ob_jsonl_file_scanner.h"
#include "sql/engine/basic/ob_parquet_file_scanner.h"


namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER((ObFunctionTableSpec, ObOpSpec), value_expr_, column_exprs_, has_correlated_expr_,
                    is_file_scan_, file_scan_path_, file_scan_root_, file_scan_device_, file_scan_inode_,
                    file_scan_size_, file_scan_mtime_sec_, file_scan_mtime_nsec_,
                    file_scan_ctime_sec_, file_scan_ctime_nsec_, file_scan_delimiter_, file_scan_quote_,
                    file_scan_escape_, file_scan_header_, file_scan_nullstr_, file_scan_is_glob_,
                    file_scan_format_);

int ObFunctionTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  already_calc_ = false;
  if (MY_SPEC.is_file_scan_) {
    ObSQLSessionInfo *session = ctx_.get_my_session();
    ObString secure_root;
    bool enabled = false;
    void *buf = nullptr;
    const int64_t scanner_size = MY_SPEC.file_scan_is_glob_ ? sizeof(GlobFileScanner)
        : MY_SPEC.file_scan_format_ == FILE_SQL_JSONL ? sizeof(JsonlFileScanner)
        : MY_SPEC.file_scan_format_ == FILE_SQL_PARQUET ? sizeof(ParquetFileScanner)
        : sizeof(ObCsvFileScanner);
    ObCsvScanOptions options;
    options.field_terminator_ = MY_SPEC.file_scan_delimiter_;
    options.field_enclosure_ = MY_SPEC.file_scan_quote_;
    options.field_escape_ = MY_SPEC.file_scan_escape_;
    options.has_header_ = MY_SPEC.file_scan_header_;
    if (!MY_SPEC.file_scan_nullstr_.empty()) {
      options.null_string_.assign(MY_SPEC.file_scan_nullstr_.ptr(), MY_SPEC.file_scan_nullstr_.length());
    }
    ObFileFingerprint expected_fingerprint;
    expected_fingerprint.device_ = MY_SPEC.file_scan_device_;
    expected_fingerprint.inode_ = MY_SPEC.file_scan_inode_;
    expected_fingerprint.size_ = MY_SPEC.file_scan_size_;
    expected_fingerprint.mtime_sec_ = MY_SPEC.file_scan_mtime_sec_;
    expected_fingerprint.mtime_nsec_ = MY_SPEC.file_scan_mtime_nsec_;
    expected_fingerprint.ctime_sec_ = MY_SPEC.file_scan_ctime_sec_;
    expected_fingerprint.ctime_nsec_ = MY_SPEC.file_scan_ctime_nsec_;
    if (OB_ISNULL(session)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(session->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) {
    } else if (!enabled) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "file SQL while enable_file_sql is OFF");
    } else if (OB_FAIL(session->get_secure_file_priv(secure_root))) {
    } else if (secure_root != MY_SPEC.file_scan_root_) {
      ret = OB_ERR_NO_PRIVILEGE;
      LOG_USER_ERROR(OB_ERR_NO_PRIVILEGE, "FILE");
    } else if (!MY_SPEC.file_scan_is_glob_ && MY_SPEC.file_scan_format_ != FILE_SQL_CSV
               && MY_SPEC.file_scan_format_ != FILE_SQL_JSONL
               && MY_SPEC.file_scan_format_ != FILE_SQL_PARQUET) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "unknown file scan format");
    } else if (OB_ISNULL(buf = get_exec_ctx().get_allocator().alloc(scanner_size))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      if (MY_SPEC.file_scan_is_glob_) {
        file_scanner_ = new (buf) GlobFileScanner();
        ret = file_scanner_->open(MY_SPEC.file_scan_root_, MY_SPEC.file_scan_path_);
      } else if (MY_SPEC.file_scan_format_ == FILE_SQL_JSONL) {
        file_scanner_ = new (buf) JsonlFileScanner();
        ret = file_scanner_->open(MY_SPEC.file_scan_root_, MY_SPEC.file_scan_path_, &expected_fingerprint);
      } else if (MY_SPEC.file_scan_format_ == FILE_SQL_PARQUET) {
        file_scanner_ = new (buf) ParquetFileScanner();
        ret = file_scanner_->open(MY_SPEC.file_scan_root_, MY_SPEC.file_scan_path_, &expected_fingerprint);
      } else {
        ObCsvFileScanner *csv = new (buf) ObCsvFileScanner();
        file_scanner_ = csv;
        ret = csv->open(MY_SPEC.file_scan_root_, MY_SPEC.file_scan_path_, options, &expected_fingerprint);
      }
    }
    if (OB_FAIL(ret) && file_scanner_ != nullptr) {
      LOG_ERROR("file scan execution open failed", K(ret), K(MY_SPEC.file_scan_path_),
                K(MY_SPEC.file_scan_root_));
      file_scanner_->~ObFileScanner();
      get_exec_ctx().get_allocator().free(file_scanner_);
      file_scanner_ = nullptr;
    } else if (OB_SUCC(ret)) {
      next_row_func_ = &ObFunctionTableOp::inner_get_next_row_file_scan;
    }
  } else if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType == MY_SPEC.value_expr_->datum_meta_.type_) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_udf;
  } else {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_sys_func;
  }
  return ret;
}

int ObFunctionTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else {
    node_idx_ = 0;
    if (MY_SPEC.is_file_scan_) {
      if (OB_ISNULL(file_scanner_)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(file_scanner_->rescan())) {
      }
    } else if (MY_SPEC.has_correlated_expr_) {
      row_count_ = 0;
      col_count_ = 0;
      value_table_ = NULL;
      already_calc_ = false;
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_close()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  row_count_ = 0;
  col_count_ = 0;
  value_table_ = NULL;
  if (OB_NOT_NULL(file_scanner_)) {
    file_scanner_->close();
    file_scanner_->~ObFileScanner();
    get_exec_ctx().get_allocator().free(file_scanner_);
    file_scanner_ = nullptr;
  }
  return ret;
}

void ObFunctionTableOp::destroy()
{
  if (file_scanner_ != nullptr) {
    file_scanner_->~ObFileScanner();
    get_exec_ctx().get_allocator().free(file_scanner_);
    file_scanner_ = nullptr;
  }
  ObOperator::destroy();
}


int ObFunctionTableOp::get_current_result(ObObj &result)
{
  int ret = OB_SUCCESS;
  void *data = NULL;
  CK (already_calc_);
  if (node_idx_ < 0 || node_idx_ >= row_count_ * col_count_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fail to get current result in table function",
              K(node_idx_), K(row_count_), K(col_count_));
  }
  do {
    CK (node_idx_ >= 0);
    if (OB_SUCC(ret) && node_idx_ >= row_count_) {
      ret = OB_ITER_END;
    }
    CK (OB_NOT_NULL(value_table_));
    OX (data = value_table_->get_data());
    CK (OB_NOT_NULL(data));
    OX (result = (static_cast<ObObj*>(data))[node_idx_++]);
  } while (OB_SUCC(ret) && result.get_meta().get_type() == ObMaxType);
  return ret;
}

int ObFunctionTableOp::inner_get_next_row()
{
  return (this->*next_row_func_)();
}

int ObFunctionTableOp::inner_get_next_row_file_scan()
{
  int ret = OB_SUCCESS;
  bool has_row = false;
  clear_evaluated_flag();
  if (OB_ISNULL(file_scanner_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ctx_.check_status())) {
  } else if (OB_FAIL(file_scanner_->next_row(file_row_, has_row))) {
    LOG_ERROR("file scan execution row read failed", K(ret), K(file_row_.count()));
  } else if (!has_row) {
    ret = OB_ITER_END;
  } else if (file_row_.count() != MY_SPEC.column_exprs_.count()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("file scan output column count mismatch", K(file_row_.count()),
              K(MY_SPEC.column_exprs_.count()));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < file_row_.count(); ++i) {
      ObObj &obj = file_row_.at(i);
      ObExpr *expr = MY_SPEC.column_exprs_.at(i);
      if (OB_ISNULL(expr)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (obj.is_null()) {
        expr->locate_datum_for_write(eval_ctx_).set_null();
        expr->set_evaluated_projected(eval_ctx_);
      } else if (OB_FAIL(expr->locate_datum_for_write(eval_ctx_).from_obj(obj, expr->obj_datum_map_))) {
      } else {
        if (obj.is_string_type() && obj.get_string_len() > 0) {
          char *buffer = expr->get_str_res_mem(eval_ctx_, obj.get_string_len());
          if (OB_ISNULL(buffer)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            MEMCPY(buffer, obj.get_string_ptr(), obj.get_string_len());
            expr->locate_datum_for_write(eval_ctx_).set_string(buffer, obj.get_string_len());
          }
        }
        expr->set_evaluated_projected(eval_ctx_);
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_udf()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType != MY_SPEC.value_expr_->datum_meta_.type_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (FALSE_IT(plan_ctx->set_autoinc_id_tmp(0))) {
  } else if (OB_FAIL(ctx_.check_status())) {
  } else {
    ObDatum *value = nullptr;
    if (!already_calc_) {
      if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
      } else if (value->is_null()) {
        //do nothing
      } else if (OB_ISNULL(value_table_ 
                 = reinterpret_cast<pl::ObPLCollection*>(value->get_ext()))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        row_count_ = value_table_->is_inited() ? value_table_->get_count() : 0;
        col_count_ = value_table_->get_column_count();
      }
      already_calc_ = true;
    }
    if (OB_SUCC(ret) && OB_UNLIKELY(node_idx_ >= row_count_)) {
      ret = OB_ITER_END;
    }
    CK (MY_SPEC.column_exprs_.count() >= col_count_);
    ObObj obj_stack[col_count_];
    if (OB_SUCC(ret)) {
      if (nullptr != value_table_ 
          && ObExtendType == value_table_->get_element_type().get_obj_type()) {
        pl::ObPLComposite *composite = NULL;
        pl::ObPLRecord *record = NULL;
        ObObj record_obj;
        OZ (get_current_result(record_obj));
        if (OB_FAIL(ret)) {
        } else if (ObUserDefinedSQLType == record_obj.get_type()) {
          obj_stack[0] = record_obj;   
        } else if (record_obj.is_pl_extend()) {
          CK (OB_NOT_NULL(composite = reinterpret_cast<pl::ObPLComposite*>(record_obj.get_ext())));
          if (OB_SUCC(ret)) {
            if (composite->is_record()) {
              OX (record = static_cast<pl::ObPLRecord*>(composite));
              CK (record->get_count() == col_count_);
              for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
                OZ (record->get_element(i, obj_stack[i]));
              }
            } else if (composite->is_collection()) {
              OX (obj_stack[0] = record_obj);
            } else {
              ret = OB_ERR_UNEXPECTED;
            }
          }
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_ERROR("unexpected here", K(ret), K(record_obj), K(record_obj.meta_));
        }
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
          if (OB_FAIL(get_current_result(obj_stack[i]))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
        if (obj_stack[i].is_null()) {
          MY_SPEC.column_exprs_.at(i)->locate_datum_for_write(eval_ctx_).set_null();
        } else {
          const ObObjDatumMapType &datum_map = MY_SPEC.column_exprs_.at(i)->obj_datum_map_;
          ObExpr * const &expr = MY_SPEC.column_exprs_.at(i);
          ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
          if (OB_FAIL(datum.from_obj(obj_stack[i], datum_map))) {
          } else if (is_lob_storage(obj_stack[i].get_type()) &&
                     OB_FAIL(ob_adjust_lob_datum(get_exec_ctx(), obj_stack[i],
                                                 expr->obj_meta_, datum_map,
                                                 get_exec_ctx().get_allocator(), datum))) {
          }
        }
        if (OB_SUCC(ret)) {
          MY_SPEC.column_exprs_.at(i)->set_evaluated_projected(eval_ctx_);
        }
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_sys_func()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  ObDatum *value = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ctx_.check_status())) {
  } else if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
    if (OB_ITER_END != ret) {
    }
  } else {
    MY_SPEC.column_exprs_.at(0)->locate_datum_for_write(eval_ctx_).set_datum(*value);
    MY_SPEC.column_exprs_.at(0)->set_evaluated_projected(eval_ctx_);
  }
  return ret;
}


} // end namespace sql
} // end namespace oceanbase
