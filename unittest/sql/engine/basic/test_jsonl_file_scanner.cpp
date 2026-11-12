/** Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include "gtest/gtest.h"
#include "common/object/ob_object.h"
#include "sql/engine/basic/ob_jsonl_file_scanner.h"

using namespace oceanbase;
using namespace oceanbase::sql;

namespace
{
class JsonlScanner : public ::testing::Test
{
public:
  void SetUp() override
  {
    char pattern[] = "/tmp/file_sql_jsonl_XXXXXX";
    char *directory = ::mkdtemp(pattern);
    ASSERT_NE(nullptr, directory);
    root_ = directory;
    path_ = root_ + "/data.jsonl";
  }
  void TearDown() override
  {
    ::unlink(path_.c_str());
    ::rmdir(root_.c_str());
  }
  void write(const std::string &contents)
  {
    const int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ASSERT_GE(fd, 0);
    size_t position = 0;
    while (position < contents.size()) {
      const ssize_t size = ::write(fd, contents.data() + position, contents.size() - position);
      ASSERT_GT(size, 0);
      position += size;
    }
    ASSERT_EQ(0, ::close(fd));
  }
  common::ObString str(const std::string &s) const { return common::ObString(s.size(), s.data()); }
  int open(JsonlFileScanner &scan) { return scan.open(str(root_), str(path_)); }
  std::string varchar(const common::ObObj &obj)
  {
    const common::ObString s = obj.get_varchar();
    return std::string(s.ptr(), s.length());
  }
  static int descriptor_count()
  {
    DIR *directory = ::opendir("/proc/self/fd");
    if (directory == nullptr) { return -1; }
    int count = 0;
    while (::readdir(directory) != nullptr) { ++count; }
    ::closedir(directory);
    return count;
  }
  std::string root_, path_;
};

TEST_F(JsonlScanner, first_seen_union_missing_null_and_native_types)
{
  write("\xef\xbb\xbf{\"b\":1,\"s\":\"001\",\"active\":true,\"n\":null}\r\n\r\n"
        "{\"a\":\"ok\",\"b\":1.5,\"active\":false}\r\n{}\r\n");
  JsonlFileScanner scan;
  ASSERT_EQ(common::OB_SUCCESS, open(scan));
  ASSERT_EQ(5, scan.get_columns().count());
  EXPECT_EQ(3, scan.get_row_count());
  EXPECT_EQ(common::ObDoubleType, scan.get_columns().at(0).type_);
  EXPECT_EQ(common::ObVarcharType, scan.get_columns().at(1).type_);
  EXPECT_EQ(common::ObTinyIntType, scan.get_columns().at(2).type_);
  EXPECT_EQ(common::ObVarcharType, scan.get_columns().at(3).type_);
  EXPECT_EQ("a", std::string(scan.get_columns().at(4).name_.ptr(), scan.get_columns().at(4).name_.length()));
  for (int i = 0; i < 5; ++i) { EXPECT_TRUE(scan.get_columns().at(i).nullable_); }
  common::ObSEArray<common::ObObj, 8> row;
  bool has_row = false;
  ASSERT_EQ(common::OB_SUCCESS, scan.next_row(row, has_row));
  ASSERT_TRUE(has_row);
  EXPECT_DOUBLE_EQ(1.0, row.at(0).get_double());
  EXPECT_EQ("001", varchar(row.at(1)));
  EXPECT_EQ(1, row.at(2).get_tinyint());
  EXPECT_TRUE(row.at(3).is_null());
  EXPECT_TRUE(row.at(4).is_null());
  ASSERT_EQ(common::OB_SUCCESS, scan.next_row(row, has_row));
  ASSERT_TRUE(has_row);
  EXPECT_EQ("ok", varchar(row.at(4)));
  ASSERT_EQ(common::OB_SUCCESS, scan.next_row(row, has_row));
  ASSERT_TRUE(has_row);
  for (int i = 0; i < row.count(); ++i) { EXPECT_TRUE(row.at(i).is_null()); }
  ASSERT_EQ(common::OB_SUCCESS, scan.next_row(row, has_row));
  EXPECT_FALSE(has_row);
}

TEST_F(JsonlScanner, string_number_conflict_preserves_original_values)
{
  write("{\"a\":1}\n{\"a\":\"001\"}\n");
  JsonlFileScanner scan;
  ASSERT_EQ(common::OB_SUCCESS, open(scan));
  EXPECT_EQ(common::ObVarcharType, scan.get_columns().at(0).type_);
  common::ObSEArray<common::ObObj, 2> row;
  bool has_row = false;
  ASSERT_EQ(common::OB_SUCCESS, scan.next_row(row, has_row));
  EXPECT_EQ("1", varchar(row.at(0)));
  ASSERT_EQ(common::OB_SUCCESS, scan.next_row(row, has_row));
  EXPECT_EQ("001", varchar(row.at(0)));
}

TEST_F(JsonlScanner, decodes_unicode_surrogates_and_embedded_null)
{
  write("{\"s\":\"\\u4e2d\\ud83d\\ude00\\u0000\\n\"}\n");
  JsonlFileScanner scan;
  ASSERT_EQ(common::OB_SUCCESS, open(scan));
  common::ObSEArray<common::ObObj, 2> row;
  bool has_row = false;
  ASSERT_EQ(common::OB_SUCCESS, scan.next_row(row, has_row));
  EXPECT_EQ(std::string("\xe4\xb8\xad\xf0\x9f\x98\x80\0\n", 9), varchar(row.at(0)));
}

TEST_F(JsonlScanner, projection_order_empty_projection_and_rescan)
{
  write("{\"id\":1,\"s\":\"first\"}\n{\"id\":2,\"s\":\"second\"}\n");
  JsonlFileScanner scan;
  ASSERT_EQ(common::OB_SUCCESS, open(scan));
  const ObFileFingerprint fingerprint = scan.get_fingerprint();
  common::ObSEArray<int64_t, 2> projection;
  ASSERT_EQ(common::OB_SUCCESS, projection.push_back(1));
  ASSERT_EQ(common::OB_SUCCESS, projection.push_back(0));
  ASSERT_EQ(common::OB_SUCCESS, scan.set_projection(projection));
  common::ObSEArray<common::ObObj, 2> row;
  bool has_row = false;
  ASSERT_EQ(common::OB_SUCCESS, scan.next_row(row, has_row));
  ASSERT_TRUE(has_row);
  EXPECT_EQ("first", varchar(row.at(0)));
  EXPECT_EQ(1, row.at(1).get_int());
  ASSERT_EQ(common::OB_SUCCESS, scan.rescan());
  EXPECT_TRUE(fingerprint == scan.get_fingerprint());
  ASSERT_EQ(common::OB_SUCCESS, scan.next_row(row, has_row));
  EXPECT_EQ("first", varchar(row.at(0)));
  projection.reset();
  ASSERT_EQ(common::OB_SUCCESS, scan.set_projection(projection));
  ASSERT_EQ(common::OB_SUCCESS, scan.rescan());
  int count = 0;
  do {
    ASSERT_EQ(common::OB_SUCCESS, scan.next_row(row, has_row));
    ASSERT_EQ(0, row.count());
    if (has_row) { ++count; }
  } while (has_row);
  EXPECT_EQ(2, count);
  scan.close();
  EXPECT_EQ(0, scan.get_columns().count());
  EXPECT_EQ(0, scan.get_row_count());
  ASSERT_EQ(common::OB_SUCCESS, open(scan));
  ASSERT_EQ(common::OB_SUCCESS, scan.next_row(row, has_row));
  EXPECT_EQ(2, row.count());
}

TEST_F(JsonlScanner, rejects_invalid_syntax_utf8_duplicates_and_nested_values)
{
  const std::pair<std::string, int> cases[] = {
    {"{\"a\":01}\n", common::OB_INVALID_ARGUMENT},
    {"{\"a\":+1}\n", common::OB_INVALID_ARGUMENT},
    {"{\"a\":1.}\n", common::OB_INVALID_ARGUMENT},
    {"{\"a\":1e}\n", common::OB_INVALID_ARGUMENT},
    {"{\"a\":trueX}\n", common::OB_INVALID_ARGUMENT},
    {"{\"a\":1,}\n", common::OB_INVALID_ARGUMENT},
    {"{\"a\":\"\\ud800\"}\n", common::OB_INVALID_ARGUMENT},
    {"{\"a\":\"\\udc00\"}\n", common::OB_INVALID_ARGUMENT},
    {"{\"a\":\"\\ud800\\u0041\"}\n", common::OB_INVALID_ARGUMENT},
    {std::string("{\"a\":\"") + "\xc0\xaf" + "\"}\n", common::OB_INVALID_ARGUMENT},
    {"{\"a\":1,\"\\u0061\":2}\n", common::OB_INVALID_ARGUMENT},
    {"{\"a\":{}}\n", common::OB_NOT_SUPPORTED},
    {"{\"a\":[]}\n", common::OB_NOT_SUPPORTED},
    {"[]\n", common::OB_INVALID_ARGUMENT},
    {"\n \r\n", common::OB_INVALID_ARGUMENT},
    {"{}\n", common::OB_INVALID_ARGUMENT}
  };
  for (const auto &item : cases) {
    SCOPED_TRACE(item.first);
    write(item.first);
    JsonlFileScanner scan;
    EXPECT_EQ(item.second, open(scan));
    EXPECT_GE(scan.get_error_location().line_, 1);
    EXPECT_GE(scan.get_error_location().column_, 1);
  }
}

TEST_F(JsonlScanner, reports_physical_line_column_and_byte)
{
  write("{\"a\":1}\r\n\r\n{\"a\":}\n");
  JsonlFileScanner scan;
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, open(scan));
  EXPECT_EQ(3, scan.get_error_location().line_);
  EXPECT_EQ(6, scan.get_error_location().column_);
  EXPECT_EQ(16, scan.get_error_location().byte_offset_);
}

TEST_F(JsonlScanner, detects_mutation_before_buffered_next_row_and_rescan)
{
  write("{\"a\":1}\n{\"a\":2}\n");
  JsonlFileScanner scan;
  ASSERT_EQ(common::OB_SUCCESS, open(scan));
  common::ObSEArray<common::ObObj, 2> row;
  bool has_row = false;
  ASSERT_EQ(common::OB_SUCCESS, scan.next_row(row, has_row));
  ASSERT_TRUE(has_row);
  write("{\"a\":999}\n");
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, scan.next_row(row, has_row));
  EXPECT_FALSE(has_row);
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, scan.rescan());
}

TEST_F(JsonlScanner, rejects_replaced_inode_and_stale_schema_fingerprint)
{
  write("{\"a\":1}\n");
  JsonlFileScanner scan;
  ASSERT_EQ(common::OB_SUCCESS, open(scan));
  const ObFileFingerprint fingerprint = scan.get_fingerprint();
  ASSERT_EQ(0, ::unlink(path_.c_str()));
  write("{\"a\":1}\n");
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, scan.rescan());
  JsonlFileScanner replacement;
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, replacement.open(str(root_), str(path_), &fingerprint));
}

TEST_F(JsonlScanner, checks_column_field_and_record_limits)
{
  std::string object = "{";
  for (int64_t i = 0; i <= ObFileScanLimits::MAX_COLUMNS; ++i) {
    object += (i == 0 ? "" : ",") + std::string("\"k") + std::to_string(i) + "\":1";
  }
  object += "}\n";
  write(object);
  JsonlFileScanner scan;
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, open(scan));
  write(std::string("{\"a\":\"") + std::string(ObFileScanLimits::MAX_FIELD_BYTES + 1, 'a') + "\"}\n");
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, open(scan));
  write(std::string(ObFileScanLimits::MAX_RECORD_BYTES + 1, ' ') + "{}\n");
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, open(scan));
}

TEST_F(JsonlScanner, input_rejects_fifo_nonregular_and_nul_without_fd_leaks)
{
  ObFileInput input;
  ASSERT_EQ(0, ::mkfifo(path_.c_str(), 0600));
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, input.open(str(root_), str(path_)));
  EXPECT_EQ(-1, input.get_fd());
  ASSERT_EQ(0, ::unlink(path_.c_str()));
  write("{\"a\":1}\n");
  const std::string nul_path = path_ + std::string("\0suffix", 7);
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, input.open(str(root_), str(nul_path)));
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, input.open(str(root_), str(root_ + std::string("\0suffix", 7))));
  const int before = descriptor_count();
  ASSERT_GT(before, 0);
  for (int i = 0; i < 32; ++i) {
    {
      ObFileInput local;
      ASSERT_EQ(common::OB_SUCCESS, local.open(str(root_), str(path_)));
    }
    write("{\"a\":}\n");
    JsonlFileScanner scan;
    EXPECT_EQ(common::OB_INVALID_ARGUMENT, open(scan));
    write("{\"a\":1}\n");
  }
  EXPECT_EQ(before, descriptor_count());
}
} // namespace
