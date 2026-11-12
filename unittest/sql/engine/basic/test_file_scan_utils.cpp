/**
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#include <fcntl.h>
#include <cstring>
#include <limits.h>
#include <cstdlib>
#include <fstream>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include "gtest/gtest.h"
#include "lib/ob_errno.h"
#include "share/ob_errno.h"
#include "sql/engine/basic/ob_file_scan_utils.h"
#include "lib/container/ob_se_array.h"
#include "common/object/ob_object.h"

using namespace oceanbase;
using namespace oceanbase::sql;

namespace
{

static int write_file(const std::string &path, const char *contents)
{
  int ret = 0;
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    ret = -1;
  } else {
    const size_t length = strlen(contents);
    if (::write(fd, contents, length) != static_cast<ssize_t>(length)) {
      ret = -1;
    }
    ::close(fd);
  }
  return ret;
}

class TestFileScanUtils : public ::testing::Test
{
public:
  void SetUp() override
  {
    char root_template[] = "/tmp/file_sql_test_XXXXXX";
    char *root = ::mkdtemp(root_template);
    ASSERT_NE(nullptr, root);
    root_ = root;
    secure_root_ = root_ + "/safe";
    outside_root_ = root_ + "/safe_evil";
    ASSERT_EQ(0, ::mkdir(secure_root_.c_str(), 0700));
    ASSERT_EQ(0, ::mkdir(outside_root_.c_str(), 0700));
    file_ = secure_root_ + "/data.csv";
    outside_file_ = outside_root_ + "/data.csv";
    link_ = secure_root_ + "/escape.csv";
    ASSERT_EQ(0, write_file(file_, "a\n"));
    ASSERT_EQ(0, write_file(outside_file_, "b\n"));
    ASSERT_EQ(0, ::symlink(outside_file_.c_str(), link_.c_str()));
  }

  void TearDown() override
  {
    ::unlink(link_.c_str());
    ::unlink(file_.c_str());
    ::unlink(outside_file_.c_str());
    ::rmdir(secure_root_.c_str());
    ::rmdir(outside_root_.c_str());
    ::rmdir(root_.c_str());
  }

  common::ObString ob_string(const std::string &value) const
  {
    return common::ObString(static_cast<int32_t>(value.size()), value.c_str());
  }

  std::string root_;
  std::string secure_root_;
  std::string outside_root_;
  std::string file_;
  std::string outside_file_;
  std::string link_;
};

TEST_F(TestFileScanUtils, rejects_prefix_collision)
{
  ObFileInput input;
  EXPECT_EQ(common::OB_ERR_NO_PRIVILEGE, input.open(ob_string(secure_root_), ob_string(outside_file_)));
}

TEST_F(TestFileScanUtils, rejects_directory_input)
{
  ObFileInput input;
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, input.open(ob_string(secure_root_), ob_string(secure_root_)));
}

TEST_F(TestFileScanUtils, rejects_symlink_escape)
{
  ObFileInput input;
  EXPECT_EQ(common::OB_ERR_NO_PRIVILEGE, input.open(ob_string(secure_root_), ob_string(link_)));
}

TEST_F(TestFileScanUtils, rejects_intermediate_symlink_swap_after_boundary_check)
{
  const std::string child_dir = secure_root_ + "/child";
  const std::string moved_dir = secure_root_ + "/held_child";
  const std::string external_dir = root_ + "/external";
  ASSERT_EQ(0, ::mkdir(child_dir.c_str(), 0700));
  ASSERT_EQ(0, ::mkdir(external_dir.c_str(), 0700));
  const std::string target_file = child_dir + "/data.jsonl";
  const std::string outside_file = external_dir + "/data.jsonl";
  ASSERT_EQ(0, write_file(target_file, "inside\n"));
  ASSERT_EQ(0, write_file(outside_file, "outside-secret\n"));

  char marker_template[] = "/tmp/file_sql_race_hook_XXXXXX";
  const int marker_fd = ::mkstemp(marker_template);
  ASSERT_GE(marker_fd, 0);
  ASSERT_EQ(0, ::close(marker_fd));
  ASSERT_EQ(0, ::unlink(marker_template));
  const std::string marker(marker_template);
  const char *env_names[] = {"FILE_SQL_RACE_TARGET", "FILE_SQL_RACE_FROM", "FILE_SQL_RACE_TO",
                             "FILE_SQL_RACE_SYMLINK_TO", "FILE_SQL_RACE_MARKER"};
  const char *env_values[] = {target_file.c_str(), child_dir.c_str(), moved_dir.c_str(),
                              external_dir.c_str(), marker.c_str()};
  std::vector<std::string> previous_values;
  std::vector<bool> previously_set;
  for (size_t i = 0; i < sizeof(env_names) / sizeof(env_names[0]); ++i) {
    const char *previous = ::getenv(env_names[i]);
    previously_set.push_back(previous != nullptr);
    previous_values.push_back(previous == nullptr ? "" : previous);
    ASSERT_EQ(0, ::setenv(env_names[i], env_values[i], 1));
  }

  ObFileInput input;
  const int open_ret = input.open(ob_string(secure_root_), ob_string(target_file));
  char buffer[64] = {0};
  int64_t read_bytes = 0;
  const int read_ret = input.read(buffer, sizeof(buffer), read_bytes);
  input.close();
  for (size_t i = 0; i < sizeof(env_names) / sizeof(env_names[0]); ++i) {
    if (previously_set[i]) {
      ASSERT_EQ(0, ::setenv(env_names[i], previous_values[i].c_str(), 1));
    } else {
      ASSERT_EQ(0, ::unsetenv(env_names[i]));
    }
  }

  std::ifstream hook_log(marker);
  std::string hook_result;
  std::getline(hook_log, hook_result);
  EXPECT_EQ("fired", hook_result);
  EXPECT_EQ(common::OB_ERR_NO_PRIVILEGE, open_ret);
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, read_ret);
  EXPECT_EQ(0, read_bytes);
  EXPECT_EQ(-1, input.get_fd());

  ASSERT_EQ(0, ::unlink((child_dir).c_str()));
  ASSERT_EQ(0, ::rename(moved_dir.c_str(), child_dir.c_str()));
  ASSERT_EQ(0, ::unlink(target_file.c_str()));
  ASSERT_EQ(0, ::unlink(outside_file.c_str()));
  ASSERT_EQ(0, ::rmdir(child_dir.c_str()));
  ASSERT_EQ(0, ::rmdir(external_dir.c_str()));
  ASSERT_EQ(0, ::unlink(marker.c_str()));
}

TEST_F(TestFileScanUtils, detects_mutation_before_rescan)
{
  ObFileInput input;
  ASSERT_EQ(common::OB_SUCCESS, input.open(ob_string(secure_root_), ob_string(file_)));
  ASSERT_EQ(0, write_file(file_, "changed\n"));
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, input.rescan());
}

TEST_F(TestFileScanUtils, rejects_mutation_after_schema_inference)
{
  ObFileInput schema_input;
  ASSERT_EQ(common::OB_SUCCESS, schema_input.open(ob_string(secure_root_), ob_string(file_)));
  const ObFileFingerprint inferred_fingerprint = schema_input.get_fingerprint();
  schema_input.close();
  ASSERT_EQ(0, write_file(file_, "different size\n"));

  ObFileInput scan_input;
  EXPECT_EQ(common::OB_INVALID_ARGUMENT,
            scan_input.open(ob_string(secure_root_), ob_string(file_), &inferred_fingerprint));
}

TEST_F(TestFileScanUtils, reads_sequentially_and_rescans)
{
  ObFileInput input;
  ASSERT_EQ(common::OB_SUCCESS, input.open(ob_string(secure_root_), ob_string(file_)));
  char buffer[8] = {0};
  int64_t read_bytes = 0;
  ASSERT_EQ(common::OB_SUCCESS, input.read(buffer, sizeof(buffer), read_bytes));
  ASSERT_EQ(2, read_bytes);
  EXPECT_EQ(0, std::memcmp(buffer, "a\n", 2));
  ASSERT_EQ(common::OB_SUCCESS, input.rescan());
  MEMSET(buffer, 0, sizeof(buffer));
  ASSERT_EQ(common::OB_SUCCESS, input.read(buffer, sizeof(buffer), read_bytes));
  ASSERT_EQ(2, read_bytes);
  EXPECT_EQ(0, std::memcmp(buffer, "a\n", 2));
}

TEST_F(TestFileScanUtils, infers_schema_and_reads_quoted_rows)
{
  ASSERT_EQ(0, write_file(file_,
      "id,name,score,note\r\n1,\"Alice, A\",1.5,\"line1\r\nline2 \"\"quoted\"\"\"\r\n2,Bob,2,\r\n"));
  ObCsvScanOptions options;
  options.has_header_ = true;
  ObCsvFileScanner scanner;
  ASSERT_EQ(common::OB_SUCCESS, scanner.open(ob_string(secure_root_), ob_string(file_), options));

  const common::ObIArray<ObFileColumn> &columns = scanner.get_columns();
  ASSERT_EQ(4, columns.count());
  EXPECT_EQ(common::ObIntType, columns.at(0).type_);
  EXPECT_EQ(common::ObDoubleType, columns.at(2).type_);
  EXPECT_STREQ("name", columns.at(1).name_.ptr());

  common::ObSEArray<common::ObObj, 4> row;
  bool has_row = false;
  ASSERT_EQ(common::OB_SUCCESS, scanner.next_row(row, has_row));
  ASSERT_TRUE(has_row);
  ASSERT_EQ(4, row.count());
  EXPECT_EQ(1, row.at(0).get_int());
  EXPECT_STREQ("Alice, A", row.at(1).get_varchar().ptr());
  EXPECT_DOUBLE_EQ(1.5, row.at(2).get_double());
  EXPECT_STREQ("line1\r\nline2 \"quoted\"", row.at(3).get_varchar().ptr());

  ASSERT_EQ(common::OB_SUCCESS, scanner.next_row(row, has_row));
  ASSERT_TRUE(has_row);
  EXPECT_TRUE(row.at(3).is_null());
  ASSERT_EQ(common::OB_SUCCESS, scanner.next_row(row, has_row));
  EXPECT_FALSE(has_row);

  ASSERT_EQ(common::OB_SUCCESS, scanner.rescan());
  ASSERT_EQ(common::OB_SUCCESS, scanner.next_row(row, has_row));
  EXPECT_TRUE(has_row);
  EXPECT_EQ(1, row.at(0).get_int());
}

TEST_F(TestFileScanUtils, reports_bad_record_location)
{
  ASSERT_EQ(0, write_file(file_, "id,name\n1,A\n2,B,C\n"));
  ObCsvScanOptions options;
  options.has_header_ = true;
  ObCsvFileScanner scanner;
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, scanner.open(ob_string(secure_root_), ob_string(file_), options));
  EXPECT_EQ(3, scanner.get_error_location().line_);
  EXPECT_EQ(3, scanner.get_error_location().column_);
}

TEST_F(TestFileScanUtils, infers_boolean_and_datetime_without_header)
{
  ASSERT_EQ(0, write_file(file_, "true,2026-02-28\nfalse,2026-02-28 12:30:45\n"));
  ObCsvScanOptions options;
  options.has_header_ = false;
  ObCsvFileScanner scanner;
  ASSERT_EQ(common::OB_SUCCESS, scanner.open(ob_string(secure_root_), ob_string(file_), options));
  ASSERT_EQ(2, scanner.get_columns().count());
  EXPECT_EQ(common::ObTinyIntType, scanner.get_columns().at(0).type_);
  EXPECT_EQ(common::ObMySQLDateTimeType, scanner.get_columns().at(1).type_);

  common::ObSEArray<common::ObObj, 4> row;
  bool has_row = false;
  ASSERT_EQ(common::OB_SUCCESS, scanner.next_row(row, has_row));
  ASSERT_TRUE(has_row);
  EXPECT_EQ(1, row.at(0).get_tinyint());
  EXPECT_EQ(common::ObMySQLDateTimeType, row.at(1).get_type());
}

TEST_F(TestFileScanUtils, detects_mutation_between_buffered_rows)
{
  ASSERT_EQ(0, write_file(file_, "id\n1\n2\n3\n"));
  ObCsvScanOptions options;
  ObCsvFileScanner scanner;
  ASSERT_EQ(common::OB_SUCCESS, scanner.open(ob_string(secure_root_), ob_string(file_), options));
  common::ObSEArray<common::ObObj, 2> row;
  bool has_row = false;
  ASSERT_EQ(common::OB_SUCCESS, scanner.next_row(row, has_row));
  ASSERT_TRUE(has_row);
  ASSERT_EQ(0, write_file(file_, "id\n9\n"));
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, scanner.next_row(row, has_row));
}

TEST_F(TestFileScanUtils, rejects_sampled_schema_inference)
{
  ObCsvScanOptions options;
  options.sample_size_ = 100;
  ObCsvFileScanner scanner;
  EXPECT_EQ(common::OB_NOT_SUPPORTED, scanner.open(ob_string(secure_root_), ob_string(file_), options));
}

TEST_F(TestFileScanUtils, widens_types_and_respects_null_options)
{
  ASSERT_EQ(0, write_file(file_, "id;amount;active;empty\n1;2;true;\n2;2.5;false;\n3;NULL;TRUE;\n"));
  ObCsvScanOptions options;
  options.field_terminator_ = ';';
  options.null_string_ = "NULL";
  ObCsvFileScanner scanner;
  ASSERT_EQ(common::OB_SUCCESS, scanner.open(ob_string(secure_root_), ob_string(file_), options));
  ASSERT_EQ(4, scanner.get_columns().count());
  EXPECT_EQ(common::ObDoubleType, scanner.get_columns().at(1).type_);
  EXPECT_EQ(common::ObTinyIntType, scanner.get_columns().at(2).type_);
  EXPECT_EQ(common::ObVarcharType, scanner.get_columns().at(3).type_);
  EXPECT_TRUE(scanner.get_columns().at(3).nullable_);

  common::ObSEArray<common::ObObj, 4> row;
  bool has_row = false;
  ASSERT_EQ(common::OB_SUCCESS, scanner.next_row(row, has_row));
  ASSERT_TRUE(has_row);
  ASSERT_EQ(common::OB_SUCCESS, scanner.next_row(row, has_row));
  ASSERT_TRUE(has_row);
  EXPECT_EQ(2.5, row.at(1).get_double());
  ASSERT_EQ(common::OB_SUCCESS, scanner.next_row(row, has_row));
  ASSERT_TRUE(has_row);
  EXPECT_TRUE(row.at(1).is_null());
  EXPECT_TRUE(row.at(3).is_null());
}

TEST_F(TestFileScanUtils, marks_header_only_columns_nullable)
{
  ASSERT_EQ(0, write_file(file_, "id,name\n"));
  ObCsvFileScanner scanner;
  ASSERT_EQ(common::OB_SUCCESS, scanner.open(ob_string(secure_root_), ob_string(file_)));
  ASSERT_EQ(2, scanner.get_columns().count());
  EXPECT_EQ(common::ObVarcharType, scanner.get_columns().at(0).type_);
  EXPECT_TRUE(scanner.get_columns().at(0).nullable_);
  EXPECT_TRUE(scanner.get_columns().at(1).nullable_);
}

} // namespace
