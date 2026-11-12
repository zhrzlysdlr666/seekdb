/** Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <cstring>
#include "gtest/gtest.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/charset/ob_charset.h"
#include "sql/parser/parse_node.h"

using namespace oceanbase;

// The server parser exposes a callback for PL external-variable resolution.
// These SQL-only cases have no PL namespace; fail if that unrelated path runs.
int lookup_pl_symbol(const void *, const char *, size_t, int64_t *)
{
  ADD_FAILURE() << "SQL file parser unexpectedly requested a PL symbol";
  return -4016;
}

namespace
{
class FileSqlParser : public ::testing::Test
{
public:
  static void SetUpTestSuite() { ASSERT_EQ(0, common::ObCharset::init_charset()); }
  int parse(const char *sql)
  {
    common::ObArenaAllocator arena;
    ParseResult result;
    std::memset(&result, 0, sizeof(result));
    result.malloc_pool_ = &arena;
    result.connection_collation_ = common::CS_TYPE_UTF8MB4_GENERAL_CI;
    result.charset_info_ = common::ObCharset::get_charset(common::CS_TYPE_UTF8MB4_GENERAL_CI);
    result.charset_info_nls_db_ = result.charset_info_;
    int status = ::parse_init(&result);
    if (status == 0) {
      status = ::parse_sql(&result, sql, std::strlen(sql));
      if (status == 0) { EXPECT_NE(nullptr, result.result_tree_); }
      EXPECT_EQ(0, ::parse_terminate(&result));
    }
    return status;
  }
};

TEST_F(FileSqlParser, accepts_file_functions_and_named_options)
{
  for (const char *sql : {
      "SELECT * FROM read_csv('/tmp/a.csv', delimiter=';', header=true, sample_size=-1) AS f",
      "SELECT * FROM read_json('/tmp/a.jsonl', format='newline_delimited') j",
      "SELECT * FROM read_ndjson('/tmp/a.jsonl')",
      "SELECT * FROM read_parquet('/tmp/a.parquet')",
      "SELECT * FROM glob('/tmp/*.csv')",
      "DESCRIBE SELECT * FROM read_csv('/tmp/a.csv')",
      "SELECT a.id FROM read_csv('/tmp/a.csv') a JOIN read_json('/tmp/a.jsonl') b ON a.id=b.id"}) {
    SCOPED_TRACE(sql);
    EXPECT_EQ(0, parse(sql));
  }
}

TEST_F(FileSqlParser, rejects_positional_file_options)
{
  EXPECT_NE(0, parse("SELECT * FROM read_csv('/tmp/a.csv', 'xml')"));
  EXPECT_NE(0, parse("SELECT * FROM read_json('/tmp/a.jsonl', 'newline_delimited')"));
}

TEST_F(FileSqlParser, copy_and_existing_select_syntax_coexist)
{
  for (const char *sql : {
      "COPY (SELECT 1 AS id) TO '/tmp/out.csv' (format csv, header, nullstr 'NULL')",
      "COPY (SELECT 1 UNION ALL SELECT 2) TO '/tmp/out.csv' WITH (header false, delimiter '|')",
      "COPY test.sample TO '/tmp/out.csv' (format='csv', header=true)",
      "DESC SELECT 1", "DESCRIBE test.sample", "EXPLAIN SELECT 1", "SELECT 1 + 2"}) {
    SCOPED_TRACE(sql);
    EXPECT_EQ(0, parse(sql));
  }
}
} // namespace
