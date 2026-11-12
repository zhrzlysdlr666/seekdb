#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <string>
#include "gtest/gtest.h"
#include "common/object/ob_object.h"
#include "sql/engine/basic/ob_file_scan_utils.h"
#include "share/ob_errno.h"

using namespace oceanbase;
using namespace oceanbase::sql;

TEST(FileSqlScalar, strict_numbers_and_calendar)
{
  EXPECT_EQ(common::ObIntType, file_sql_infer_scalar("-9223372036854775808"));
  EXPECT_EQ(common::ObIntType, file_sql_infer_scalar("9223372036854775807"));
  EXPECT_EQ(common::ObVarcharType, file_sql_infer_scalar("9223372036854775808"));
  EXPECT_EQ(common::ObVarcharType, file_sql_infer_scalar("-9223372036854775809"));
  EXPECT_EQ(common::ObVarcharType, file_sql_infer_scalar(" 12"));
  EXPECT_EQ(common::ObVarcharType, file_sql_infer_scalar("0x12"));
  EXPECT_EQ(common::ObVarcharType, file_sql_infer_scalar("NaN"));
  EXPECT_EQ(common::ObVarcharType, file_sql_infer_scalar("1e999"));
  EXPECT_EQ(common::ObDoubleType, file_sql_infer_scalar("-1.25e+2"));
  EXPECT_EQ(common::ObTinyIntType, file_sql_infer_scalar("FaLsE"));
  EXPECT_EQ(common::ObMySQLDateType, file_sql_infer_scalar("2024-02-29"));
  EXPECT_EQ(common::ObVarcharType, file_sql_infer_scalar("2023-02-29"));
  EXPECT_EQ(common::ObVarcharType, file_sql_infer_scalar("2024-04-31"));
  EXPECT_EQ(common::ObMySQLDateTimeType, file_sql_infer_scalar("2024-02-29T23:59:59.123456"));
  EXPECT_EQ(common::ObVarcharType, file_sql_infer_scalar("2024-02-29 24:00:00"));
  EXPECT_EQ(common::ObVarcharType, file_sql_infer_scalar("2024-02-29 23:59:59.1234567"));
}

TEST(FileSqlScalar, merge_is_commutative_and_conversion_is_strict)
{
  const common::ObObjType types[] = {common::ObNullType, common::ObTinyIntType,
      common::ObIntType, common::ObDoubleType, common::ObMySQLDateType,
      common::ObMySQLDateTimeType, common::ObVarcharType};
  for (auto a : types) {
    for (auto b : types) {
      EXPECT_EQ(file_sql_merge_types(a, b), file_sql_merge_types(b, a));
    }
  }
  common::ObObj obj;
  ASSERT_EQ(common::OB_SUCCESS, file_sql_convert_scalar("-9223372036854775808", common::ObIntType, obj));
  EXPECT_EQ(INT64_MIN, obj.get_int());
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, file_sql_convert_scalar("9223372036854775808", common::ObIntType, obj));
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, file_sql_convert_scalar("truex", common::ObTinyIntType, obj));
  ASSERT_EQ(common::OB_SUCCESS, file_sql_convert_scalar("2024-02-29T23:59:59.123456", common::ObMySQLDateTimeType, obj));
  EXPECT_EQ(common::ObMySQLDateTimeType, obj.get_type());
}

TEST(FileSqlScalar, utf8_rejects_overlong_surrogate_and_truncation)
{
  EXPECT_TRUE(file_sql_valid_utf8("\xe4\xb8\xad\xe6\x96\x87"));
  EXPECT_FALSE(file_sql_valid_utf8(std::string("\xc0\xaf", 2)));
  EXPECT_FALSE(file_sql_valid_utf8(std::string("\xed\xa0\x80", 3)));
  EXPECT_FALSE(file_sql_valid_utf8(std::string("\xf4\x90\x80\x80", 4)));
  EXPECT_FALSE(file_sql_valid_utf8(std::string("\xe4\xb8", 2)));
}

