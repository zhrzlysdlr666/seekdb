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
#include "sql/engine/basic/ob_file_sql_copy_writer.h"

#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <memory>
#include <new>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#include "common/object/ob_object.h"
#include "lib/charset/ob_charset.h"
#include "lib/container/ob_iarray.h"
#include "lib/ob_errno.h"
#include "share/ob_errno.h"
#include "sql/engine/basic/ob_file_scan_utils.h"

namespace oceanbase
{
namespace sql
{
namespace
{

bool within_directory(const char *path, const std::string &directory)
{
  const size_t length = std::strlen(path);
  return (directory == "/" && length > 0 && path[0] == '/')
      || (length == directory.size() && 0 == std::memcmp(path, directory.data(), length))
      || (length > directory.size()
          && 0 == std::memcmp(path, directory.data(), directory.size())
          && path[directory.size()] == '/');
}

bool disabled_root(const common::ObString &root)
{
  bool disabled = root.empty() || root.ptr() == nullptr || root.length() >= PATH_MAX;
  if (!disabled) {
    disabled = std::memchr(root.ptr(), '\0', root.length()) != nullptr;
    if (!disabled && root.length() == 4) {
      static const char null_text[] = "NULL";
      disabled = true;
      for (int64_t i = 0; disabled && i < 4; ++i) {
        disabled = std::toupper(static_cast<unsigned char>(root.ptr()[i])) == null_text[i];
      }
    }
  }
  return disabled;
}

int validate_options(const FileSqlCsvOptions &options)
{
  int ret = common::OB_SUCCESS;
  if (options.delimiter_ == '\0' || options.quote_ == '\0' || options.escape_ == '\0'
      || options.delimiter_ == options.quote_
      || options.delimiter_ == '\r' || options.delimiter_ == '\n'
      || options.quote_ == '\r' || options.quote_ == '\n'
      || options.escape_ == '\r' || options.escape_ == '\n') {
    ret = common::OB_INVALID_ARGUMENT;
  } else if (options.nullstr_.size() > static_cast<size_t>(ObFileScanLimits::MAX_FIELD_BYTES)) {
    ret = common::OB_SIZE_OVERFLOW;
  } else {
    // The NULL marker must remain an unquoted, single CSV field.
    for (size_t i = 0; OB_SUCC(ret) && i < options.nullstr_.size(); ++i) {
      const char ch = options.nullstr_[i];
      if (ch == options.delimiter_ || ch == options.quote_ || ch == '\r' || ch == '\n') {
        ret = common::OB_INVALID_ARGUMENT;
      }
    }
  }
  return ret;
}

struct FormatBuffer
{
  FormatBuffer() : data_(), capacity_(0) {}

  int format(const common::ObObj &value, const common::ObObjPrintParams &params, int64_t &length)
  {
    int ret = common::OB_SUCCESS;
    bool done = false;
    while (OB_SUCC(ret) && !done) {
      if (capacity_ == 0) {
        capacity_ = 256;
        data_.reset(new (std::nothrow) char[capacity_]);
        if (data_ == nullptr) {
          ret = common::OB_ALLOCATE_MEMORY_FAILED;
        }
      }
      if (OB_SUCC(ret)) {
        length = 0;
        ret = value.print_plain_str_literal(data_.get(), capacity_, length, params);
        if (ret == common::OB_SIZE_OVERFLOW && capacity_ < ObFileScanLimits::MAX_FIELD_BYTES) {
          const int64_t next_capacity = capacity_ > ObFileScanLimits::MAX_FIELD_BYTES / 2
              ? ObFileScanLimits::MAX_FIELD_BYTES : capacity_ * 2;
          // The prior failed format is disposable; avoid retaining two
          // format buffers while growing.
          data_.reset();
          capacity_ = next_capacity;
          data_.reset(new (std::nothrow) char[capacity_]);
          ret = data_ == nullptr ? common::OB_ALLOCATE_MEMORY_FAILED : common::OB_SUCCESS;
        } else {
          done = true;
          if (OB_SUCC(ret) && (length < 0 || length > capacity_
              || length > ObFileScanLimits::MAX_FIELD_BYTES)) {
            ret = common::OB_SIZE_OVERFLOW;
          }
        }
      }
    }
    return ret;
  }

