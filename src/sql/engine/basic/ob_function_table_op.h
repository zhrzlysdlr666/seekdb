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

#ifndef OCEANBASE_BASIC_OB_FUNCTION_TABLE_OP_H_
#define OCEANBASE_BASIC_OB_FUNCTION_TABLE_OP_H_

#include "sql/engine/ob_operator.h"
#include "sql/engine/basic/ob_chunk_datum_store.h"
#include "sql/engine/basic/ob_file_scan_utils.h"
#include "lib/charset/ob_charset.h"

namespace oceanbase
{
namespace sql
{

class ObExpr;
class ObFunctionTableSpec : public ObOpSpec
{
OB_UNIS_VERSION_V(4);
public:
  ObFunctionTableSpec(common::ObIAllocator &alloc, const ObPhyOperatorType type)
    : ObOpSpec(alloc, type), value_expr_(nullptr), column_exprs_(alloc), has_correlated_expr_(false),
      is_file_scan_(false), file_scan_is_glob_(false), file_scan_format_(FILE_SQL_CSV),
      file_scan_path_(), file_scan_root_(),
      file_scan_delimiter_(','), file_scan_quote_('"'), file_scan_escape_('\\'),
      file_scan_header_(true), file_scan_nullstr_(), file_scan_device_(0),
      file_scan_inode_(0), file_scan_size_(0), file_scan_mtime_sec_(0), file_scan_mtime_nsec_(0),
      file_scan_ctime_sec_(0), file_scan_ctime_nsec_(0)
  {}
  ObExpr *value_expr_;
  common::ObFixedArray<ObExpr*, common::ObIAllocator> column_exprs_;
  bool has_correlated_expr_;
  bool is_file_scan_;
  bool file_scan_is_glob_;
  int64_t file_scan_format_;
  common::ObString file_scan_path_;
  common::ObString file_scan_root_;
  char file_scan_delimiter_;
  char file_scan_quote_;
  char file_scan_escape_;
  bool file_scan_header_;
  common::ObString file_scan_nullstr_;
  uint64_t file_scan_device_;
  uint64_t file_scan_inode_;
  int64_t file_scan_size_;
  int64_t file_scan_mtime_sec_;
  int64_t file_scan_mtime_nsec_;
  int64_t file_scan_ctime_sec_;
  int64_t file_scan_ctime_nsec_;
};

class ObFunctionTableOp : public ObOperator
{
public:
  ObFunctionTableOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input)
    : ObOperator(exec_ctx, spec, input),
    node_idx_(0),
    already_calc_(false),
    row_count_(0),
    col_count_(0),
    value_table_(NULL),
    file_scanner_(nullptr),
    file_row_()
  {}

  virtual int inner_open() override;
  virtual int inner_rescan() override;
  virtual int inner_get_next_row() override;
  //virtual int inner_get_next_batch(int64_t max_row_cnt) override;
  virtual int inner_close() override;
  virtual void destroy() override;
private:
  int inner_get_next_row_udf();
  int inner_get_next_row_sys_func();
  int inner_get_next_row_file_scan();
  int get_current_result(common::ObObj &result);
  int64_t node_idx_;
  bool already_calc_;
  int64_t row_count_;
  int64_t col_count_;
  common::ObObj value_;
  pl::ObPLCollection *value_table_;
protected:
  ObFileScanner *file_scanner_;
  common::ObSEArray<common::ObObj, 16> file_row_;
  int (ObFunctionTableOp::*next_row_func_)();
};

} // end namespace sql
} // end namespace oceanbase

#endif /* OCEANBASE_BASIC_OB_FUNCTION_TABLE_OP_H_ */
