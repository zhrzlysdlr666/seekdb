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
#include "lib/string/ob_string.h"

namespace oceanbase
{
namespace common
{
template <typename T> class ObIArray;
class ObObj;
struct ObObjPrintParams;
}
namespace sql
{

struct FileSqlCsvOptions
{
  char delimiter_ = ',';
  char quote_ = '"';
  char escape_ = '\\';
  bool header_ = false;
  std::string nullstr_;
};

class FileSqlCopyWriter
{
public:
  FileSqlCopyWriter();
  ~FileSqlCopyWriter();
  FileSqlCopyWriter(const FileSqlCopyWriter &) = delete;
  FileSqlCopyWriter &operator=(const FileSqlCopyWriter &) = delete;

  int open(const common::ObString &secure_root, const common::ObString &path,
           const FileSqlCsvOptions &options,
           const common::ObIArray<common::ObString> &headers);
  // UTF8MB4 and binary strings preserve their bytes; invalid UTF8 is rejected.
  int write_row(const common::ObIArray<common::ObObj> &row,
                const common::ObObjPrintParams &print_params);
  // Successful linkat publishes the complete file. A later close or staging-
  // unlink error can return OB_IO_ERROR with the published file retained.
  int commit();
  void abort();

private:
  int validate_parent() const;
  int create_temp();
  int write_header(const common::ObIArray<common::ObString> &headers);
  int write_bytes(const char *data, int64_t length);
  int unlink_temp();

private:
  int fd_;
  int dir_fd_;
  std::string secure_root_;
  std::string parent_;
  std::string filename_;
  char temp_name_[64];
  dev_t root_device_;
  ino_t root_inode_;
  dev_t parent_device_;
  ino_t parent_inode_;
  dev_t temp_device_;
  ino_t temp_inode_;
  bool temp_identity_valid_;
  FileSqlCsvOptions options_;
};

} // namespace sql
} // namespace oceanbase
