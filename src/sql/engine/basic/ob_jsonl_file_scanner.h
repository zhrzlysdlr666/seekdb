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

#include <string>
#include <unordered_map>
#include <vector>
#include "sql/engine/basic/ob_file_scan_utils.h"

namespace oceanbase
{
namespace sql
{

// A two-pass, bounded JSON Lines scanner. Returned string values remain valid
// until the next next_row(), rescan(), or close().
class JsonlFileScanner : public ObFileScanner
{
public:
  JsonlFileScanner();
  virtual ~JsonlFileScanner();
  int open(const common::ObString &secure_file_priv, const common::ObString &path,
           const ObFileFingerprint *expected_fingerprint = nullptr) override;
  int next_row(common::ObIArray<common::ObObj> &row, bool &has_row) override;
  int rescan() override;
  void close() override;
  const common::ObIArray<ObFileColumn> &get_columns() const override { return columns_; }
  const ObFileErrorLocation &get_error_location() const override { return error_location_; }
  const ObFileFingerprint &get_fingerprint() const { return input_.get_fingerprint(); }
  int64_t get_row_count() const override { return row_count_; }

private:
  struct Field
  {
    Field() : key_(), value_(), type_(common::ObNullType), position_(0) {}
    std::string key_;
    std::string value_;
    common::ObObjType type_;
    size_t position_;
  };

  int next_byte(char &ch, bool &has_byte);
  int read_record(bool &has_record);
  int parse_record();
  int parse_string(size_t &position, std::string &value);
  int parse_hex4(size_t &position, uint32_t &codepoint);
  int parse_value(size_t &position, Field &field);
  int append_string(std::string &value, const char *bytes, size_t length, size_t position);
  int infer_record();
  int align_record();
  int convert_field(int64_t column_idx, common::ObObj &obj) const;
  int fail(const char *reason, size_t position, int code = common::OB_INVALID_ARGUMENT);
  void skip_space(size_t &position) const;
  void reset_reader();

  ObFileInput input_;
  common::ObSEArray<ObFileColumn, 16> columns_;
  std::vector<std::string> column_names_;
  std::unordered_map<std::string, int64_t> column_ids_;
  std::vector<common::ObObjType> inferred_types_;
  std::vector<uint8_t> nullable_;
  std::vector<int64_t> field_ids_;
  std::vector<Field> fields_;
  std::string record_;
  char io_buffer_[64 * 1024];
  int64_t io_pos_;
  int64_t io_size_;
  int64_t line_;
  int64_t byte_offset_;
  int64_t record_line_;
  int64_t record_offset_;
  bool input_eof_;
  ObFileErrorLocation error_location_;
  int64_t row_count_;
};

} // namespace sql
} // namespace oceanbase