  std::unique_ptr<char[]> data_;
  int64_t capacity_;
};

int get_cell(const common::ObObj &value, const common::ObObjPrintParams &params,
             const FileSqlCsvOptions &options, FormatBuffer &format,
             const char *&data, int64_t &length, bool &quoted)
{
  int ret = common::OB_SUCCESS;
  data = nullptr;
  length = 0;
  quoted = false;
  const common::ObObjType type = value.get_type();
  if (value.is_null()) {
    data = options.nullstr_.data();
    length = static_cast<int64_t>(options.nullstr_.size());
  } else if (type == common::ObVarcharType || type == common::ObCharType
      || type == common::ObHexStringType) {
    length = value.get_string_len();
    data = value.get_string_ptr();
    quoted = true;
    if (length < 0 || (length > 0 && data == nullptr)) {
      ret = common::OB_INVALID_ARGUMENT;
    } else if (length > ObFileScanLimits::MAX_FIELD_BYTES) {
      ret = common::OB_SIZE_OVERFLOW;
    } else {
      const common::ObCharsetType charset =
          common::ObCharset::charset_type_by_coll(value.get_collation_type());
      if (charset != common::CHARSET_UTF8MB4 && charset != common::CHARSET_BINARY) {
        ret = common::OB_NOT_SUPPORTED;
      } else {
        // The retained native charsets are UTF8MB4 and binary. Native binary
        // casts preserve bytes; do not reinterpret them as latin1 or replace
        // invalid sequences. Every CSV string must still be valid UTF8.
        int64_t well_formed_length = 0;
        if (OB_FAIL(common::ObCharset::well_formed_len(
            common::CS_TYPE_UTF8MB4_BIN, data, length, well_formed_length))) {
          if (ret == common::OB_ERR_INCORRECT_STRING_VALUE) {
            ret = common::OB_INVALID_ARGUMENT;
          }
        } else if (well_formed_length != length) {
          ret = common::OB_INVALID_ARGUMENT;
        }
      }
    }
  } else if ((type >= common::ObTinyIntType && type <= common::ObYearType)
      || type == common::ObBitType || type == common::ObDecimalIntType
      || type == common::ObMySQLDateType || type == common::ObMySQLDateTimeType) {
    if (OB_FAIL(format.format(value, params, length))) {
    } else {
      data = format.data_.get();
      // Scalar output needs CSV enclosure when a custom delimiter or NULL
      // marker would otherwise change its meaning. No SQL quoting is added.
      quoted = length == 0 || (static_cast<size_t>(length) == options.nullstr_.size()
          && 0 == std::memcmp(data, options.nullstr_.data(), length));
      for (int64_t i = 0; !quoted && i < length; ++i) {
        quoted = data[i] == options.delimiter_ || data[i] == options.quote_
            || data[i] == '\r' || data[i] == '\n';
      }
    }
  } else {
    // LOBs, enum/set values needing schema metadata, and composite values
    // require materialization that this bounded streaming writer does not do.
    ret = common::OB_NOT_SUPPORTED;
  }
  return ret;
}

int encoded_length(const char *data, int64_t length, bool quoted,
                   const FileSqlCsvOptions &options, int64_t &encoded)
{
  int ret = common::OB_SUCCESS;
  encoded = 0;
  if (length < 0 || (length > 0 && data == nullptr)) {
    ret = common::OB_INVALID_ARGUMENT;
  } else if (length > ObFileScanLimits::MAX_FIELD_BYTES) {
    ret = common::OB_SIZE_OVERFLOW;
  } else {
    encoded = length + (quoted ? 2 : 0);
    for (int64_t i = 0; quoted && i < length; ++i) {
      if (data[i] == options.quote_ || data[i] == options.escape_) {
        ++encoded;
      }
    }
  }
  return ret;
}

int append_cell(char *record, int64_t capacity, int64_t &pos, const char *data,
                int64_t length, bool quoted, const FileSqlCsvOptions &options)
{
  int ret = common::OB_SUCCESS;
  int64_t encoded = 0;
  if (OB_FAIL(encoded_length(data, length, quoted, options, encoded))) {
  } else if (pos < 0 || pos > capacity || encoded > capacity - pos) {
    ret = common::OB_SIZE_OVERFLOW;
  } else {
    if (quoted) {
      record[pos++] = options.quote_;
    }
    for (int64_t i = 0; i < length; ++i) {
      if (quoted && (data[i] == options.quote_ || data[i] == options.escape_)) {
        // When escape equals quote, this emits the scanner's doubled quote.
        record[pos++] = options.escape_;
      }
      record[pos++] = data[i];
    }
    if (quoted) {
      record[pos++] = options.quote_;
    }
  }
  return ret;
}

} // namespace

FileSqlCopyWriter::FileSqlCopyWriter()
  : fd_(-1), dir_fd_(-1), secure_root_(), parent_(), filename_(), temp_name_{0},
    root_device_(0), root_inode_(0), parent_device_(0), parent_inode_(0),
    temp_device_(0), temp_inode_(0), temp_identity_valid_(false), options_()
{
}

FileSqlCopyWriter::~FileSqlCopyWriter()
{
  abort();
}

int FileSqlCopyWriter::validate_parent() const
{
  int ret = common::OB_SUCCESS;
  char root_path[PATH_MAX] = {0};
  char parent_path[PATH_MAX] = {0};
  struct stat root_stat;
  struct stat parent_stat;
  struct stat dir_stat;
  if (dir_fd_ < 0) {
    ret = common::OB_NOT_INIT;
  } else if (::realpath(secure_root_.c_str(), root_path) == nullptr
      || secure_root_ != root_path || ::lstat(root_path, &root_stat) != 0
      || !S_ISDIR(root_stat.st_mode)
      || root_stat.st_dev != root_device_ || root_stat.st_ino != root_inode_) {
    ret = common::OB_ERR_NO_PRIVILEGE;
  } else if (::realpath(parent_.c_str(), parent_path) == nullptr) {
    ret = (errno == ENOENT || errno == ENOTDIR) ? common::OB_FILE_NOT_EXIST : common::OB_IO_ERROR;
  } else if (parent_ != parent_path || !within_directory(parent_path, secure_root_)) {
    ret = common::OB_ERR_NO_PRIVILEGE;
  } else if (::lstat(parent_path, &parent_stat) != 0 || ::fstat(dir_fd_, &dir_stat) != 0) {
    ret = common::OB_IO_ERROR;
  } else if (!S_ISDIR(parent_stat.st_mode) || !S_ISDIR(dir_stat.st_mode)
      || parent_stat.st_dev != parent_device_ || parent_stat.st_ino != parent_inode_
      || dir_stat.st_dev != parent_device_ || dir_stat.st_ino != parent_inode_) {
    ret = common::OB_ERR_NO_PRIVILEGE;
  }
  return ret;
}

int FileSqlCopyWriter::create_temp()
{
  int ret = common::OB_SUCCESS;
  static const char prefix[] = ".seekdb-copy-";
  static const char hex[] = "0123456789abcdef";
  for (int attempt = 0; OB_SUCC(ret) && fd_ < 0 && attempt < 32; ++attempt) {
    unsigned char random_bytes[16];
    size_t random_pos = 0;
    while (OB_SUCC(ret) && random_pos < sizeof(random_bytes)) {
      const ssize_t count = ::getrandom(random_bytes + random_pos,
                                       sizeof(random_bytes) - random_pos, 0);
      if (count < 0 && errno == EINTR) {
      } else if (count <= 0) {
        ret = common::OB_IO_ERROR;
      } else {
        random_pos += static_cast<size_t>(count);
      }
    }
    if (OB_SUCC(ret)) {
      char name[sizeof(temp_name_)] = {0};
      std::memcpy(name, prefix, sizeof(prefix) - 1);
      for (size_t i = 0; i < sizeof(random_bytes); ++i) {
        name[sizeof(prefix) - 1 + i * 2] = hex[random_bytes[i] >> 4];
        name[sizeof(prefix) + i * 2] = hex[random_bytes[i] & 15];
      }
      fd_ = ::openat(dir_fd_, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
      if (fd_ >= 0) {
        std::memcpy(temp_name_, name, sizeof(temp_name_));
        struct stat temp_stat;
        if (::fstat(fd_, &temp_stat) != 0 || !S_ISREG(temp_stat.st_mode)) {
          ret = common::OB_IO_ERROR;
        } else {
          temp_device_ = temp_stat.st_dev;
          temp_inode_ = temp_stat.st_ino;
          temp_identity_valid_ = true;
        }
      } else if (errno != EEXIST) {
        ret = common::OB_IO_ERROR;
      }
    }
  }
  if (OB_SUCC(ret) && fd_ < 0) {
    ret = common::OB_IO_ERROR;
  }
  return ret;
}

int FileSqlCopyWriter::open(const common::ObString &secure_root, const common::ObString &path,
                            const FileSqlCsvOptions &options,
                            const common::ObIArray<common::ObString> &headers)
{
  int ret = common::OB_SUCCESS;
  abort();
  if (disabled_root(secure_root)) {
    ret = common::OB_ERR_NO_PRIVILEGE;
  } else if (path.empty() || path.ptr() == nullptr || path.length() >= PATH_MAX
      || std::memchr(path.ptr(), '\0', path.length()) != nullptr) {
    ret = common::OB_INVALID_ARGUMENT;
  } else if (headers.count() > ObFileScanLimits::MAX_COLUMNS) {
    ret = common::OB_SIZE_OVERFLOW;
  } else if (options.header_ && headers.count() == 0) {
    ret = common::OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(validate_options(options))) {
  } else {
    const std::string root_input(secure_root.ptr(), secure_root.length());
    const std::string path_input(path.ptr(), path.length());
    const size_t slash = path_input.find_last_of('/');
    const std::string parent_input = slash == std::string::npos ? "."
        : slash == 0 ? "/" : path_input.substr(0, slash);
    filename_ = slash == std::string::npos ? path_input : path_input.substr(slash + 1);
    char root_path[PATH_MAX] = {0};
    char parent_path[PATH_MAX] = {0};
    struct stat root_stat;
    struct stat parent_stat;
    if (filename_.empty() || filename_ == "." || filename_ == ".." || filename_.size() > NAME_MAX) {
      ret = common::OB_INVALID_ARGUMENT;
    } else if (::realpath(root_input.c_str(), root_path) == nullptr
        || ::stat(root_path, &root_stat) != 0 || !S_ISDIR(root_stat.st_mode)) {
      ret = common::OB_ERR_NO_PRIVILEGE;
    } else if (::realpath(parent_input.c_str(), parent_path) == nullptr) {
      ret = errno == ENAMETOOLONG ? common::OB_INVALID_ARGUMENT
          : (errno == ENOENT || errno == ENOTDIR) ? common::OB_FILE_NOT_EXIST : common::OB_IO_ERROR;
    } else {
      secure_root_.assign(root_path);
      parent_.assign(parent_path);
      root_device_ = root_stat.st_dev;
      root_inode_ = root_stat.st_ino;
      if (!within_directory(parent_path, secure_root_)) {
        ret = common::OB_ERR_NO_PRIVILEGE;
      } else if (::stat(parent_path, &parent_stat) != 0 || !S_ISDIR(parent_stat.st_mode)) {
        ret = common::OB_FILE_NOT_EXIST;
      } else {
        parent_device_ = parent_stat.st_dev;
        parent_inode_ = parent_stat.st_ino;
        dir_fd_ = ::open(parent_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (dir_fd_ < 0) {
          ret = common::OB_IO_ERROR;
        } else if (OB_FAIL(validate_parent())) {
        } else {
          struct stat target_stat;
          if (::fstatat(dir_fd_, filename_.c_str(), &target_stat, AT_SYMLINK_NOFOLLOW) == 0) {
            ret = common::OB_FILE_ALREADY_EXIST;
          } else if (errno != ENOENT) {
            ret = common::OB_IO_ERROR;
          } else {
            // Validate the marker before copying its potentially large string.
            options_ = options;
            if (OB_FAIL(create_temp())) {
            } else if (options_.header_ && OB_FAIL(write_header(headers))) {
            }
          }
        }
      }
    }
  }
  if (OB_FAIL(ret)) {
    abort();
  }
  return ret;
}

int FileSqlCopyWriter::write_header(const common::ObIArray<common::ObString> &headers)
{
  int ret = common::OB_SUCCESS;
  // Delimiters plus the final newline occupy exactly one byte per column.
  int64_t record_size = headers.count();
  for (int64_t i = 0; OB_SUCC(ret) && i < headers.count(); ++i) {
    int64_t encoded = 0;
    const common::ObString &name = headers.at(i);
    if (OB_FAIL(encoded_length(name.ptr(), name.length(), true, options_, encoded))) {
    } else if (encoded > ObFileScanLimits::MAX_RECORD_BYTES - record_size) {
      ret = common::OB_SIZE_OVERFLOW;
    } else {
      record_size += encoded;
    }
  }
  std::unique_ptr<char[]> record;
  if (OB_SUCC(ret)) {
    record.reset(new (std::nothrow) char[record_size]);
    if (record == nullptr) {
      ret = common::OB_ALLOCATE_MEMORY_FAILED;
    }
  }
  int64_t pos = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < headers.count(); ++i) {
    const common::ObString &name = headers.at(i);
    if (OB_FAIL(append_cell(record.get(), record_size, pos, name.ptr(), name.length(), true, options_))) {
    } else if (pos >= record_size) {
      ret = common::OB_SIZE_OVERFLOW;
    } else {
      record[pos++] = i + 1 == headers.count() ? '\n' : options_.delimiter_;
    }
  }
  if (OB_SUCC(ret)) {
    ret = write_bytes(record.get(), pos);
  }
  return ret;
}

int FileSqlCopyWriter::write_row(const common::ObIArray<common::ObObj> &row,
                                 const common::ObObjPrintParams &print_params)
{
  int ret = common::OB_SUCCESS;
  if (fd_ < 0 || dir_fd_ < 0) {
    ret = common::OB_NOT_INIT;
  } else if (row.count() > ObFileScanLimits::MAX_COLUMNS) {
    ret = common::OB_SIZE_OVERFLOW;
  } else if (row.count() == 0) {
    ret = common::OB_INVALID_ARGUMENT;
  } else {
    FormatBuffer format;
    int64_t record_size = row.count();
    // Measure every encoded field before allocating the current row. Only a
    // bounded scalar-format buffer is retained between the two passes.
    for (int64_t i = 0; OB_SUCC(ret) && i < row.count(); ++i) {
      const char *data = nullptr;
      int64_t length = 0;
      int64_t encoded = 0;
      bool quoted = false;
      if (OB_FAIL(get_cell(row.at(i), print_params, options_, format, data, length, quoted))) {
      } else if (OB_FAIL(encoded_length(data, length, quoted, options_, encoded))) {
      } else if (encoded > ObFileScanLimits::MAX_RECORD_BYTES - record_size) {
        ret = common::OB_SIZE_OVERFLOW;
      } else {
        record_size += encoded;
      }
    }
    std::unique_ptr<char[]> record;
    if (OB_SUCC(ret)) {
      record.reset(new (std::nothrow) char[record_size]);
      if (record == nullptr) {
        ret = common::OB_ALLOCATE_MEMORY_FAILED;
      }
    }
    int64_t pos = 0;
    for (int64_t i = 0; OB_SUCC(ret) && i < row.count(); ++i) {
      const char *data = nullptr;
      int64_t length = 0;
      bool quoted = false;
      if (OB_FAIL(get_cell(row.at(i), print_params, options_, format, data, length, quoted))) {
      } else if (OB_FAIL(append_cell(record.get(), record_size, pos, data, length, quoted, options_))) {
      } else if (pos >= record_size) {
        ret = common::OB_SIZE_OVERFLOW;
      } else {
        record[pos++] = i + 1 == row.count() ? '\n' : options_.delimiter_;
      }
    }
    if (OB_SUCC(ret)) {
      ret = write_bytes(record.get(), pos);
    }
  }
  if (OB_FAIL(ret)) {
    abort();
  }
  return ret;
}

int FileSqlCopyWriter::write_bytes(const char *data, int64_t length)
{
  int ret = common::OB_SUCCESS;
  int64_t pos = 0;
  if (length < 0 || (length > 0 && data == nullptr)) {
    ret = common::OB_INVALID_ARGUMENT;
  } else if (length > ObFileScanLimits::MAX_RECORD_BYTES) {
    ret = common::OB_SIZE_OVERFLOW;
  }
  while (OB_SUCC(ret) && pos < length) {
    const ssize_t count = ::write(fd_, data + pos, static_cast<size_t>(length - pos));
    if (count < 0 && errno == EINTR) {
    } else if (count <= 0) {
      ret = common::OB_IO_ERROR;
    } else {
      pos += count;
    }
  }
  return ret;
}

int FileSqlCopyWriter::unlink_temp()
{
  int ret = common::OB_SUCCESS;
  if (dir_fd_ >= 0 && temp_name_[0] != '\0') {
    int result = 0;
    struct stat temp_stat;
    do {
      result = ::fstatat(dir_fd_, temp_name_, &temp_stat, AT_SYMLINK_NOFOLLOW);
    } while (result < 0 && errno == EINTR);
    if (result < 0 && errno == ENOENT) {
      temp_name_[0] = '\0';
      temp_identity_valid_ = false;
    } else if (result < 0) {
      ret = common::OB_IO_ERROR;
    } else if (temp_identity_valid_
        && (temp_stat.st_dev != temp_device_ || temp_stat.st_ino != temp_inode_)) {
      // A replaced staging entry belongs to somebody else; do not remove it.
      temp_name_[0] = '\0';
      temp_identity_valid_ = false;
      ret = common::OB_IO_ERROR;
    } else {
      do {
        result = ::unlinkat(dir_fd_, temp_name_, 0);
      } while (result < 0 && errno == EINTR);
      if (result == 0 || errno == ENOENT) {
        temp_name_[0] = '\0';
        temp_identity_valid_ = false;
      } else {
        ret = common::OB_IO_ERROR;
      }
    }
  }
  return ret;
}

int FileSqlCopyWriter::commit()
{
  int ret = common::OB_SUCCESS;
  bool published = false;
  if (fd_ < 0 || dir_fd_ < 0 || temp_name_[0] == '\0') {
    ret = common::OB_NOT_INIT;
  } else {
    int result = 0;
    do {
      result = ::fsync(fd_);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
      ret = common::OB_IO_ERROR;
    } else if (OB_FAIL(validate_parent())) {
    } else {
      // Keep the descriptor open until publication. Resolving this procfs
      // link pins the inode we wrote, even if the staging name is replaced.
      char descriptor_path[64] = {0};
      const int path_length = std::snprintf(
          descriptor_path, sizeof(descriptor_path), "/proc/self/fd/%d", fd_);
      if (path_length < 0 || path_length >= static_cast<int>(sizeof(descriptor_path))) {
        ret = common::OB_ERR_UNEXPECTED;
      } else {
        do {
          result = ::linkat(AT_FDCWD, descriptor_path, dir_fd_, filename_.c_str(), AT_SYMLINK_FOLLOW);
        } while (result < 0 && errno == EINTR);
        if (result < 0) {
          ret = errno == EEXIST ? common::OB_FILE_ALREADY_EXIST : common::OB_IO_ERROR;
        } else {
          // Publication is the commit point. Never unlink the final path:
          // another process may already have replaced that directory entry.
          published = true;
          // On Linux close consumes the descriptor even when it reports
          // EINTR; retrying could close a descriptor reused by another thread.
          const int fd = fd_;
          fd_ = -1;
          if (::close(fd) != 0) {
            ret = common::OB_IO_ERROR;
          }
          const int cleanup_ret = unlink_temp();
          if (OB_SUCC(ret)) {
            ret = cleanup_ret;
          }
          if (cleanup_ret == common::OB_SUCCESS) {
            const int dir_fd = dir_fd_;
            dir_fd_ = -1;
            if (::close(dir_fd) != 0 && OB_SUCC(ret)) {
              ret = common::OB_IO_ERROR;
            }
          }
        }
      }
    }
  }
  if (!published) {
    abort();
  }
  // After publication a failed unlink leaves the directory descriptor and
  // staging name available for abort/destruction to retry the cleanup.
  return ret;
}

void FileSqlCopyWriter::abort()
{
  if (fd_ >= 0) {
    const int fd = fd_;
    fd_ = -1;
    (void)::close(fd);
  }
  (void)unlink_temp();
  if (dir_fd_ >= 0) {
    const int dir_fd = dir_fd_;
    dir_fd_ = -1;
    (void)::close(dir_fd);
  }
  temp_name_[0] = '\0';
  temp_identity_valid_ = false;
  secure_root_.clear();
  parent_.clear();
  filename_.clear();
  options_ = FileSqlCsvOptions();
}

} // namespace sql
} // namespace oceanbase
