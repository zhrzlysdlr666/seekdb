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

// Native, flat-schema Parquet reader. Only the footer is inspected during open;
// page/dictionary buffers are allocated lazily for projected columns.
class ParquetFileScanner : public ObFileScanner
{
public:
  ParquetFileScanner();
  ~ParquetFileScanner() override;
  int open(const common::ObString &secure_file_priv, const common::ObString &path,
           const ObFileFingerprint *expected_fingerprint = nullptr) override;
  int next_row(common::ObIArray<common::ObObj> &row, bool &has_row) override;
  int rescan() override;
  void close() override;
  const common::ObIArray<ObFileColumn> &get_columns() const override { return columns_; }
  const ObFileErrorLocation &get_error_location() const override { return error_location_; }
  int64_t get_row_count() const override { return row_count_; }
  const ObFileFingerprint &get_fingerprint() const { return input_.get_fingerprint(); }

private:
  struct Impl;
  ParquetFileScanner(const ParquetFileScanner &) = delete;
  ParquetFileScanner &operator=(const ParquetFileScanner &) = delete;
  int read_at(int64_t offset, int64_t size, void *buffer);
  int report_error(int ret, const char *detail);
  ObFileInput input_;
  common::ObSEArray<ObFileColumn, 16> columns_;
  ObFileErrorLocation error_location_;
  int64_t row_count_;
  Impl *impl_;
};

} // namespace sql
} // namespace oceanbase