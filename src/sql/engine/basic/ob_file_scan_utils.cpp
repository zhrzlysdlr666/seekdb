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
#include "sql/engine/basic/ob_file_scan_utils.h"

#include <cerrno>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/openat2.h>
#include <locale.h>
#include <limits.h>
#include <limits>
#include <set>
#include <strings.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "common/object/ob_object.h"
#include "common/timezone/ob_time_convert.h"
#include "lib/ob_errno.h"
#include "share/ob_errno.h"

namespace oceanbase
{
namespace sql
{

static bool is_null_path(const common::ObString &path)
{
  static const char null_text[] = "NULL";
  bool is_null = path.length() == static_cast<int64_t>(sizeof(null_text) - 1);
  for (int64_t i = 0; is_null && i < path.length(); ++i) {
    const char ch = path.ptr()[i];
    is_null = static_cast<char>(toupper(static_cast<unsigned char>(ch))) == null_text[i];
  }
  return is_null;
}

bool file_sql_valid_utf8(const std::string &value)
{
  bool valid = true;
  for (size_t i = 0; valid && i < value.size();) {
    const unsigned char first = static_cast<unsigned char>(value[i]);
    size_t continuation_count = 0;
    if (first <= 0x7f) {
      continuation_count = 0;
    } else if (first >= 0xc2 && first <= 0xdf) {
      continuation_count = 1;
    } else if (first >= 0xe0 && first <= 0xef) {
      continuation_count = 2;
    } else if (first >= 0xf0 && first <= 0xf4) {
      continuation_count = 3;
    } else {
      valid = false;
    }
    if (valid && i + continuation_count >= value.size()) {
      valid = false;
    }
    for (size_t j = 1; valid && j <= continuation_count; ++j) {
      const unsigned char next = static_cast<unsigned char>(value[i + j]);
      valid = (next & 0xc0) == 0x80;
      if (valid && j == 1) {
        valid = !(first == 0xe0 && next < 0xa0) && !(first == 0xed && next >= 0xa0)
            && !(first == 0xf0 && next < 0x90) && !(first == 0xf4 && next >= 0x90);
      }
    }
    i += continuation_count + 1;
  }
  return valid;
}

static bool is_digit(char ch)
{
  return ch >= '0' && ch <= '9';
}

static bool is_boolean(const std::string &value, bool &parsed)
{
  const bool is_true = value.size() == 4 && 0 == strncasecmp(value.data(), "true", 4);
  const bool is_false = value.size() == 5 && 0 == strncasecmp(value.data(), "false", 5);
  parsed = is_true;
  return is_true || is_false;
}

// Distinguish integer syntax from its range, so an overflowing integer never
// silently passes through a lossy floating-point fallback.
static bool parse_integer(const std::string &value, int64_t &parsed, bool &in_range)
{
  size_t pos = (!value.empty() && (value[0] == '-' || value[0] == '+')) ? 1 : 0;
  const bool negative = !value.empty() && value[0] == '-';
  bool valid = pos < value.size();
  in_range = true;
  uint64_t magnitude = 0;
  const uint64_t limit = negative ? (uint64_t(1) << 63) : static_cast<uint64_t>(INT64_MAX);
  for (; valid && pos < value.size(); ++pos) {
    if (!is_digit(value[pos])) {
      valid = false;
    } else if (in_range) {
      const uint64_t digit = value[pos] - '0';
      if (magnitude > (limit - digit) / 10) {
        in_range = false;
      } else {
        magnitude = magnitude * 10 + digit;
      }
    }
  }
  if (valid && in_range) {
    parsed = negative ? (magnitude == (uint64_t(1) << 63) ? INT64_MIN
        : -static_cast<int64_t>(magnitude)) : static_cast<int64_t>(magnitude);
  }
  return valid;
}

static bool parse_double(const std::string &value, double &parsed)
{
  size_t pos = (!value.empty() && (value[0] == '+' || value[0] == '-')) ? 1 : 0;
  const size_t integer_start = pos;
  while (pos < value.size() && is_digit(value[pos])) { ++pos; }
  bool has_digits = pos > integer_start;
  if (pos < value.size() && value[pos] == '.') {
    const size_t fraction_start = ++pos;
    while (pos < value.size() && is_digit(value[pos])) { ++pos; }
    has_digits = has_digits || pos > fraction_start;
  }
  bool valid = has_digits;
  if (valid && pos < value.size() && (value[pos] == 'e' || value[pos] == 'E')) {
    ++pos;
    if (pos < value.size() && (value[pos] == '+' || value[pos] == '-')) { ++pos; }
    const size_t exponent_start = pos;
    while (pos < value.size() && is_digit(value[pos])) { ++pos; }
    valid = pos > exponent_start;
  }
  valid = valid && pos == value.size();
  if (valid) {
    struct NumericLocale {
      NumericLocale() : locale_(::newlocale(LC_NUMERIC_MASK, "C", nullptr)) {}
      ~NumericLocale() { if (locale_ != nullptr) { ::freelocale(locale_); } }
      locale_t locale_;
    };
    static const NumericLocale numeric_locale;
    if (numeric_locale.locale_ == nullptr) {
      valid = false;
    } else {
      char *end = nullptr;
      parsed = ::strtod_l(value.c_str(), &end, numeric_locale.locale_);
      valid = end == value.data() + value.size() && std::isfinite(parsed);
    }
  }
  return valid;
}

static bool is_iso_date(const std::string &value)
{
  bool valid = value.size() == 10 && value[4] == '-' && value[7] == '-';
  for (size_t i = 0; valid && i < value.size(); ++i) {
    if (i != 4 && i != 7) {
      valid = is_digit(value[i]);
    }
  }
  if (valid) {
    const int year = (value[0] - '0') * 1000 + (value[1] - '0') * 100
        + (value[2] - '0') * 10 + value[3] - '0';
    const int month = (value[5] - '0') * 10 + value[6] - '0';
    const int day = (value[8] - '0') * 10 + value[9] - '0';
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    valid = year > 0 && month >= 1 && month <= 12 && day >= 1;
    if (valid) {
      const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
      valid = day <= days[month - 1] + (month == 2 && leap ? 1 : 0);
    }
  }
  return valid;
}

static bool is_iso_datetime(const std::string &value)
{
  bool valid = value.size() >= 19 && value.size() <= 26 && is_iso_date(value.substr(0, 10))
      && (value[10] == ' ' || value[10] == 'T')
      && value[13] == ':' && value[16] == ':';
  for (size_t i = 0; valid && i < 19; ++i) {
    if (i != 4 && i != 7 && i != 10 && i != 13 && i != 16) {
      valid = is_digit(value[i]);
    }
  }
  if (valid && value.size() > 19) {
    valid = value[19] == '.' && value.size() > 20;
    for (size_t i = 20; valid && i < value.size(); ++i) {
      valid = is_digit(value[i]);
    }
  }
  if (valid) {
    valid = (value[11] - '0') * 10 + value[12] - '0' <= 23
        && (value[14] - '0') * 10 + value[15] - '0' <= 59
        && (value[17] - '0') * 10 + value[18] - '0' <= 59;
  }
  return valid;
}

common::ObObjType file_sql_infer_scalar(const std::string &value)
{
  common::ObObjType type = common::ObVarcharType;
  bool boolean_value = false;
  int64_t integer_value = 0;
  bool in_range = false;
  double double_value = 0;
  if (is_boolean(value, boolean_value)) {
    type = common::ObTinyIntType;
  } else if (parse_integer(value, integer_value, in_range)) {
    type = in_range ? common::ObIntType : common::ObVarcharType;
  } else if (parse_double(value, double_value)) {
    type = common::ObDoubleType;
  } else if (is_iso_date(value)) {
    type = common::ObMySQLDateType;
  } else if (is_iso_datetime(value)) {
    type = common::ObMySQLDateTimeType;
  }
  return type;
}

int file_sql_convert_scalar(const std::string &value, common::ObObjType type, common::ObObj &obj)
{
  int ret = common::OB_SUCCESS;
  int64_t integer_value = 0;
  bool in_range = false;
  bool boolean_value = false;
  double double_value = 0;
  if (type == common::ObVarcharType) {
    if (value.size() > static_cast<size_t>(INT32_MAX) || !file_sql_valid_utf8(value)) {
      ret = common::OB_INVALID_ARGUMENT;
    } else {
      obj.set_varchar(value.data(), static_cast<int32_t>(value.size()));
    }
  } else if (type == common::ObNullType) {
    obj.set_null();
  } else if (type == common::ObTinyIntType) {
    if (!is_boolean(value, boolean_value)) {
      ret = common::OB_INVALID_ARGUMENT;
    } else {
      obj.set_tinyint(boolean_value ? 1 : 0);
    }
  } else if (type == common::ObIntType) {
    if (!parse_integer(value, integer_value, in_range) || !in_range) {
      ret = common::OB_INVALID_ARGUMENT;
    } else {
      obj.set_int(integer_value);
    }
  } else if (type == common::ObDoubleType) {
    if (!parse_double(value, double_value)) {
      ret = common::OB_INVALID_ARGUMENT;
    } else {
      obj.set_double(double_value);
    }
  } else if (type == common::ObMySQLDateType || type == common::ObMySQLDateTimeType) {
    if (!is_iso_date(value) && !(type == common::ObMySQLDateTimeType && is_iso_datetime(value))) {
      ret = common::OB_INVALID_ARGUMENT;
    } else {
      std::string normalized = value;
      if (normalized.size() > 10) { normalized[10] = ' '; }
      common::ObTime ob_time;
      int16_t scale = 0;
      common::ObMySQLDateTime datetime = 0;
      const common::ObString str(static_cast<int32_t>(normalized.size()), normalized.data());
      if (OB_FAIL(common::ObTimeConverter::str_to_ob_time_with_date(str, ob_time, &scale, 0))) {
        ret = common::OB_INVALID_ARGUMENT;
      } else if (type == common::ObMySQLDateType) {
        obj.set_mysql_date(common::ObTimeConverter::ob_time_to_mdate(ob_time));
      } else if (OB_FAIL(common::ObTimeConverter::ob_time_to_mdatetime(ob_time, datetime))) {
        ret = common::OB_INVALID_ARGUMENT;
      } else {
        obj.set_mysql_datetime(datetime);
      }
    }
  } else {
    ret = common::OB_NOT_SUPPORTED;
  }
  return ret;
}

common::ObObjType file_sql_merge_types(common::ObObjType left, common::ObObjType right)
{
  common::ObObjType merged = left;
  if (common::ObNullType == left) {
    merged = right;
  } else if (common::ObNullType == right || left == right) {
  } else if ((common::ObIntType == left && common::ObDoubleType == right)
      || (common::ObDoubleType == left && common::ObIntType == right)) {
    merged = common::ObDoubleType;
  } else if ((common::ObMySQLDateType == left && common::ObMySQLDateTimeType == right)
      || (common::ObMySQLDateTimeType == left && common::ObMySQLDateType == right)) {
    merged = common::ObMySQLDateTimeType;
  } else {
    merged = common::ObVarcharType;
  }
  return merged;
}

ObFileFingerprint::ObFileFingerprint()
  : device_(0), inode_(0), size_(0), mtime_sec_(0), mtime_nsec_(0), ctime_sec_(0), ctime_nsec_(0)
{
}

bool ObFileFingerprint::operator==(const ObFileFingerprint &other) const
{
  return device_ == other.device_ && inode_ == other.inode_ && size_ == other.size_
      && mtime_sec_ == other.mtime_sec_ && mtime_nsec_ == other.mtime_nsec_
      && ctime_sec_ == other.ctime_sec_ && ctime_nsec_ == other.ctime_nsec_;
}

ObFileInput::ObFileInput() : fd_(-1), root_fd_(-1), canonical_path_(), relative_path_(), canonical_root_(),
                             fingerprint_(), read_offset_(0)
{
}

ObFileInput::~ObFileInput()
{
  close();
}

bool ObFileInput::is_within_directory(const std::string &path, const std::string &directory)
{
  bool within = false;
  if (directory == "/") {
    within = !path.empty() && path[0] == '/';
  } else if (path == directory) {
    within = true;
  } else if (path.size() > directory.size() && path.compare(0, directory.size(), directory) == 0) {
    within = path[directory.size()] == '/';
  }
  return within;
}

int ObFileInput::open_relative_file(int flags) const
{
  int fd = -1;
  if (root_fd_ >= 0 && !relative_path_.empty()) {
    struct open_how how = {};
    how.flags = static_cast<uint64_t>(flags);
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
    fd = static_cast<int>(::syscall(SYS_openat2, root_fd_, relative_path_.c_str(), &how, sizeof(how)));
  }
  return fd;
}

int ObFileInput::open(const common::ObString &secure_file_priv, const common::ObString &path,
                      const ObFileFingerprint *expected_fingerprint)
{
  int ret = common::OB_SUCCESS;
  close();
  if ((!secure_file_priv.empty() && memchr(secure_file_priv.ptr(), '\0', secure_file_priv.length()) != nullptr)
      || (!path.empty() && memchr(path.ptr(), '\0', path.length()) != nullptr)
      || path.empty() || path.length() >= PATH_MAX || secure_file_priv.length() >= PATH_MAX) {
    ret = common::OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file input path (empty, embedded NUL or excessive length)");
  } else if (secure_file_priv.empty() || is_null_path(secure_file_priv)) {
    ret = common::OB_ERR_NO_PRIVILEGE;
  } else {
    const std::string root_input(secure_file_priv.ptr(), secure_file_priv.length());
    const std::string path_input(path.ptr(), path.length());
    char root_buf[PATH_MAX] = {0};
    char path_buf[PATH_MAX] = {0};
    struct stat root_stat;
    struct stat path_stat;
    if (NULL == ::realpath(root_input.c_str(), root_buf)
        || 0 != ::stat(root_buf, &root_stat)
        || !S_ISDIR(root_stat.st_mode)) {
      ret = common::OB_ERR_NO_PRIVILEGE;
    } else if (NULL == ::realpath(path_input.c_str(), path_buf)) {
      ret = (ENOENT == errno || ENOTDIR == errno) ? common::OB_FILE_NOT_EXIST
          : (EACCES == errno || EPERM == errno) ? common::OB_ERR_NO_PRIVILEGE : common::OB_IO_ERROR;
    } else if (!is_within_directory(path_buf, root_buf)) {
      ret = common::OB_ERR_NO_PRIVILEGE;
    } else if (0 != ::stat(path_buf, &path_stat)) {
      ret = (ENOENT == errno || ENOTDIR == errno) ? common::OB_FILE_NOT_EXIST
          : (EACCES == errno || EPERM == errno) ? common::OB_ERR_NO_PRIVILEGE : common::OB_IO_ERROR;
    } else if (!S_ISREG(path_stat.st_mode)) {
      ret = common::OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file input (requires a regular file)");
    } else {
      canonical_root_.assign(root_buf);
      canonical_path_.assign(path_buf);
      relative_path_.assign(canonical_root_ == "/" ? path_buf + 1
          : path_buf + canonical_root_.size() + 1);
      ObFileFingerprint current_fingerprint;
      current_fingerprint.device_ = static_cast<uint64_t>(path_stat.st_dev);
      current_fingerprint.inode_ = static_cast<uint64_t>(path_stat.st_ino);
      current_fingerprint.size_ = static_cast<int64_t>(path_stat.st_size);
      current_fingerprint.mtime_sec_ = static_cast<int64_t>(path_stat.st_mtim.tv_sec);
      current_fingerprint.mtime_nsec_ = static_cast<int64_t>(path_stat.st_mtim.tv_nsec);
      current_fingerprint.ctime_sec_ = static_cast<int64_t>(path_stat.st_ctim.tv_sec);
      current_fingerprint.ctime_nsec_ = static_cast<int64_t>(path_stat.st_ctim.tv_nsec);
      if (expected_fingerprint != nullptr && !(current_fingerprint == *expected_fingerprint)) {
        ret = common::OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file input (changed since schema inference)");
      } else {
        fingerprint_ = (expected_fingerprint == nullptr) ? current_fingerprint : *expected_fingerprint;
      }
      struct stat opened_root;
      if (OB_SUCC(ret) && (root_fd_ = ::open(canonical_root_.c_str(),
          O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)) < 0) {
        ret = common::OB_ERR_NO_PRIVILEGE;
      } else if (OB_SUCC(ret) && (0 != ::fstat(root_fd_, &opened_root)
          || opened_root.st_dev != root_stat.st_dev || opened_root.st_ino != root_stat.st_ino)) {
        ret = common::OB_ERR_NO_PRIVILEGE;
      // openat2 anchors the open at the checked directory descriptor and
      // rejects symlinks in every descendant component.
      } else if (OB_SUCC(ret) && (fd_ = open_relative_file(
          O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)) < 0) {
        ret = (EACCES == errno || EPERM == errno) ? common::OB_ERR_NO_PRIVILEGE
            : (ENOENT == errno || ENOTDIR == errno) ? common::OB_FILE_NOT_EXIST
            : (ELOOP == errno || EXDEV == errno || ENOSYS == errno) ? common::OB_ERR_NO_PRIVILEGE
            : common::OB_IO_ERROR;
      } else if (OB_SUCC(ret) && OB_FAIL(verify_unchanged())) {
        close();
      }
    }
  }
  if (OB_FAIL(ret)) {
    close();
  }
  return ret;
}

int ObFileInput::verify_unchanged() const
{
  int ret = common::OB_SUCCESS;
  struct stat fd_stat;
  struct stat root_stat;
  struct stat path_stat;
  char resolved_root[PATH_MAX] = {0};
  if (fd_ < 0) {
    ret = common::OB_NOT_INIT;
  } else if (root_fd_ < 0 || 0 != ::fstat(root_fd_, &root_stat)
      || NULL == ::realpath(canonical_root_.c_str(), resolved_root)
      || canonical_root_ != resolved_root) {
    ret = common::OB_ERR_NO_PRIVILEGE;
  } else if (0 != ::fstat(fd_, &fd_stat)) {
    ret = common::OB_INVALID_ARGUMENT;
  } else {
    const int path_fd = open_relative_file(O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (path_fd < 0) {
      ret = (EACCES == errno || EPERM == errno || ELOOP == errno || EXDEV == errno || ENOSYS == errno)
          ? common::OB_ERR_NO_PRIVILEGE : common::OB_INVALID_ARGUMENT;
    } else if (0 != ::fstat(path_fd, &path_stat)) {
      ret = common::OB_INVALID_ARGUMENT;
    }
    if (path_fd >= 0) { ::close(path_fd); }
    ObFileFingerprint fd_fingerprint;
    ObFileFingerprint path_fingerprint;
    if (OB_SUCC(ret)) {
      fd_fingerprint.device_ = static_cast<uint64_t>(fd_stat.st_dev);
      fd_fingerprint.inode_ = static_cast<uint64_t>(fd_stat.st_ino);
      fd_fingerprint.size_ = static_cast<int64_t>(fd_stat.st_size);
      fd_fingerprint.mtime_sec_ = static_cast<int64_t>(fd_stat.st_mtim.tv_sec);
      fd_fingerprint.mtime_nsec_ = static_cast<int64_t>(fd_stat.st_mtim.tv_nsec);
      fd_fingerprint.ctime_sec_ = static_cast<int64_t>(fd_stat.st_ctim.tv_sec);
      fd_fingerprint.ctime_nsec_ = static_cast<int64_t>(fd_stat.st_ctim.tv_nsec);
      path_fingerprint.device_ = static_cast<uint64_t>(path_stat.st_dev);
      path_fingerprint.inode_ = static_cast<uint64_t>(path_stat.st_ino);
      path_fingerprint.size_ = static_cast<int64_t>(path_stat.st_size);
      path_fingerprint.mtime_sec_ = static_cast<int64_t>(path_stat.st_mtim.tv_sec);
      path_fingerprint.mtime_nsec_ = static_cast<int64_t>(path_stat.st_mtim.tv_nsec);
      path_fingerprint.ctime_sec_ = static_cast<int64_t>(path_stat.st_ctim.tv_sec);
      path_fingerprint.ctime_nsec_ = static_cast<int64_t>(path_stat.st_ctim.tv_nsec);
    }
    if (OB_SUCC(ret) && (!S_ISREG(fd_stat.st_mode) || !S_ISREG(path_stat.st_mode)
        || !(fd_fingerprint == fingerprint_) || !(path_fingerprint == fingerprint_))) {
      ret = common::OB_INVALID_ARGUMENT;
    }
  }
  if (ret == common::OB_INVALID_ARGUMENT) {
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file input (file fingerprint changed)");
  }
  return ret;
}

int ObFileInput::read(void *buffer, int64_t capacity, int64_t &read_bytes)
{
  int ret = common::OB_SUCCESS;
  read_bytes = 0;
  if (fd_ < 0 || NULL == buffer || capacity < 0) {
    ret = common::OB_INVALID_ARGUMENT;
  } else if (capacity > static_cast<int64_t>(std::numeric_limits<ssize_t>::max())) {
    ret = common::OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(verify_unchanged())) {
  } else if (capacity > 0 && read_offset_ < fingerprint_.size_) {
    const int64_t bounded_capacity = std::min(capacity, fingerprint_.size_ - read_offset_);
    ssize_t nread = 0;
    do {
      nread = ::pread(fd_, buffer, static_cast<size_t>(bounded_capacity), read_offset_);
    } while (nread < 0 && EINTR == errno);
    if (nread < 0) {
      ret = common::OB_IO_ERROR;
    } else if (nread == 0) {
      ret = common::OB_INVALID_ARGUMENT;
    } else if (OB_FAIL(verify_unchanged())) {
    } else {
      read_bytes = static_cast<int64_t>(nread);
      read_offset_ += read_bytes;
    }
  }
  return ret;
}

int ObFileInput::rescan()
{
  int ret = common::OB_SUCCESS;
  if (OB_FAIL(verify_unchanged())) {
  } else if (0 != ::lseek(fd_, 0, SEEK_SET)) {
    ret = common::OB_IO_ERROR;
  } else {
    read_offset_ = 0;
  }
  return ret;
}

void ObFileInput::close()
{
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  if (root_fd_ >= 0) {
    ::close(root_fd_);
    root_fd_ = -1;
  }
  canonical_path_.clear();
  relative_path_.clear();
  canonical_root_.clear();
  fingerprint_ = ObFileFingerprint();
  read_offset_ = 0;
}

ObCsvFileScanner::ObCsvFileScanner()
  : input_(), options_(), columns_(), fields_(), column_names_(), inferred_types_(), nullable_(),
    io_buffer_(), io_pos_(0), io_size_(0), line_(1), byte_offset_(0), record_line_(1),
    record_offset_(0), pending_byte_(0), has_pending_byte_(false), input_eof_(false), error_location_(), row_count_(0)
{
}

ObCsvFileScanner::~ObCsvFileScanner()
{
  close();
}

int ObCsvFileScanner::open(const common::ObString &secure_file_priv,
                           const common::ObString &path,
                           const ObFileFingerprint *expected_fingerprint)
{
  ObCsvScanOptions options;
  return open(secure_file_priv, path, options, expected_fingerprint);
}

int ObCsvFileScanner::open(const common::ObString &secure_file_priv,
                           const common::ObString &path,
                           const ObCsvScanOptions &options,
                           const ObFileFingerprint *expected_fingerprint)
{
  int ret = common::OB_SUCCESS;
  close();
  if (options.sample_size_ != -1) {
    ret = common::OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "sampled file schema inference");
  } else if (options.field_terminator_ == '\0' || options.field_enclosure_ == '\0'
      || options.field_terminator_ == '\r' || options.field_terminator_ == '\n'
      || options.field_enclosure_ == '\r' || options.field_enclosure_ == '\n'
      || options.field_escape_ == '\r' || options.field_escape_ == '\n'
      || options.field_terminator_ == options.field_enclosure_) {
    ret = common::OB_INVALID_ARGUMENT;
    set_error_location(1, 1, 0);
  } else if (OB_FAIL(input_.open(secure_file_priv, path, expected_fingerprint))) {
  } else {
    options_ = options;
    reset_reader();
    bool has_record = false;
    if (options_.has_header_) {
      if (OB_FAIL(read_record(has_record))) {
      } else if (has_record) {
        if (fields_.empty() || fields_.size() > static_cast<size_t>(ObFileScanLimits::MAX_COLUMNS)) {
          ret = common::OB_INVALID_ARGUMENT;
          set_error_location(record_line_, 1, record_offset_);
        } else {
          column_names_.reserve(fields_.size());
          // Reserve literal header names before choosing suffixes, otherwise
          // an early duplicate could steal a later literal "id__2" column.
          const auto key = [](std::string value) {
            for (char &ch : value) {
              if (ch >= 'A' && ch <= 'Z') { ch += 'a' - 'A'; }
            }
            return value;
          };
          std::set<std::string> reserved;
          std::set<std::string> used;
          for (const Field &field : fields_) {
            if (!field.value_.empty()) { reserved.insert(key(field.value_)); }
          }
          for (size_t i = 0; OB_SUCC(ret) && i < fields_.size(); ++i) {
            const bool generated = fields_[i].value_.empty();
            const std::string base = generated ? "column" + std::to_string(i) : fields_[i].value_;
            std::string name = base;
            int64_t suffix = 2;
            while (used.count(key(name)) != 0 || (generated && reserved.count(key(name)) != 0)
                || (name != base && reserved.count(key(name)) != 0)) {
              name = base + "__" + std::to_string(suffix++);
            }
            used.insert(key(name));
            column_names_.push_back(std::move(name));
          }
          inferred_types_.assign(fields_.size(), common::ObNullType);
          nullable_.assign(fields_.size(), 0);
        }
      }
    }
    if (OB_SUCC(ret) && !options_.has_header_) {
      if (OB_FAIL(read_record(has_record))) {
      } else if (has_record) {
        if (fields_.size() > static_cast<size_t>(ObFileScanLimits::MAX_COLUMNS)) {
          ret = common::OB_INVALID_ARGUMENT;
          set_error_location(record_line_, ObFileScanLimits::MAX_COLUMNS + 1, record_offset_);
        } else {
          column_names_.reserve(fields_.size());
          inferred_types_.assign(fields_.size(), common::ObNullType);
          nullable_.assign(fields_.size(), 0);
          for (size_t i = 0; i < fields_.size(); ++i) {
            column_names_.push_back("column" + std::to_string(i));
          }
          ret = infer_record();
          if (OB_SUCC(ret)) { ++row_count_; }
        }
      }
    }
    while (OB_SUCC(ret)) {
      if (OB_FAIL(read_record(has_record))) {
      } else if (!has_record) {
        break;
      }
      if (OB_SUCC(ret) && has_record) {
        ret = infer_record();
        if (OB_SUCC(ret)) { ++row_count_; }
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(column_names_.size()); ++i) {
      ObFileColumn column;
      column.name_.assign_ptr(const_cast<char *>(column_names_[i].c_str()),
                              static_cast<int32_t>(column_names_[i].size()));
      column.type_ = common::ObNullType == inferred_types_[i] ? common::ObVarcharType : inferred_types_[i];
      column.nullable_ = nullable_[i] != 0 || common::ObNullType == inferred_types_[i];
      if (OB_FAIL(columns_.push_back(column))) {
      }
    }
    if (OB_SUCC(ret)) {
      ret = rescan();
    }
  }
  if (OB_FAIL(ret)) {
    input_.close();
  }
  return ret;
}

int ObCsvFileScanner::next_row(common::ObIArray<common::ObObj> &row, bool &has_row)
{
  int ret = common::OB_SUCCESS;
  has_row = false;
  row.reset();
  if (OB_FAIL(input_.validate_unchanged())) {
    set_error_location(line_, 1, byte_offset_);
  } else if (OB_FAIL(read_record(has_row))) {
  } else if (OB_FAIL(input_.validate_unchanged())) {
    has_row = false;
    set_error_location(record_line_, 1, record_offset_);
  } else if (has_row && fields_.size() != inferred_types_.size()) {
    const size_t index = std::min(fields_.size(), inferred_types_.size());
    set_error_location(index < fields_.size() ? fields_[index].line_ : line_, index + 1,
        index < fields_.size() ? fields_[index].byte_offset_ : byte_offset_);
    ret = common::OB_INVALID_ARGUMENT;
  } else {
    const int64_t output_count = has_projection_ ? projection_.count() : fields_.size();
    for (int64_t i = 0; OB_SUCC(ret) && has_row && i < output_count; ++i) {
      const int64_t column_idx = has_projection_ ? projection_.at(i) : i;
      common::ObObj obj;
      if (OB_FAIL(convert_field(column_idx, obj))) {
        set_error_location(fields_[column_idx].line_, column_idx + 1, fields_[column_idx].byte_offset_);
      } else if (OB_FAIL(row.push_back(obj))) {
      }
    }
  }
  if (OB_FAIL(ret)) { has_row = false; }
  return ret;
}

int ObCsvFileScanner::rescan()
{
  int ret = input_.rescan();
  if (ret == common::OB_INVALID_ARGUMENT) { set_error_location(1, 1, 0); }
  if (OB_SUCC(ret)) {
    reset_reader();
    if (options_.has_header_) {
      bool has_header = false;
      if (OB_FAIL(read_record(has_header))) {
      } else if (column_names_.empty() && has_header) {
        ret = common::OB_INVALID_ARGUMENT;
      } else if (!column_names_.empty() && !has_header) {
        ret = common::OB_INVALID_ARGUMENT;
      }
    }
  }
  return ret;
}

void ObCsvFileScanner::close()
{
  reset_projection();
  row_count_ = 0;
  input_.close();
  columns_.reuse();
  fields_.clear();
  column_names_.clear();
  inferred_types_.clear();
  nullable_.clear();
  options_ = ObCsvScanOptions();
  reset_reader();
  error_location_ = ObFileErrorLocation();
}

int ObCsvFileScanner::next_byte(char &ch, bool &has_byte)
{
  int ret = common::OB_SUCCESS;
  has_byte = false;
  if (has_pending_byte_) {
    ch = pending_byte_;
    has_pending_byte_ = false;
    ++byte_offset_;
    has_byte = true;
  } else if (input_eof_) {
  } else {
    if (io_pos_ >= io_size_) {
      int64_t read_size = 0;
      if (OB_FAIL(input_.read(io_buffer_, sizeof(io_buffer_), read_size))) {
      } else if (read_size == 0) {
        input_eof_ = true;
      } else {
        io_pos_ = 0;
        io_size_ = read_size;
        if (byte_offset_ == 0 && io_size_ >= 3
            && static_cast<unsigned char>(io_buffer_[0]) == 0xef
            && static_cast<unsigned char>(io_buffer_[1]) == 0xbb
            && static_cast<unsigned char>(io_buffer_[2]) == 0xbf) {
          io_pos_ = 3;
          byte_offset_ = 3;
          if (io_pos_ == io_size_) { return next_byte(ch, has_byte); }
        }
      }
    }
    if (OB_SUCC(ret) && !input_eof_) {
      ch = io_buffer_[io_pos_++];
      ++byte_offset_;
      has_byte = true;
    }
  }
  return ret;
}

void ObCsvFileScanner::unread_byte(char ch)
{
  if (!has_pending_byte_) {
    pending_byte_ = ch;
    has_pending_byte_ = true;
    --byte_offset_;
  }
}

void ObCsvFileScanner::reset_reader()
{
  io_pos_ = 0;
  io_size_ = 0;
  line_ = 1;
  byte_offset_ = 0;
  record_line_ = 1;
  record_offset_ = 0;
  pending_byte_ = 0;
  has_pending_byte_ = false;
  input_eof_ = false;
}

void ObCsvFileScanner::set_error_location(int64_t line, int64_t column, int64_t byte_offset)
{
  error_location_.line_ = line;
  error_location_.column_ = column;
  error_location_.byte_offset_ = byte_offset;
  char detail[160] = {0};
  snprintf(detail, sizeof(detail), "CSV at physical line %lld, column %lld, byte %lld",
      static_cast<long long>(line), static_cast<long long>(column), static_cast<long long>(byte_offset));
  LOG_USER_ERROR(OB_INVALID_ARGUMENT, detail);
}

bool ObCsvFileScanner::is_null_field(const Field &field) const
{
  return !field.quoted_ && ((options_.empty_as_null_ && field.value_.empty())
      || (!options_.null_string_.empty() && field.value_ == options_.null_string_));
}

int ObCsvFileScanner::read_record(bool &has_record)
{
  int ret = common::OB_SUCCESS;
  has_record = false;
  fields_.clear();
  enum State { FIELD_START, UNQUOTED, QUOTED, CLOSED_QUOTE };
  State state = FIELD_START;
  std::string value;
  bool quoted = false;
  bool saw_byte = false;
  int64_t record_size = 0;
  int64_t field_line = line_;
  int64_t field_offset = byte_offset_;
  int utf8_remaining = 0;
  unsigned char utf8_min = 0x80;
  unsigned char utf8_max = 0xbf;
  int64_t utf8_line = line_;
  int64_t utf8_offset = byte_offset_;
  record_line_ = line_;
  record_offset_ = byte_offset_;

  const auto fail_here = [&]() {
    set_error_location(line_, static_cast<int64_t>(fields_.size() + 1),
        std::max<int64_t>(0, byte_offset_ - 1));
    return common::OB_INVALID_ARGUMENT;
  };
  const auto consume = [&](char &ch, bool &has_byte) {
    int status = next_byte(ch, has_byte);
    if (status == common::OB_SUCCESS && has_byte) {
      if (!saw_byte && record_offset_ == 0 && byte_offset_ > 1) {
        record_offset_ = byte_offset_ - 1; // Exclude an initial UTF-8 BOM.
        field_offset = record_offset_;
      }
      saw_byte = true;
      if (++record_size > ObFileScanLimits::MAX_RECORD_BYTES) { status = fail_here(); }
    }
    return status;
  };
  // Validate decoded bytes while their physical source position is available.
  // This remains exact after quote doubling, escaping and embedded newlines.
  const auto append = [&](char ch) {
    const unsigned char byte = static_cast<unsigned char>(ch);
    bool valid = true;
    if (utf8_remaining > 0) {
      valid = byte >= utf8_min && byte <= utf8_max;
      --utf8_remaining;
      utf8_min = 0x80;
      utf8_max = 0xbf;
    } else if (byte > 0x7f) {
      utf8_line = line_;
      utf8_offset = byte_offset_ - 1;
      utf8_min = 0x80;
      utf8_max = 0xbf;
      if (byte >= 0xc2 && byte <= 0xdf) {
        utf8_remaining = 1;
      } else if (byte >= 0xe0 && byte <= 0xef) {
        utf8_remaining = 2;
        if (byte == 0xe0) { utf8_min = 0xa0; }
        if (byte == 0xed) { utf8_max = 0x9f; }
      } else if (byte >= 0xf0 && byte <= 0xf4) {
        utf8_remaining = 3;
        if (byte == 0xf0) { utf8_min = 0x90; }
        if (byte == 0xf4) { utf8_max = 0x8f; }
      } else {
        valid = false;
      }
    }
    if (!valid) {
      set_error_location(line_, fields_.size() + 1, byte_offset_ - 1);
    } else {
      value.push_back(ch);
    }
    return valid ? common::OB_SUCCESS : common::OB_INVALID_ARGUMENT;
  };
  const auto finish_field = [&]() {
    int status = common::OB_SUCCESS;
    if (fields_.size() >= static_cast<size_t>(ObFileScanLimits::MAX_COLUMNS)) {
      status = fail_here();
    } else if (utf8_remaining != 0) {
      set_error_location(utf8_line, static_cast<int64_t>(fields_.size() + 1), utf8_offset);
      status = common::OB_INVALID_ARGUMENT;
    } else {
      Field field;
      field.value_.swap(value);
      field.quoted_ = quoted;
      field.line_ = field_line;
      field.byte_offset_ = field_offset;
      fields_.push_back(std::move(field));
      quoted = false;
      state = FIELD_START;
      field_line = line_;
      field_offset = byte_offset_;
    }
    return status;
  };
  while (OB_SUCC(ret) && !has_record) {
    char ch = 0;
    bool has_byte = false;
    if (OB_FAIL(consume(ch, has_byte))) {
      if (error_location_.line_ == 0) { set_error_location(line_, fields_.size() + 1, byte_offset_); }
    } else if (!has_byte) {
      if (state == QUOTED) {
        set_error_location(line_, fields_.size() + 1, byte_offset_);
        ret = common::OB_INVALID_ARGUMENT;
      } else if (saw_byte) {
        if (OB_SUCC(finish_field())) { has_record = true; }
        else { ret = common::OB_INVALID_ARGUMENT; }
      }
      break;
    } else if (state == QUOTED) {
      if (options_.field_escape_ != '\0' && options_.field_escape_ != options_.field_enclosure_
          && ch == options_.field_escape_) {
        char escaped = 0;
        bool has_escaped = false;
        if (OB_FAIL(consume(escaped, has_escaped))) {
        } else if (!has_escaped) {
          ret = fail_here();
        } else {
          ret = append(escaped);
          if (escaped == '\n') { ++line_; }
        }
      } else if (ch == options_.field_enclosure_) {
        state = CLOSED_QUOTE;
      } else {
        ret = append(ch);
        if (ch == '\n') { ++line_; }
      }
    } else if (state == CLOSED_QUOTE && ch == options_.field_enclosure_) {
      ret = append(ch);
      state = QUOTED;
    } else if (ch == options_.field_terminator_) {
      ret = finish_field();
      // A separator always starts another field, even immediately before EOF.
      if (OB_SUCC(ret) && fields_.size() == static_cast<size_t>(ObFileScanLimits::MAX_COLUMNS)) {
        ret = fail_here();
      }
    } else if (ch == '\n' || ch == '\r') {
      if (ch == '\r') {
        char following = 0;
        bool has_following = false;
        if (OB_FAIL(consume(following, has_following))) {
        } else if (has_following && following != '\n') {
          // Standalone CR is a record boundary too; do not lose the first
          // byte of the next record while checking for CRLF.
          unread_byte(following);
          --record_size;
        }
      }
      if (OB_SUCC(ret)) {
        ret = finish_field();
        if (OB_SUCC(ret)) { ++line_; has_record = true; }
      }
    } else if (state == CLOSED_QUOTE) {
      ret = fail_here();
    } else if (ch == options_.field_enclosure_) {
      if (state != FIELD_START) {
        ret = fail_here();
      } else {
        quoted = true;
        state = QUOTED;
        field_offset = byte_offset_;
      }
    } else {
      ret = append(ch);
      state = UNQUOTED;
    }
    if (OB_SUCC(ret) && value.size() > static_cast<size_t>(ObFileScanLimits::MAX_FIELD_BYTES)) {
      ret = fail_here();
    }
  }
  if (OB_FAIL(ret)) { has_record = false; }
  return ret;
}

int ObCsvFileScanner::infer_record()
{
  int ret = common::OB_SUCCESS;
  if (fields_.size() != inferred_types_.size()) {
    const size_t index = std::min(fields_.size(), inferred_types_.size());
    set_error_location(index < fields_.size() ? fields_[index].line_ : line_, index + 1,
        index < fields_.size() ? fields_[index].byte_offset_ : byte_offset_);
    ret = common::OB_INVALID_ARGUMENT;
  } else {
    for (size_t i = 0; OB_SUCC(ret) && i < fields_.size(); ++i) {
      if (is_null_field(fields_[i])) {
        nullable_[i] = 1;
      } else {
        inferred_types_[i] = file_sql_merge_types(inferred_types_[i], file_sql_infer_scalar(fields_[i].value_));
      }
    }
  }
  return ret;
}

int ObCsvFileScanner::convert_field(int64_t column_idx, common::ObObj &obj) const
{
  int ret = common::OB_SUCCESS;
  if (column_idx < 0 || column_idx >= static_cast<int64_t>(fields_.size())
      || column_idx >= static_cast<int64_t>(inferred_types_.size())) {
    ret = common::OB_INVALID_ARGUMENT;
  } else if (is_null_field(fields_[column_idx])) {
    obj.set_null();
  } else {
    ret = file_sql_convert_scalar(fields_[column_idx].value_, inferred_types_[column_idx], obj);
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