namespace {
class FileSqlCsv : public ::testing::Test
{
public:
  void SetUp() override
  {
    char name[] = "/tmp/file_sql_scalar_XXXXXX";
    char *root = ::mkdtemp(name);
    ASSERT_NE(nullptr, root);
    root_ = root;
    path_ = root_ + "/input.csv";
  }
  void TearDown() override { ::unlink(path_.c_str()); ::rmdir(root_.c_str()); }
  void write(const std::string &data)
  {
    const int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(static_cast<ssize_t>(data.size()), ::write(fd, data.data(), data.size()));
    ASSERT_EQ(0, ::close(fd));
  }
  common::ObString str(const std::string &s) { return common::ObString(s.size(), s.data()); }
  std::string root_, path_;
};

TEST_F(FileSqlCsv, normalizes_header_collisions_and_all_null_schema)
{
  write("\xef\xbb\xbfid,id,id__2,,allnull\r\n1,2,3,4,\r\n");
  ObCsvFileScanner scan;
  ASSERT_EQ(common::OB_SUCCESS, scan.open(str(root_), str(path_)));
  ASSERT_EQ(5, scan.get_columns().count());
  EXPECT_STREQ("id", scan.get_columns().at(0).name_.ptr());
  EXPECT_STREQ("id__3", scan.get_columns().at(1).name_.ptr());
  EXPECT_STREQ("id__2", scan.get_columns().at(2).name_.ptr());
  EXPECT_STREQ("column3", scan.get_columns().at(3).name_.ptr());
  EXPECT_EQ(common::ObVarcharType, scan.get_columns().at(4).type_);
  EXPECT_TRUE(scan.get_columns().at(4).nullable_);
}

TEST_F(FileSqlCsv, preserves_quoted_null_text_and_multiline_values)
{
  write("a,b,c,d\r\n,\"\",NULL,\"x\r\ny\"\r\n");
  ObCsvScanOptions options;
  options.null_string_ = "NULL";
  ObCsvFileScanner scan;
  ASSERT_EQ(common::OB_SUCCESS, scan.open(str(root_), str(path_), options));
  common::ObSEArray<common::ObObj, 4> row;
  bool has_row = false;
  ASSERT_EQ(common::OB_SUCCESS, scan.next_row(row, has_row));
  ASSERT_TRUE(has_row);
  EXPECT_TRUE(row.at(0).is_null());
  EXPECT_FALSE(row.at(1).is_null());
  EXPECT_EQ(0, row.at(1).get_varchar().length());
  EXPECT_TRUE(row.at(2).is_null());
  EXPECT_EQ(std::string("x\r\ny"), std::string(row.at(3).get_string_ptr(), row.at(3).get_string_len()));
}

TEST_F(FileSqlCsv, rejects_bad_utf8_before_a_delimiter_and_reports_physical_line)
{
  write("a,b\n\"x\ny\",\xff\n");
  ObCsvFileScanner scan;
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, scan.open(str(root_), str(path_)));
  EXPECT_EQ(3, scan.get_error_location().line_);
  EXPECT_EQ(2, scan.get_error_location().column_);
  EXPECT_GE(scan.get_error_location().byte_offset_, 10);
}

TEST_F(FileSqlCsv, rejects_nonregular_and_embedded_nul_without_blocking)
{
  ObFileInput input;
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, input.open(str(root_), str(root_)));
  ASSERT_EQ(0, ::mkfifo(path_.c_str(), 0600));
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, input.open(str(root_), str(path_)));
  const std::string nul_path = path_ + std::string("\0suffix", 7);
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, input.open(str(root_), str(nul_path)));
}

TEST_F(FileSqlCsv, no_header_uses_zero_based_names_and_validates_empty_projection)
{
  write("1,2\n3,4\n");
  ObCsvScanOptions options;
  options.has_header_ = false;
  ObCsvFileScanner scan;
  ASSERT_EQ(common::OB_SUCCESS, scan.open(str(root_), str(path_), options));
  EXPECT_STREQ("column0", scan.get_columns().at(0).name_.ptr());
  common::ObSEArray<int64_t, 1> columns;
  ASSERT_EQ(common::OB_SUCCESS, scan.set_projection(columns));
  common::ObSEArray<common::ObObj, 2> row;
  bool has_row = false;
  ASSERT_EQ(common::OB_SUCCESS, scan.next_row(row, has_row));
  EXPECT_TRUE(has_row);
  EXPECT_EQ(0, row.count());
  EXPECT_EQ(2, scan.get_row_count());
}

TEST_F(FileSqlCsv, strict_quotes_rejects_every_malformed_position)
{
  const char *invalid[] = {"a\n\"unterminated", "a\n\"x\"tail\n", "a\nx\"y\n", "a\n\"x\\"};
  for (const char *data : invalid) {
    write(data);
    ObCsvFileScanner scan;
    EXPECT_EQ(common::OB_INVALID_ARGUMENT, scan.open(str(root_), str(path_)));
    EXPECT_GE(scan.get_error_location().line_, 2);
    EXPECT_EQ(1, scan.get_error_location().column_);
    EXPECT_GE(scan.get_error_location().byte_offset_, 2);
  }
}

TEST_F(FileSqlCsv, field_record_and_column_limits_are_enforced)
{
  ObCsvScanOptions options;
  options.has_header_ = false;
  ObCsvFileScanner scan;
  std::string field(ObFileScanLimits::MAX_FIELD_BYTES + 1, 'x');
  write(field);
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, scan.open(str(root_), str(path_), options));
  std::string many_columns(ObFileScanLimits::MAX_COLUMNS, ',');
  write(many_columns);
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, scan.open(str(root_), str(path_), options));
  field.resize(ObFileScanLimits::MAX_FIELD_BYTES);
  write(field + ',' + field + ',' + field + ',' + field + '\n');
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, scan.open(str(root_), str(path_), options));
}

TEST_F(FileSqlCsv, file_growth_is_rejected_before_bounded_read)
{
  write("a\n1\n");
  ObFileInput input;
  ASSERT_EQ(common::OB_SUCCESS, input.open(str(root_), str(path_)));
  write("a\n1\n2\n");
  char buffer[16] = {0};
  int64_t bytes = -1;
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, input.read(buffer, sizeof(buffer), bytes));
  EXPECT_EQ(0, bytes);
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, input.rescan());
}
} // namespace
