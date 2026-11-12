/**
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
 */
#include "sql/engine/basic/ob_glob_file_scanner.h"
#include "common/object/ob_object.h"
#include "lib/ob_errno.h"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <unistd.h>

namespace oceanbase
{
namespace sql
{
using namespace common;

int GlobFileScanner::open(const ObString &secure_root, const ObString &pattern,
                          const ObFileFingerprint *expected)
{
  UNUSED(expected);
  int ret = OB_SUCCESS;
  close();
  DIR *directory = nullptr;
  if (secure_root.empty() || secure_root.length() >= PATH_MAX
      || memchr(secure_root.ptr(), '\0', secure_root.length())) {
    ret = OB_ERR_NO_PRIVILEGE;
  } else if (pattern.empty() || pattern.length() >= PATH_MAX
             || memchr(pattern.ptr(), '\0', pattern.length())) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    secure_root_.assign(secure_root.ptr(), secure_root.length());
    const std::string input(pattern.ptr(), pattern.length());
    const size_t slash = input.find_last_of('/');
    const std::string parent = slash == std::string::npos ? "." : slash == 0 ? "/" : input.substr(0, slash);
    const std::string mask = slash == std::string::npos ? input : input.substr(slash + 1);
    char root_path[PATH_MAX];
    char parent_path[PATH_MAX];
    struct stat root_stat;
    if (strcasecmp(secure_root_.c_str(), "NULL") == 0
        || ::realpath(secure_root_.c_str(), root_path) == nullptr
        || ::stat(root_path, &root_stat) != 0 || !S_ISDIR(root_stat.st_mode)) {
      ret = OB_ERR_NO_PRIVILEGE;
    } else if (::realpath(parent.c_str(), parent_path) == nullptr) {
      ret = errno == ENOENT || errno == ENOTDIR ? OB_FILE_NOT_EXIST : OB_IO_ERROR;
    } else {
      const size_t root_length = strlen(root_path);
      const size_t parent_length = strlen(parent_path);
      const bool within = strcmp(root_path, "/") == 0 || strcmp(root_path, parent_path) == 0
          || (parent_length > root_length && strncmp(root_path, parent_path, root_length) == 0
              && parent_path[root_length] == '/');
      if (!within) {
        ret = OB_ERR_NO_PRIVILEGE;
      } else if (mask.empty()) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        const int fd = ::open(parent_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
          ret = OB_IO_ERROR;
        } else if ((directory = ::fdopendir(fd)) == nullptr) {
          ::close(fd);
          ret = OB_IO_ERROR;
        }
      }
      while (OB_SUCC(ret)) {
        errno = 0;
        const dirent *entry = ::readdir(directory);
        if (entry == nullptr) {
          if (errno != 0) { ret = OB_IO_ERROR; }
          break;
        } else if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0
                   || fnmatch(mask.c_str(), entry->d_name, FNM_PERIOD) != 0) {
          continue;
        }
        const std::string candidate = std::string(parent_path) + "/" + entry->d_name;
        struct stat file_stat;
        if (::stat(candidate.c_str(), &file_stat) != 0 || !S_ISREG(file_stat.st_mode)) {
          continue;
        } else if (paths_.size() >= static_cast<size_t>(ObFileScanLimits::MAX_GLOB_MATCHES)) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          ObFileInput file;
          if (OB_FAIL(file.open(secure_root, ObString(candidate.size(), candidate.data())))) {
          } else {
            paths_.push_back(candidate);
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      std::sort(paths_.begin(), paths_.end());
      ObFileColumn column;
      column.name_ = ObString::make_string("file");
      column.type_ = ObVarcharType;
      ret = columns_.push_back(column);
    }
  }
  if (directory != nullptr) { ::closedir(directory); }
  if (OB_FAIL(ret)) { close(); }
  return ret;
}

int GlobFileScanner::next_row(ObIArray<ObObj> &row, bool &has_row)
{
  int ret = OB_SUCCESS;
  row.reuse();
  has_row = false;
  if (position_ < static_cast<int64_t>(paths_.size())) {
    const std::string &path = paths_[position_];
    ObFileInput input;
    if (OB_FAIL(input.open(ObString(secure_root_.size(), secure_root_.data()),
                           ObString(path.size(), path.data())))) {
    } else {
      ObObj value;
      value.set_varchar(ObString(path.size(), path.data()));
      if ((!has_projection_ || projection_.count() != 0) && OB_FAIL(row.push_back(value))) {
      } else {
        ++position_;
        has_row = true;
      }
    }
  }
  return ret;
}

void GlobFileScanner::close()
{
  reset_projection();
  secure_root_.clear();
  paths_.clear();
  columns_.reuse();
  position_ = 0;
}
}
}
