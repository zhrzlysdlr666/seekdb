/**
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
 */
#pragma once

#include "sql/engine/basic/ob_file_scan_utils.h"

namespace oceanbase
{
namespace sql
{
class GlobFileScanner : public ObFileScanner
{
public:
  GlobFileScanner() : position_(0) {}
  ~GlobFileScanner() override { close(); }
  int open(const common::ObString &secure_root, const common::ObString &pattern,
           const ObFileFingerprint *expected = nullptr) override;
  int next_row(common::ObIArray<common::ObObj> &row, bool &has_row) override;
  int rescan() override { position_ = 0; return common::OB_SUCCESS; }
  void close() override;
  const common::ObIArray<ObFileColumn> &get_columns() const override { return columns_; }
  const ObFileErrorLocation &get_error_location() const override { return error_; }
  int64_t get_row_count() const override { return paths_.size(); }

private:
  std::string secure_root_;
  std::vector<std::string> paths_;
  int64_t position_;
  common::ObSEArray<ObFileColumn, 1> columns_;
  ObFileErrorLocation error_;
};
}
}
