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
#include "sql/engine/basic/ob_jsonl_file_scanner.h"

#include <cstdio>
#include <unordered_set>
#include <utility>
#include "common/object/ob_object.h"
#include "lib/ob_errno.h"
#include "share/ob_errno.h"

namespace oceanbase
{
namespace sql
{

namespace
{
bool jsonl_digit(char ch)
{
  return ch >= '0' && ch <= '9';
}

bool jsonl_space(char ch)
{
  return ch == ' ' || ch == '\t' || ch == '\r';
}
} // namespace

JsonlFileScanner::JsonlFileScanner()
  : input_(), columns_(), column_names_(), column_ids_(), inferred_types_(), nullable_(),
    field_ids_(), fields_(), record_(), io_buffer_(), io_pos_(0), io_size_(0), line_(1),
    byte_offset_(0), record_line_(1), record_offset_(0), input_eof_(false), error_location_(),
    row_count_(0)
{
}

JsonlFileScanner::~JsonlFileScanner()
{
  close();
}

int JsonlFileScanner::open(const common::ObString &secure_file_priv,
                           const common::ObString &path,
                           const ObFileFingerprint *expected_fingerprint)
{
  int ret = common::OB_SUCCESS;
  close();
  if (OB_FAIL(input_.open(secure_file_priv, path, expected_fingerprint))) {
  } else {
    bool has_record = false;
    while (OB_SUCC(ret)) {
      if (OB_FAIL(read_record(has_record))) {
      } else if (!has_record) {
        break;
      } else if (OB_FAIL(infer_record())) {
      } else {
        ++row_count_;
      }
    }
    if (OB_SUCC(ret) && column_names_.empty()) {
      ret = fail("JSONL file has no columns", 0);
    }
    // Construct ObStrings only after the name vector stops growing.
    for (size_t i = 0; OB_SUCC(ret) && i < column_names_.size(); ++i) {
      if (common::ObNullType == inferred_types_[i]) {
        inferred_types_[i] = common::ObVarcharType;
        nullable_[i] = 1;
      }
      ObFileColumn column;
      column.name_.assign_ptr(const_cast<char *>(column_names_[i].c_str()),
                              static_cast<int32_t>(column_names_[i].size()));
      column.type_ = inferred_types_[i];
      column.nullable_ = nullable_[i] != 0;
      if (OB_FAIL(columns_.push_back(column))) {
      }
    }
    if (OB_SUCC(ret)) {
      ret = rescan();
    }
  }
  if (OB_FAIL(ret)) {
    // Keep the diagnostic available to the resolver, but never keep an open fd.
    input_.close();
  }
  return ret;
}

int JsonlFileScanner::next_row(common::ObIArray<common::ObObj> &row, bool &has_row)
{
  int ret = common::OB_SUCCESS;
  has_row = false;
  row.reset();
  if (OB_FAIL(input_.validate_unchanged())) {
  } else if (OB_FAIL(read_record(has_row))) {
  } else if (OB_FAIL(input_.validate_unchanged())) {
  } else if (has_row && OB_FAIL(align_record())) {
  } else if (has_row) {
    // Parse and validate every field above, even for an empty COUNT(*) projection.
    const int64_t output_count = has_projection_ ? projection_.count() : columns_.count();
    for (int64_t i = 0; OB_SUCC(ret) && i < output_count; ++i) {
      const int64_t column_idx = has_projection_ ? projection_.at(i) : i;
      common::ObObj obj;
      if (OB_FAIL(convert_field(column_idx, obj))) {
        const int64_t field_idx = field_ids_[column_idx];
        ret = fail("JSONL scalar conversion failed",
                   field_idx < 0 ? 0 : fields_[field_idx].position_);
      } else if (OB_FAIL(row.push_back(obj))) {
      }
    }
  }
  if (OB_FAIL(ret)) {
    has_row = false;
    row.reset();
  }
  return ret;
}

int JsonlFileScanner::rescan()
{
  int ret = input_.rescan();
  if (OB_SUCC(ret)) {
    reset_reader();
    fields_.clear();
    field_ids_.clear();
    record_.clear();
    error_location_ = ObFileErrorLocation();
  }
  return ret;
}

void JsonlFileScanner::close()
{
  input_.close();
  reset_projection();
  columns_.reuse();
  column_names_.clear();
  column_ids_.clear();
  inferred_types_.clear();
  nullable_.clear();
  field_ids_.clear();
  fields_.clear();
  record_.clear();
  row_count_ = 0;
  error_location_ = ObFileErrorLocation();
  reset_reader();
}

void JsonlFileScanner::reset_reader()
{
  io_pos_ = 0;
  io_size_ = 0;
  line_ = 1;
  byte_offset_ = 0;
  record_line_ = 1;
  record_offset_ = 0;
  input_eof_ = false;
}

int JsonlFileScanner::next_byte(char &ch, bool &has_byte)
{
  int ret = common::OB_SUCCESS;
  has_byte = false;
  if (!input_eof_) {
    if (io_pos_ == io_size_) {
      int64_t read_bytes = 0;
      if (OB_FAIL(input_.read(io_buffer_, sizeof(io_buffer_), read_bytes))) {
      } else {
        io_pos_ = 0;
        io_size_ = read_bytes;
        input_eof_ = read_bytes == 0;
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

int JsonlFileScanner::read_record(bool &has_record)
{
  int ret = common::OB_SUCCESS;
  has_record = false;
  fields_.clear();
  while (OB_SUCC(ret) && !has_record && !input_eof_) {
    record_.clear();
    record_line_ = line_;
    record_offset_ = byte_offset_;
    bool line_end = false;
    while (OB_SUCC(ret) && !line_end) {
      char ch = 0;
      bool has_byte = false;
      if (OB_FAIL(next_byte(ch, has_byte))) {
      } else if (!has_byte) {
        line_end = true;
      } else if (ch == '\n') {
        ++line_;
        line_end = true;
      } else if (record_.size() >= static_cast<size_t>(ObFileScanLimits::MAX_RECORD_BYTES)) {
        ret = fail("JSONL record exceeds byte limit", record_.size());
      } else {
        record_.push_back(ch);
      }
    }
    size_t position = 0;
    if (record_offset_ == 0 && record_.size() >= 3
        && record_.compare(0, 3, "\xef\xbb\xbf", 3) == 0) {
      position = 3;
    }
    skip_space(position);
    if (OB_SUCC(ret) && position < record_.size()) {
      if (OB_FAIL(parse_record())) {
      } else {
        has_record = true;
      }
    }
  }
  return ret;
}

void JsonlFileScanner::skip_space(size_t &position) const
{
  while (position < record_.size() && jsonl_space(record_[position])) {
    ++position;
  }
}

int JsonlFileScanner::fail(const char *reason, size_t position, int code)
{
  error_location_.line_ = record_line_;
  error_location_.column_ = static_cast<int64_t>(position) + 1;
  error_location_.byte_offset_ = record_offset_ + static_cast<int64_t>(position);
  char message[384];
  std::snprintf(message, sizeof(message), "%s at line %lld, column %lld, byte %lld",
                reason, static_cast<long long>(error_location_.line_),
                static_cast<long long>(error_location_.column_),
                static_cast<long long>(error_location_.byte_offset_));
  if (common::OB_NOT_SUPPORTED == code) {
    LOG_USER_ERROR(OB_NOT_SUPPORTED, message);
  } else {
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, message);
  }
  return code;
}

int JsonlFileScanner::append_string(std::string &value, const char *bytes,
                                    size_t length, size_t position)
{
  int ret = common::OB_SUCCESS;
  if (length > static_cast<size_t>(ObFileScanLimits::MAX_FIELD_BYTES) - value.size()) {
    ret = fail("JSONL field exceeds byte limit", position);
  } else {
    value.append(bytes, length);
  }
  return ret;
}

int JsonlFileScanner::parse_hex4(size_t &position, uint32_t &codepoint)
{
  int ret = common::OB_SUCCESS;
  codepoint = 0;
  for (int i = 0; OB_SUCC(ret) && i < 4; ++i) {
    if (position == record_.size()) {
      ret = fail("Incomplete JSONL Unicode escape", position);
    } else {
      const char ch = record_[position];
      int digit = -1;
      if (ch >= '0' && ch <= '9') {
        digit = ch - '0';
      } else if (ch >= 'a' && ch <= 'f') {
        digit = ch - 'a' + 10;
      } else if (ch >= 'A' && ch <= 'F') {
        digit = ch - 'A' + 10;
      }
      if (digit < 0) {
        ret = fail("Invalid JSONL Unicode escape", position);
      } else {
        codepoint = (codepoint << 4) | digit;
        ++position;
      }
    }
  }
  return ret;
}

int JsonlFileScanner::parse_string(size_t &position, std::string &value)
{
  int ret = common::OB_SUCCESS;
  value.clear();
  const size_t start = position;
  bool closed = false;
  if (position >= record_.size() || record_[position] != '"') {
    ret = fail("Expected JSONL string", position);
  } else {
    ++position;
    while (OB_SUCC(ret) && position < record_.size() && !closed) {
      const size_t character_position = position;
      char ch = record_[position++];
      if (ch == '"') {
        closed = true;
      } else if (static_cast<unsigned char>(ch) < 0x20) {
        ret = fail("Unescaped control character in JSONL string", character_position);
      } else if (ch != '\\') {
        ret = append_string(value, &ch, 1, character_position);
      } else if (position == record_.size()) {
        ret = fail("Incomplete JSONL escape", position);
      } else {
        ch = record_[position++];
        if (ch == 'u') {
          uint32_t codepoint = 0;
          if (OB_FAIL(parse_hex4(position, codepoint))) {
          } else if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
            if (position + 2 > record_.size() || record_[position] != '\\'
                || record_[position + 1] != 'u') {
              ret = fail("Missing low surrogate in JSONL string", position);
            } else {
              position += 2;
              uint32_t low = 0;
              if (OB_FAIL(parse_hex4(position, low))) {
              } else if (low < 0xdc00 || low > 0xdfff) {
                ret = fail("Invalid low surrogate in JSONL string", position - 4);
              } else {
                codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + low - 0xdc00;
              }
            }
          } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
            ret = fail("Unexpected low surrogate in JSONL string", position - 4);
          }
          if (OB_SUCC(ret)) {
            char encoded[4];
            size_t count = 0;
            if (codepoint <= 0x7f) {
              encoded[count++] = static_cast<char>(codepoint);
            } else if (codepoint <= 0x7ff) {
              encoded[count++] = static_cast<char>(0xc0 | (codepoint >> 6));
              encoded[count++] = static_cast<char>(0x80 | (codepoint & 0x3f));
            } else if (codepoint <= 0xffff) {
              encoded[count++] = static_cast<char>(0xe0 | (codepoint >> 12));
              encoded[count++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
              encoded[count++] = static_cast<char>(0x80 | (codepoint & 0x3f));
            } else {
              encoded[count++] = static_cast<char>(0xf0 | (codepoint >> 18));
              encoded[count++] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f));
              encoded[count++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
              encoded[count++] = static_cast<char>(0x80 | (codepoint & 0x3f));
            }
            ret = append_string(value, encoded, count, character_position);
          }
        } else {
          switch (ch) {
            case '"': case '\\': case '/': break;
            case 'b': ch = '\b'; break;
            case 'f': ch = '\f'; break;
            case 'n': ch = '\n'; break;
            case 'r': ch = '\r'; break;
            case 't': ch = '\t'; break;
            default: ret = fail("Invalid JSONL string escape", position - 1); break;
          }
          if (OB_SUCC(ret)) {
            ret = append_string(value, &ch, 1, character_position);
          }
        }
      }
    }
    if (OB_SUCC(ret) && !closed) {
      ret = fail("Unterminated JSONL string", position);
    } else if (OB_SUCC(ret) && !file_sql_valid_utf8(value)) {
      ret = fail("Invalid UTF-8 in JSONL string", start);
    }
  }
  return ret;
}

int JsonlFileScanner::parse_value(size_t &position, Field &field)
{
  int ret = common::OB_SUCCESS;
  field.position_ = position;
  const size_t start = position;
  if (position == record_.size()) {
    ret = fail("Missing JSONL value", position);
  } else if (record_[position] == '{' || record_[position] == '[') {
    ret = fail("Nested JSONL objects and arrays", position, common::OB_NOT_SUPPORTED);
  } else if (record_[position] == '"') {
    if (OB_FAIL(parse_string(position, field.value_))) {
    } else {
      const common::ObObjType inferred = file_sql_infer_scalar(field.value_);
      field.type_ = inferred == common::ObMySQLDateType || inferred == common::ObMySQLDateTimeType
          ? inferred : common::ObVarcharType;
    }
  } else if (record_.compare(position, 4, "null") == 0) {
    field.type_ = common::ObNullType;
    position += 4;
  } else if (record_.compare(position, 4, "true") == 0) {
    field.type_ = common::ObTinyIntType;
    field.value_ = "true";
    position += 4;
  } else if (record_.compare(position, 5, "false") == 0) {
    field.type_ = common::ObTinyIntType;
    field.value_ = "false";
    position += 5;
  } else {
    // JSON numbers have a stricter grammar than CSV numbers: no '+', leading
    // zeroes, NaN/Inf, or incomplete fractions/exponents.
    if (record_[position] == '-') {
      ++position;
    }
    if (position == record_.size() || !jsonl_digit(record_[position])) {
      ret = fail("Invalid JSONL value", position);
    } else if (record_[position] == '0') {
      ++position;
      if (position < record_.size() && jsonl_digit(record_[position])) {
        ret = fail("Leading zero in JSONL number", position);
      }
    } else {
      while (position < record_.size() && jsonl_digit(record_[position])) {
        ++position;
      }
    }
    if (OB_SUCC(ret) && position < record_.size() && record_[position] == '.') {
      ++position;
      const size_t fraction_start = position;
      while (position < record_.size() && jsonl_digit(record_[position])) {
        ++position;
      }
      if (position == fraction_start) {
        ret = fail("Missing fraction in JSONL number", position);
      }
    }
    if (OB_SUCC(ret) && position < record_.size()
        && (record_[position] == 'e' || record_[position] == 'E')) {
      ++position;
      if (position < record_.size() && (record_[position] == '+' || record_[position] == '-')) {
        ++position;
      }
      const size_t exponent_start = position;
      while (position < record_.size() && jsonl_digit(record_[position])) {
        ++position;
      }
      if (position == exponent_start) {
        ret = fail("Missing exponent in JSONL number", position);
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(append_string(field.value_, record_.data() + start,
                                              position - start, start))) {
    } else if (OB_SUCC(ret)) {
      field.type_ = file_sql_infer_scalar(field.value_);
    }
  }
  return ret;
}

int JsonlFileScanner::parse_record()
{
  int ret = common::OB_SUCCESS;
  size_t position = 0;
  if (record_offset_ == 0 && record_.size() >= 3
      && record_.compare(0, 3, "\xef\xbb\xbf", 3) == 0) {
    position = 3;
  }
  skip_space(position);
  std::unordered_set<std::string> seen_keys;
  if (position == record_.size() || record_[position] != '{') {
    ret = fail("JSONL top-level value must be an object", position);
  } else {
    ++position;
    skip_space(position);
    bool closed = position < record_.size() && record_[position] == '}';
    if (closed) {
      ++position;
    }
    while (OB_SUCC(ret) && !closed) {
      Field field;
      const size_t key_position = position;
      if (fields_.size() >= static_cast<size_t>(ObFileScanLimits::MAX_COLUMNS)) {
        ret = fail("JSONL object exceeds column limit", position);
      } else if (OB_FAIL(parse_string(position, field.key_))) {
      } else if (!seen_keys.insert(field.key_).second) {
        ret = fail("Duplicate JSONL object key", key_position);
      } else {
        skip_space(position);
        if (position == record_.size() || record_[position] != ':') {
          ret = fail("Expected colon after JSONL key", position);
        } else {
          ++position;
          skip_space(position);
          if (OB_FAIL(parse_value(position, field))) {
          } else {
            fields_.push_back(std::move(field));
            skip_space(position);
            if (position == record_.size()) {
              ret = fail("Unterminated JSONL object", position);
            } else if (record_[position] == '}') {
              ++position;
              closed = true;
            } else if (record_[position] != ',') {
              ret = fail("Expected comma or closing brace in JSONL object", position);
            } else {
              ++position;
              skip_space(position);
            }
          }
        }
      }
    }
    skip_space(position);
    if (OB_SUCC(ret) && position != record_.size()) {
      ret = fail("Trailing content after JSONL object", position);
    }
  }
  return ret;
}

int JsonlFileScanner::infer_record()
{
  int ret = common::OB_SUCCESS;
  std::vector<uint8_t> present(column_names_.size(), 0);
  for (size_t i = 0; OB_SUCC(ret) && i < fields_.size(); ++i) {
    const Field &field = fields_[i];
    const auto found = column_ids_.find(field.key_);
    int64_t column_idx = -1;
    if (found == column_ids_.end()) {
      if (column_names_.size() >= static_cast<size_t>(ObFileScanLimits::MAX_COLUMNS)) {
        ret = fail("JSONL schema exceeds column limit", field.position_);
      } else {
        column_idx = static_cast<int64_t>(column_names_.size());
        column_names_.push_back(field.key_);
        column_ids_.emplace(field.key_, column_idx);
        inferred_types_.push_back(common::ObNullType);
        nullable_.push_back(row_count_ > 0 ? 1 : 0);
        present.push_back(0);
      }
    } else {
      column_idx = found->second;
    }
    if (OB_SUCC(ret)) {
      present[column_idx] = 1;
      if (field.type_ == common::ObNullType) {
        nullable_[column_idx] = 1;
      } else {
        inferred_types_[column_idx] = file_sql_merge_types(inferred_types_[column_idx], field.type_);
      }
    }
  }
  for (size_t i = 0; OB_SUCC(ret) && i < present.size(); ++i) {
    if (present[i] == 0) {
      nullable_[i] = 1;
    }
  }
  return ret;
}

int JsonlFileScanner::align_record()
{
  int ret = common::OB_SUCCESS;
  field_ids_.assign(column_names_.size(), -1);
  for (size_t i = 0; OB_SUCC(ret) && i < fields_.size(); ++i) {
    const auto found = column_ids_.find(fields_[i].key_);
    if (found == column_ids_.end()) {
      ret = fail("JSONL field absent from inferred schema", fields_[i].position_);
    } else {
      field_ids_[found->second] = static_cast<int64_t>(i);
    }
  }
  return ret;
}

int JsonlFileScanner::convert_field(int64_t column_idx, common::ObObj &obj) const
{
  int ret = common::OB_SUCCESS;
  if (column_idx < 0 || column_idx >= static_cast<int64_t>(field_ids_.size())) {
    ret = common::OB_INVALID_ARGUMENT;
  } else {
    const int64_t field_idx = field_ids_[column_idx];
    if (field_idx < 0 || fields_[field_idx].type_ == common::ObNullType) {
      obj.set_null();
    } else {
      ret = file_sql_convert_scalar(fields_[field_idx].value_, inferred_types_[column_idx], obj);
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
