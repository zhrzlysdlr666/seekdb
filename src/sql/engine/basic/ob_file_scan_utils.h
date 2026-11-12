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

#include <sys/types.h>
#include <string>
#include <vector>
#include "common/object/ob_obj_type.h"
#include "lib/string/ob_string.h"
#include "lib/container/ob_se_array.h"

namespace oceanbase
{
namespace common
{
template <typename T> class ObIArray;
class ObObj;
}
namespace sql
{

enum FileSqlFormat : int64_t
{
  FILE_SQL_CSV = 0,
  FILE_SQL_JSONL = 1,
  FILE_SQL_PARQUET = 2
};

// Conversion to VARCHAR borrows value's storage; the caller owns its lifetime.
bool file_sql_valid_utf8(const std::string &value);
common::ObObjType file_sql_infer_scalar(const std::string &value);
common::ObObjType file_sql_merge_types(common::ObObjType left, common::ObObjType right);
int file_sql_convert_scalar(const std::string &value, common::ObObjType type, common::ObObj &obj);

struct ObFileScanLimits
{
  static const int64_t MAX_COLUMNS = 4096;
  static const int64_t MAX_FIELD_BYTES = 16 * 1024 * 1024;
  static const int64_t MAX_RECORD_BYTES = 64 * 1024 * 1024;
  static const int64_t MAX_PARQUET_PAGE_BYTES = 64 * 1024 * 1024;
  static const int64_t MAX_GLOB_MATCHES = 10000;
};

struct ObFileFingerprint
{
  uint64_t device_;
  uint64_t inode_;
  int64_t size_;
  int64_t mtime_sec_;
  int64_t mtime_nsec_;
  int64_t ctime_sec_;
  int64_t ctime_nsec_;

  ObFileFingerprint();
  bool operator==(const ObFileFingerprint &other) const;
};

class ObFileInput
{
public:
  ObFileInput();
  ~ObFileInput();
  ObFileInput(const ObFileInput &) = delete;
  ObFileInput &operator=(const ObFileInput &) = delete;

  int open(const common::ObString &secure_file_priv, const common::ObString &path,
           const ObFileFingerprint *expected_fingerprint = nullptr);
  int read(void *buffer, int64_t capacity, int64_t &read_bytes);
  int rescan();
  int validate_unchanged() const { return verify_unchanged(); }
  void close();
  int get_fd() const { return fd_; }
  const std::string &get_canonical_path() const { return canonical_path_; }
  const ObFileFingerprint &get_fingerprint() const { return fingerprint_; }

private:
  int verify_unchanged() const;
  static bool is_within_directory(const std::string &path, const std::string &directory);
  int open_relative_file(int flags) const;

private:
  int fd_;
  int root_fd_;
  std::string canonical_path_;
  std::string relative_path_;
  std::string canonical_root_;
  ObFileFingerprint fingerprint_;
  int64_t read_offset_;
};

struct ObCsvScanOptions
{
  ObCsvScanOptions()
    : field_terminator_(','), field_enclosure_('"'), field_escape_('\\'), has_header_(true),
      empty_as_null_(true), null_string_(), sample_size_(-1)
  {}

  char field_terminator_;
  char field_enclosure_;
  char field_escape_;
  bool has_header_;
  bool empty_as_null_;
  std::string null_string_;
  int64_t sample_size_;
};

struct ObFileColumn
{
  common::ObString name_;
  common::ObObjType type_;
  bool nullable_;

  ObFileColumn() : type_(common::ObNullType), nullable_(false) {}
};

struct ObFileErrorLocation
{
  int64_t line_;
  int64_t column_;
  int64_t byte_offset_;

  ObFileErrorLocation() : line_(0), column_(0), byte_offset_(0) {}
};

class ObFileScanner
{
public:
  ObFileScanner() : has_projection_(false), projection_() {}
  virtual ~ObFileScanner() {}
  virtual int open(const common::ObString &secure_file_priv,
                   const common::ObString &path,
                   const ObFileFingerprint *expected_fingerprint = nullptr) = 0;
  virtual int next_row(common::ObIArray<common::ObObj> &row, bool &has_row) = 0;
  virtual int rescan() = 0;
  virtual void close() = 0;
  virtual const common::ObIArray<ObFileColumn> &get_columns() const = 0;
  virtual const ObFileErrorLocation &get_error_location() const = 0;
  virtual int64_t get_row_count() const { return -1; }
  virtual int set_projection(const common::ObIArray<int64_t> &column_ids)
  {
    int ret = common::OB_SUCCESS;
    if (column_ids.count() > ObFileScanLimits::MAX_COLUMNS) {
      ret = common::OB_INVALID_ARGUMENT;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < column_ids.count(); ++i) {
      if (column_ids.at(i) < 0 || column_ids.at(i) >= get_columns().count()) {
        ret = common::OB_INVALID_ARGUMENT;
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(projection_.assign(column_ids))) {
    } else if (OB_SUCC(ret)) {
      has_projection_ = true;
    }
    return ret;
  }
protected:
  void reset_projection() { has_projection_ = false; projection_.reuse(); }
  bool has_projection_;
  common::ObSEArray<int64_t, 16> projection_;
};

class ObCsvFileScanner : public ObFileScanner
{
public:
  ObCsvFileScanner();
  virtual ~ObCsvFileScanner();
  int open(const common::ObString &secure_file_priv,
           const common::ObString &path,
           const ObFileFingerprint *expected_fingerprint = nullptr) override;
  int open(const common::ObString &secure_file_priv,
           const common::ObString &path,
           const ObCsvScanOptions &options,
           const ObFileFingerprint *expected_fingerprint = nullptr);
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
    std::string value_;
    bool quoted_;
    int64_t line_;
    int64_t byte_offset_;
  };

  int read_record(bool &has_record);
  int infer_record();
  int convert_field(int64_t column_idx, common::ObObj &obj) const;
  int next_byte(char &ch, bool &has_byte);
  void unread_byte(char ch);
  void reset_reader();
  void set_error_location(int64_t line, int64_t column, int64_t byte_offset);
  bool is_null_field(const Field &field) const;

  ObFileInput input_;
  ObCsvScanOptions options_;
  common::ObSEArray<ObFileColumn, 16> columns_;
  std::vector<Field> fields_;
  std::vector<std::string> column_names_;
  std::vector<common::ObObjType> inferred_types_;
  std::vector<uint8_t> nullable_;
  char io_buffer_[64 * 1024];
  int64_t io_pos_;
  int64_t io_size_;
  int64_t line_;
  int64_t byte_offset_;
  int64_t record_line_;
  int64_t record_offset_;
  char pending_byte_;
  bool has_pending_byte_;
  bool input_eof_;
  ObFileErrorLocation error_location_;
  int64_t row_count_;
};

} // namespace sql
} // namespace oceanbase
