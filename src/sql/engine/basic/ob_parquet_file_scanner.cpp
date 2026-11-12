/**
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
 */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/basic/ob_parquet_file_scanner.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <unistd.h>
#include "common/object/ob_object.h"
#include "lib/ob_errno.h"
#include "share/ob_errno.h"

namespace oceanbase
{
namespace sql
{
namespace
{
using namespace common;
static const int64_t PAGE_LIMIT = ObFileScanLimits::MAX_PARQUET_PAGE_BYTES;
static const int64_t FIELD_LIMIT = ObFileScanLimits::MAX_FIELD_BYTES;
static const int64_t FOOTER_LIMIT = 64 * 1024 * 1024;
static const int64_t HEADER_LIMIT = 64 * 1024;
static const int64_t ROW_GROUP_LIMIT = 65536;

uint64_t little(const uint8_t *p, int bytes)
{
  uint64_t value = 0;
  for (int i = 0; i < bytes; ++i) { value |= uint64_t(p[i]) << (8 * i); }
  return value;
}

// A bounded subset-independent Compact Protocol reader. Unknown fields are
// skipped recursively, so newer optional metadata does not change semantics.
struct Compact
{
  const uint8_t *data_;
  int64_t size_;
  int64_t pos_;
  int ret_;
  bool truncated_;
  int depth_;
  Compact(const uint8_t *p, int64_t n)
    : data_(p), size_(n), pos_(0), ret_(OB_SUCCESS), truncated_(false), depth_(0) {}
  bool good() const { return ret_ == OB_SUCCESS; }
  void bad() { if (good()) { ret_ = OB_INVALID_ARGUMENT; } }
  void unsupported() { if (good()) { ret_ = OB_NOT_SUPPORTED; } }
  bool take(int64_t n)
  {
    if (!good()) { return false; }
    if (n < 0 || n > size_ - pos_) { truncated_ = true; bad(); return false; }
    return true;
  }
  uint8_t byte() { return take(1) ? data_[pos_++] : 0; }
  uint64_t varint()
  {
    uint64_t v = 0;
    for (int i = 0; good() && i < 10; ++i) {
      const uint8_t b = byte();
      if (i == 9 && (b & 0xfe) != 0) { bad(); break; }
      v |= uint64_t(b & 0x7f) << (i * 7);
      if ((b & 0x80) == 0) { return v; }
    }
    bad(); return 0;
  }
  int64_t integer(uint8_t type, uint8_t expected)
  {
    if (type != expected) { bad(); return 0; }
    const uint64_t v = varint();
    const int64_t value = static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1);
    if ((type == 4 && (value < INT16_MIN || value > INT16_MAX))
        || (type == 5 && (value < INT32_MIN || value > INT32_MAX))) { bad(); }
    return value;
  }
  bool boolean(uint8_t type)
  {
    if (type != 1 && type != 2) { bad(); }
    return type == 1;
  }
  std::string binary(uint8_t type)
  {
    std::string value;
    if (type != 8) { bad(); }
    const uint64_t n = varint();
    if (n > static_cast<uint64_t>(FIELD_LIMIT)) { bad(); }
    if (good() && take(static_cast<int64_t>(n))) {
      value.assign(reinterpret_cast<const char *>(data_ + pos_), static_cast<size_t>(n));
      pos_ += n;
    }
    return value;
  }
  int64_t list(uint8_t type, uint8_t expected_element, int64_t limit)
  {
    if (type != 9 && type != 10) { bad(); return 0; }
    const uint8_t head = byte();
    uint64_t n = head >> 4;
    if (n == 15) { n = varint(); }
    if ((head & 15) != expected_element || n > static_cast<uint64_t>(limit)
        || n > static_cast<uint64_t>(size_ - pos_)) { bad(); return 0; }
    return good() ? static_cast<int64_t>(n) : 0;
  }
  template <typename F> void fields(F function)
  {
    if (++depth_ > 32) { bad(); }
    int64_t previous = 0;
    uint64_t seen = 0;
    bool ended = false;
    while (good() && !ended) {
      const uint8_t head = byte();
      if (head == 0) { ended = true; }
      else {
        const uint8_t type = head & 15;
        const int64_t id = (head >> 4) != 0 ? previous + (head >> 4) : integer(4, 4);
        if (id <= 0 || id > INT16_MAX || type == 0 || type > 12) { bad(); }
        if (id < 64) {
          const uint64_t mask = uint64_t(1) << id;
          if (seen & mask) { bad(); }
          seen |= mask;
        }
        previous = id;
        if (good()) { function(id, type); }
      }
    }
    --depth_;
  }
  void skip(uint8_t type, bool collection_boolean = false)
  {
    if (!good()) { return; }
    switch (type) {
      case 1: case 2:
        if (collection_boolean) { const uint8_t b = byte(); if (b != 1 && b != 2) { bad(); } }
        break;
      case 3: if (take(1)) { ++pos_; } break;
      case 4: case 5: case 6: (void)integer(type, type); break;
      case 7: if (take(8)) { pos_ += 8; } break;
      case 8: {
        const uint64_t n = varint();
        if (n > static_cast<uint64_t>(size_ - pos_)) { truncated_ = true; bad(); }
        else if (good()) { pos_ += static_cast<int64_t>(n); }
        break;
      }
      case 9: case 10: {
        if (++depth_ > 32) { bad(); }
        const uint8_t h = byte();
        const uint8_t element = h & 15;
        const uint64_t n = (h >> 4) == 15 ? varint() : h >> 4;
        if (n > static_cast<uint64_t>(size_ - pos_) || element == 0 || element > 12) { bad(); }
        for (uint64_t i = 0; good() && i < n; ++i) { skip(element, true); }
        --depth_;
        break;
      }
      case 11: {
        if (++depth_ > 32) { bad(); }
        const uint64_t n = varint();
        if (n > static_cast<uint64_t>((size_ - pos_) / 2)) { bad(); }
        if (good() && n != 0) {
          const uint8_t h = byte();
          if ((h >> 4) == 0 || (h >> 4) > 12 || (h & 15) == 0 || (h & 15) > 12) { bad(); }
          for (uint64_t i = 0; good() && i < n; ++i) { skip(h >> 4, true); skip(h & 15, true); }
        }
        --depth_;
        break;
      }
      case 12: fields([&](int64_t, uint8_t t) { skip(t); }); break;
      default: bad(); break;
    }
  }
  void structure(uint8_t type) { if (type != 12) { bad(); } }
};

struct Schema
{
  int physical_ = -1;
  int repetition_ = -1;
  int64_t children_ = -1;
  int converted_ = -1;
  int logical_ = -1;
  int timestamp_unit_ = -1;
  std::string name_;
  ObObjType sql_type_ = ObNullType;
};

void logical_type(Compact &c, Schema &s)
{
  int annotations = 0;
  c.fields([&](int64_t id, uint8_t type) {
    ++annotations;
    c.structure(type);
    if (id == 1 || id == 6) {
      s.logical_ = id == 1 ? 0 : 6;
      c.skip(type);
    } else if (id == 8) {
      s.logical_ = 9;
      int seen = 0;
      c.fields([&](int64_t tid, uint8_t t) {
        if (tid == 1) { (void)c.boolean(t); seen |= 1; }
        else if (tid == 2) {
          seen |= 2; c.structure(t);
          int units = 0;
          c.fields([&](int64_t uid, uint8_t ut) {
            ++units; c.structure(ut);
            if (uid != 1 && uid != 2) { c.unsupported(); }
            s.timestamp_unit_ = static_cast<int>(uid);
            c.skip(ut);
          });
          if (units != 1) { c.bad(); }
        } else { c.skip(t); }
      });
      if (seen != 3) { c.bad(); }
    } else {
      // Signed integer annotations are accepted only at their physical width.
      if (id == 10) {
        int width = -1; bool signed_value = false; int seen = 0;
        c.fields([&](int64_t iid, uint8_t it) {
          if (iid == 1) { if (it != 3) { c.bad(); } width = c.byte(); seen |= 1; }
          else if (iid == 2) { signed_value = c.boolean(it); seen |= 2; }
          else { c.skip(it); }
        });
        if (seen != 3) { c.bad(); }
        else if (!signed_value || (width != 8 && width != 16 && width != 32 && width != 64)) { c.unsupported(); }
        else { s.logical_ = 15 + (width == 8 ? 0 : width == 16 ? 1 : width == 32 ? 2 : 3); }
      } else { c.unsupported(); }
    }
  });
  if (annotations != 1) { c.bad(); }
}

void schema_element(Compact &c, Schema &s)
{
  bool named = false;
  c.fields([&](int64_t id, uint8_t t) {
    switch (id) {
      case 1: s.physical_ = c.integer(t, 5); break;
      case 3: s.repetition_ = c.integer(t, 5); break;
      case 4: s.name_ = c.binary(t); named = true; break;
      case 5: s.children_ = c.integer(t, 5); break;
      case 6: s.converted_ = c.integer(t, 5); break;
      case 10: c.structure(t); logical_type(c, s); break;
      default: c.skip(t); break;
    }
  });
  if (!named || s.name_.empty() || s.name_.find('\0') != std::string::npos
      || !file_sql_valid_utf8(s.name_)) { c.bad(); }
}

int validate_schema(Schema &s)
{
  int ret = OB_SUCCESS;
  if (s.children_ != -1 || s.repetition_ == 2) { ret = OB_NOT_SUPPORTED; }
  else if (s.repetition_ != 0 && s.repetition_ != 1) { ret = OB_INVALID_ARGUMENT; }
  else if (s.physical_ < 0 || s.physical_ > 7) { ret = OB_INVALID_ARGUMENT; }
  else if (s.physical_ == 3 || s.physical_ == 7) { ret = OB_NOT_SUPPORTED; }
  else {
    int annotation = s.logical_ >= 0 ? s.logical_ : s.converted_;
    if (s.logical_ == 9) { annotation = s.timestamp_unit_ == 1 ? 9 : 10; }
    if (s.logical_ >= 0 && s.converted_ >= 0 && annotation != s.converted_) { ret = OB_INVALID_ARGUMENT; }
    else if (annotation == 0 && s.physical_ == 6) { s.sql_type_ = ObVarcharType; }
    else if (annotation == 6 && s.physical_ == 1) { s.sql_type_ = ObMySQLDateType; }
    else if ((annotation == 9 || annotation == 10) && s.physical_ == 2) {
      s.timestamp_unit_ = annotation == 9 ? 1 : 2; s.sql_type_ = ObMySQLDateTimeType;
    } else if (annotation >= 15 && annotation <= 18
        && s.physical_ == (annotation == 18 ? 2 : 1)) { s.sql_type_ = ObIntType; }
    else if (annotation >= 0) { ret = OB_NOT_SUPPORTED; }
    else {
      switch (s.physical_) {
        case 0: s.sql_type_ = ObTinyIntType; break;
        case 1: case 2: s.sql_type_ = ObIntType; break;
        case 4: case 5: s.sql_type_ = ObDoubleType; break;
        // SQL has no binary type in the Lab2 contract. Unannotated byte arrays
        // are accepted as VARCHAR only when each selected value is valid UTF-8.
        case 6: s.sql_type_ = ObVarcharType; break;
        default: ret = OB_NOT_SUPPORTED; break;
      }
    }
  }
  return ret;
}

struct Chunk
{
  int physical_ = -1;
  int codec_ = -1;
  int64_t values_ = -1;
  int64_t compressed_ = -1;
  int64_t uncompressed_ = -1;
  int64_t data_offset_ = -1;
  int64_t dict_offset_ = -1;
  int64_t start_ = -1;
  int64_t end_ = -1;
  std::string name_;
  bool metadata_ = false;
};
struct RowGroup
{
  int64_t rows_ = -1;
  std::vector<Chunk> chunks_;
};

void column_metadata(Compact &c, Chunk &chunk)
{
  uint32_t required = 0;
  c.fields([&](int64_t id, uint8_t t) {
    switch (id) {
      case 1: chunk.physical_ = c.integer(t, 5); required |= 1; break;
      case 2: {
        const int64_t n = c.list(t, 5, 32); required |= 2;
        if (n == 0) { c.bad(); }
        for (int64_t i = 0; c.good() && i < n; ++i) {
          const int64_t encoding = c.integer(5, 5);
          if (encoding != 0 && encoding != 2 && encoding != 3 && encoding != 4 && encoding != 8) { c.unsupported(); }
        }
        break;
      }
      case 3: {
        const int64_t n = c.list(t, 8, ObFileScanLimits::MAX_COLUMNS); required |= 4;
        if (n != 1) { c.unsupported(); }
        if (c.good()) { chunk.name_ = c.binary(8); }
        break;
      }
      case 4:
        chunk.codec_ = c.integer(t, 5); required |= 8;
        if (chunk.codec_ != 0 && chunk.codec_ != 1) { c.unsupported(); }
        break;
      case 5: chunk.values_ = c.integer(t, 6); required |= 16; break;
      case 6: chunk.uncompressed_ = c.integer(t, 6); required |= 32; break;
      case 7: chunk.compressed_ = c.integer(t, 6); required |= 64; break;
      case 9: chunk.data_offset_ = c.integer(t, 6); required |= 128; break;
      case 11: chunk.dict_offset_ = c.integer(t, 6); break;
      default: c.skip(t); break;
    }
  });
  if (required != 255) { c.bad(); }
}

void column_chunk(Compact &c, Chunk &chunk)
{
  bool offset_seen = false;
  c.fields([&](int64_t id, uint8_t t) {
    if (id == 1) { const std::string file = c.binary(t); if (!file.empty()) { c.unsupported(); } }
    else if (id == 2) { if (c.integer(t, 6) < 0) { c.bad(); } offset_seen = true; }
    else if (id == 3) { c.structure(t); column_metadata(c, chunk); chunk.metadata_ = true; }
    else if (id == 8 || id == 9) { c.unsupported(); }
    else { c.skip(t); }
  });
  if (!offset_seen || !chunk.metadata_) { c.bad(); }
}

void row_group(Compact &c, RowGroup &group)
{
  int required = 0;
  c.fields([&](int64_t id, uint8_t t) {
    if (id == 1) {
      required |= 1;
      const int64_t n = c.list(t, 12, ObFileScanLimits::MAX_COLUMNS);
      group.chunks_.resize(n);
      for (int64_t i = 0; c.good() && i < n; ++i) { column_chunk(c, group.chunks_[i]); }
    } else if (id == 2) { required |= 2; if (c.integer(t, 6) < 0) { c.bad(); } }
    else if (id == 3) { group.rows_ = c.integer(t, 6); required |= 4; }
    else { c.skip(t); }
  });
  if (required != 7 || group.rows_ < 0) { c.bad(); }
}

// RLE/bit-packed hybrid decoder retains one run, never expands a page's levels
// or dictionary indexes into a row-count-sized allocation.
struct Hybrid
{
  const uint8_t *data_ = nullptr;
  int64_t size_ = 0;
  int64_t pos_ = 0;
  int width_ = 0;
  uint64_t remaining_ = 0;
  uint64_t bit_index_ = 0;
  uint32_t repeated_ = 0;
  bool packed_ = false;
  void reset(const uint8_t *p, int64_t n, int width)
  {
    data_ = p; size_ = n; pos_ = 0; width_ = width;
    remaining_ = 0; bit_index_ = 0; repeated_ = 0; packed_ = false;
  }
  int next(uint32_t &value)
  {
    int ret = OB_SUCCESS;
    if (width_ < 0 || width_ > 32) { return OB_INVALID_ARGUMENT; }
    if (remaining_ == 0) {
      Compact c(data_ + pos_, size_ - pos_);
      const uint64_t h = c.varint();
      if (!c.good() || h < 2 || (h >> 1) > INT32_MAX) { return OB_INVALID_ARGUMENT; }
      pos_ += c.pos_;
      packed_ = (h & 1) != 0;
      remaining_ = packed_ ? (h >> 1) * 8 : h >> 1;
      if (packed_) {
        const uint64_t bytes = (h >> 1) * width_;
        if (bytes > static_cast<uint64_t>(size_ - pos_)) { return OB_INVALID_ARGUMENT; }
        bit_index_ = uint64_t(pos_) * 8;
        pos_ += static_cast<int64_t>(bytes);
      } else {
        const int bytes = (width_ + 7) / 8;
        if (bytes > size_ - pos_) { return OB_INVALID_ARGUMENT; }
        repeated_ = static_cast<uint32_t>(little(data_ + pos_, bytes)); pos_ += bytes;
        if (width_ < 32 && repeated_ >= (uint64_t(1) << width_)) { return OB_INVALID_ARGUMENT; }
      }
    }
    value = repeated_;
    if (packed_) {
      value = 0;
      for (int i = 0; i < width_; ++i, ++bit_index_) {
        value |= uint32_t((data_[bit_index_ >> 3] >> (bit_index_ & 7)) & 1) << i;
      }
    }
    --remaining_;
    return ret;
  }
  bool finished() const { return pos_ == size_ && (remaining_ == 0 || (packed_ && remaining_ < 8)); }
};

int decompress(int codec, const uint8_t *src, int64_t n, int64_t expected, std::vector<uint8_t> &out)
{
  if (n < 0 || n > PAGE_LIMIT || expected < 0 || expected > PAGE_LIMIT) { return OB_INVALID_ARGUMENT; }
  if (codec == 0) {
    if (n != expected) { return OB_INVALID_ARGUMENT; }
    if (n == 0) { out.clear(); } else { out.assign(src, src + n); }
    return OB_SUCCESS;
  }
  if (codec != 1) { return OB_NOT_SUPPORTED; }
  Compact c(src, n);
  const uint64_t claimed = c.varint();
  if (!c.good() || claimed != static_cast<uint64_t>(expected)) { return OB_INVALID_ARGUMENT; }
  out.resize(expected);
  int64_t written = 0;
  while (c.good() && c.pos_ < n) {
    const uint8_t tag = c.byte();
    const int kind = tag & 3;
    int64_t length = 0;
    uint64_t offset = 0;
    if (kind == 0) {
      length = tag >> 2;
      if (length < 60) { ++length; }
      else {
        const int bytes = static_cast<int>(length - 59);
        if (!c.take(bytes)) { break; }
        length = static_cast<int64_t>(little(src + c.pos_, bytes)) + 1; c.pos_ += bytes;
      }
      if (length > expected - written || !c.take(length)) { c.bad(); }
      else { std::memcpy(out.data() + written, src + c.pos_, length); written += length; c.pos_ += length; }
    } else {
      if (kind == 1) { length = 4 + ((tag >> 2) & 7); offset = uint64_t(tag & 0xe0) << 3; offset |= c.byte(); }
      else {
        length = 1 + (tag >> 2);
        const int bytes = kind == 2 ? 2 : 4;
        if (!c.take(bytes)) { break; }
        offset = little(src + c.pos_, bytes); c.pos_ += bytes;
      }
      if (!c.good() || offset == 0 || offset > static_cast<uint64_t>(written) || length > expected - written) { c.bad(); }
      else {
        // The source may overlap the destination, which is legal in Snappy.
        for (int64_t i = 0; i < length; ++i) { out[written] = out[written - offset]; ++written; }
      }
    }
  }
  return c.good() && c.pos_ == n && written == expected ? OB_SUCCESS : OB_INVALID_ARGUMENT;
}

uint32_t page_crc(const uint8_t *p, int64_t n)
{
  uint32_t crc = 0xffffffffU;
  for (int64_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (int j = 0; j < 8; ++j) { crc = (crc >> 1) ^ (0xedb88320U & -(crc & 1)); }
  }
  return ~crc;
}

struct Page
{
  int kind_ = -1;
  int64_t compressed_ = -1;
  int64_t uncompressed_ = -1;
  int64_t values_ = -1;
  int encoding_ = -1;
  int def_encoding_ = -1;
  int rep_encoding_ = -1;
  int64_t nulls_ = -1;
  int64_t rows_ = -1;
  int64_t def_bytes_ = -1;
  int64_t rep_bytes_ = -1;
  bool compressed_values_ = true;
  bool has_crc_ = false;
  uint32_t crc_ = 0;
  int subtype_ = -1;
};

void page_header(Compact &c, Page &page)
{
  int required = 0;
  c.fields([&](int64_t id, uint8_t t) {
    if (id == 1) { page.kind_ = c.integer(t, 5); required |= 1; }
    else if (id == 2) { page.uncompressed_ = c.integer(t, 5); required |= 2; }
    else if (id == 3) { page.compressed_ = c.integer(t, 5); required |= 4; }
    else if (id == 4) { page.crc_ = static_cast<uint32_t>(c.integer(t, 5)); page.has_crc_ = true; }
    else if (id == 5 || id == 7 || id == 8) {
      if (page.subtype_ != -1) { c.bad(); }
      page.subtype_ = id; c.structure(t);
      int fields = 0;
      c.fields([&](int64_t pid, uint8_t pt) {
        if (pid == 1) { page.values_ = c.integer(pt, 5); fields |= 1; }
        else if (id == 5 && pid == 2) { page.encoding_ = c.integer(pt, 5); fields |= 2; }
        else if (id == 5 && pid == 3) { page.def_encoding_ = c.integer(pt, 5); fields |= 4; }
        else if (id == 5 && pid == 4) { page.rep_encoding_ = c.integer(pt, 5); fields |= 8; }
        else if (id == 7 && pid == 2) { page.encoding_ = c.integer(pt, 5); fields |= 2; }
        else if (id == 8 && pid == 2) { page.nulls_ = c.integer(pt, 5); fields |= 2; }
        else if (id == 8 && pid == 3) { page.rows_ = c.integer(pt, 5); fields |= 4; }
        else if (id == 8 && pid == 4) { page.encoding_ = c.integer(pt, 5); fields |= 8; }
        else if (id == 8 && pid == 5) { page.def_bytes_ = c.integer(pt, 5); fields |= 16; }
        else if (id == 8 && pid == 6) { page.rep_bytes_ = c.integer(pt, 5); fields |= 32; }
        else if (id == 8 && pid == 7) { page.compressed_values_ = c.boolean(pt); }
        else { c.skip(pt); }
      });
      if ((id == 5 && fields != 15) || (id == 7 && fields != 3) || (id == 8 && fields != 63)) { c.bad(); }
    } else { c.skip(t); }
  });
  if (required != 7 || page.compressed_ < 0 || page.compressed_ > PAGE_LIMIT
      || page.uncompressed_ < 0 || page.uncompressed_ > PAGE_LIMIT) { c.bad(); }
  if (page.kind_ == 0 || page.kind_ == 2 || page.kind_ == 3) {
    if (page.values_ < 0 || page.subtype_ != (page.kind_ == 0 ? 5 : page.kind_ == 2 ? 7 : 8)) { c.bad(); }
  } else { c.unsupported(); }
}

// Convert epoch days with integer arithmetic, independent of libc TZ/locale.
int epoch_text(int64_t days, int64_t time_us, bool timestamp, std::string &out)
{
  if (days < -719162 || days > 2932896 || time_us < 0 || time_us >= 86400000000LL) { return OB_INVALID_ARGUMENT; }
  const int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t year = yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned day = doy - (153 * mp + 2) / 5 + 1;
  const unsigned month = mp < 10 ? mp + 3 : mp - 9;
  year += month <= 2;
  char text[40];
  int n = 0;
  if (timestamp) {
    n = snprintf(text, sizeof(text), "%04lld-%02u-%02u %02lld:%02lld:%02lld.%06lld",
                 static_cast<long long>(year), month, day,
                 static_cast<long long>(time_us / 3600000000LL),
                 static_cast<long long>((time_us / 60000000LL) % 60),
                 static_cast<long long>((time_us / 1000000LL) % 60),
                 static_cast<long long>(time_us % 1000000));
  } else { n = snprintf(text, sizeof(text), "%04lld-%02u-%02u", static_cast<long long>(year), month, day); }
  if (n <= 0 || n >= static_cast<int>(sizeof(text))) { return OB_INVALID_ARGUMENT; }
  out.assign(text, n); return OB_SUCCESS;
}

} // anonymous namespace
struct ParquetFileScanner::Impl
{
  struct ColumnState
  {
    int64_t index_ = -1;
    const Chunk *chunk_ = nullptr;
    int64_t cursor_ = 0;
    int64_t emitted_ = 0;
    int64_t page_rows_ = 0;
    int64_t page_used_ = 0;
    int64_t page_nonnull_ = 0;
    int64_t expected_nonnull_ = -1;
    int64_t value_start_ = 0;
    int64_t value_pos_ = 0;
    int64_t value_bits_ = 0;
    int64_t definition_start_ = 0;
    int64_t dictionary_count_ = 0;
    int encoding_ = -1;
    bool dictionary_seen_ = false;
    bool data_seen_ = false;
    bool legacy_definitions_ = false;
    std::vector<uint8_t> page_;
    std::vector<uint8_t> dictionary_;
    std::vector<uint32_t> dictionary_offsets_;
    Hybrid definitions_;
    Hybrid indexes_;
    ObObj value_;
  };

  ParquetFileScanner &owner_;
  std::vector<Schema> schema_;
  std::vector<RowGroup> groups_;
  std::vector<ColumnState> states_;
  std::vector<int64_t> output_states_;
  int64_t footer_offset_ = 0;
  int64_t row_ = 0;
  int64_t group_index_ = 0;
  int64_t group_row_ = 0;
  bool initialized_ = false;
  explicit Impl(ParquetFileScanner &owner) : owner_(owner) {}

  int footer()
  {
    int ret = OB_SUCCESS;
    const int64_t size = owner_.input_.get_fingerprint().size_;
    uint8_t magic[4]; uint8_t tail[8];
    if (size < 12) { return OB_INVALID_ARGUMENT; }
    if (OB_FAIL(owner_.read_at(0, 4, magic))) {}
    else if (OB_FAIL(owner_.read_at(size - 8, 8, tail))) {}
    else if (std::memcmp(magic, "PAR1", 4) != 0 || std::memcmp(tail + 4, "PAR1", 4) != 0) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      const int64_t length = little(tail, 4);
      if (length <= 0 || length > FOOTER_LIMIT || length > size - 12) { ret = OB_INVALID_ARGUMENT; }
      else {
        footer_offset_ = size - 8 - length;
        std::vector<uint8_t> bytes(length);
        if (OB_FAIL(owner_.read_at(footer_offset_, length, bytes.data()))) {}
        else {
          Compact c(bytes.data(), bytes.size());
          int required = 0;
          c.fields([&](int64_t id, uint8_t t) {
            if (id == 1) {
              required |= 1; const int64_t version = c.integer(t, 5);
              if (version != 1 && version != 2) { c.unsupported(); }
            } else if (id == 2) {
              required |= 2;
              const int64_t n = c.list(t, 12, ObFileScanLimits::MAX_COLUMNS + 1);
              if (n < 2) { c.bad(); }
              schema_.resize(n);
              for (int64_t i = 0; c.good() && i < n; ++i) { schema_element(c, schema_[i]); }
            } else if (id == 3) { required |= 4; owner_.row_count_ = c.integer(t, 6); }
            else if (id == 4) {
              required |= 8;
              const int64_t n = c.list(t, 12, ROW_GROUP_LIMIT);
              groups_.resize(n);
              for (int64_t i = 0; c.good() && i < n; ++i) { row_group(c, groups_[i]); }
            } else if (id == 8 || id == 9) { c.unsupported(); }
            else { c.skip(t); }
          });
          ret = c.ret_;
          owner_.error_location_.byte_offset_ = footer_offset_ + c.pos_;
          if (OB_SUCC(ret) && (required != 15 || c.pos_ != length || owner_.row_count_ < 0)) { ret = OB_INVALID_ARGUMENT; }
        }
      }
    }
    if (OB_SUCC(ret)) { ret = validate_metadata(); }
    return ret;
  }

  int validate_metadata()
  {
    int ret = OB_SUCCESS;
    if (schema_.empty() || schema_[0].children_ < 0) { return OB_INVALID_ARGUMENT; }
    if (schema_[0].physical_ != -1 || schema_[0].children_ != static_cast<int64_t>(schema_.size()) - 1) {
      return OB_NOT_SUPPORTED;
    }
    schema_.erase(schema_.begin());
    for (size_t i = 0; OB_SUCC(ret) && i < schema_.size(); ++i) {
      owner_.error_location_.column_ = i + 1;
      ret = validate_schema(schema_[i]);
      for (size_t j = 0; OB_SUCC(ret) && j < i; ++j) {
        if (schema_[j].name_ == schema_[i].name_) { ret = OB_INVALID_ARGUMENT; }
      }
    }
    int64_t rows = 0;
    // Metadata-only checks deliberately cover unselected columns too, while
    // page corruption is examined only if a column is actually projected.
    std::vector<std::pair<int64_t, int64_t> > ranges;
    for (size_t g = 0; OB_SUCC(ret) && g < groups_.size(); ++g) {
      RowGroup &group = groups_[g];
      if (group.chunks_.size() != schema_.size() || group.rows_ > INT64_MAX - rows) { ret = OB_INVALID_ARGUMENT; }
      else { rows += group.rows_; }
      for (size_t i = 0; OB_SUCC(ret) && i < group.chunks_.size(); ++i) {
        Chunk &ch = group.chunks_[i];
        owner_.error_location_.column_ = i + 1;
        if (ch.physical_ != schema_[i].physical_ || ch.name_ != schema_[i].name_
            || ch.values_ != group.rows_ || ch.compressed_ < 0 || ch.uncompressed_ < 0
            || ch.data_offset_ < 4 || (ch.dict_offset_ != -1 && ch.dict_offset_ < 4)) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          ch.start_ = ch.dict_offset_ >= 0 ? ch.dict_offset_ : ch.data_offset_;
          if (ch.start_ > ch.data_offset_ || ch.start_ > footer_offset_
              || ch.compressed_ > footer_offset_ - ch.start_) { ret = OB_INVALID_ARGUMENT; }
          else {
            ch.end_ = ch.start_ + ch.compressed_;
            if (ch.data_offset_ > ch.end_ || (ch.values_ > 0 && (ch.compressed_ == 0 || ch.data_offset_ == ch.end_))) {
              ret = OB_INVALID_ARGUMENT;
            } else if (ch.compressed_ != 0) { ranges.push_back(std::make_pair(ch.start_, ch.end_)); }
          }
        }
      }
    }
    if (OB_SUCC(ret) && rows != owner_.row_count_) { ret = OB_INVALID_ARGUMENT; }
    if (OB_SUCC(ret)) {
      std::sort(ranges.begin(), ranges.end());
      for (size_t i = 1; OB_SUCC(ret) && i < ranges.size(); ++i) {
        if (ranges[i].first < ranges[i - 1].second) { ret = OB_INVALID_ARGUMENT; }
      }
    }
    for (size_t i = 0; OB_SUCC(ret) && i < schema_.size(); ++i) {
      ObFileColumn column;
      column.name_.assign_ptr(schema_[i].name_.data(), static_cast<int32_t>(schema_[i].name_.size()));
      column.type_ = schema_[i].sql_type_; column.nullable_ = schema_[i].repetition_ == 1;
      ret = owner_.columns_.push_back(column);
    }
    return ret;
  }

  void reset()
  {
    states_.clear(); output_states_.clear();
    row_ = 0; group_index_ = 0; group_row_ = 0; initialized_ = false;
  }

  int initialize()
  {
    int ret = OB_SUCCESS;
    std::vector<int64_t> map(schema_.size(), -1);
    const int64_t count = owner_.has_projection_ ? owner_.projection_.count() : schema_.size();
    for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
      const int64_t index = owner_.has_projection_ ? owner_.projection_.at(i) : i;
      if (index < 0 || index >= static_cast<int64_t>(schema_.size())) { ret = OB_INVALID_ARGUMENT; }
      else {
        if (map[index] == -1) {
          map[index] = states_.size();
          ColumnState state; state.index_ = index; states_.push_back(std::move(state));
        }
        output_states_.push_back(map[index]);
      }
    }
    if (OB_SUCC(ret)) { initialized_ = true; start_group(); }
    return ret;
  }

  void start_group()
  {
    if (group_index_ < static_cast<int64_t>(groups_.size())) {
      for (size_t i = 0; i < states_.size(); ++i) {
        const int64_t index = states_[i].index_;
        states_[i] = ColumnState();
        states_[i].index_ = index;
        states_[i].chunk_ = &groups_[group_index_].chunks_[index];
        states_[i].cursor_ = states_[i].chunk_->start_;
      }
    }
  }

  int read_header(ColumnState &s, Page &page, int64_t &header_size)
  {
    const int64_t available = s.chunk_->end_ - s.cursor_;
    if (available <= 0) { return OB_INVALID_ARGUMENT; }
    uint8_t bytes[HEADER_LIMIT];
    int64_t want = std::min<int64_t>(64, available);
    while (true) {
      int ret = owner_.read_at(s.cursor_, want, bytes);
      if (ret != OB_SUCCESS) { return ret; }
      Compact c(bytes, want); page = Page(); page_header(c, page);
      if (c.good()) { header_size = c.pos_; return OB_SUCCESS; }
      if (!c.truncated_ || want == available || want == HEADER_LIMIT) { return c.ret_; }
      want = std::min<int64_t>(std::min<int64_t>(want * 2, HEADER_LIMIT), available);
    }
  }

  bool memory_available(const ColumnState &target, int64_t new_page, int64_t new_dictionary,
                        int64_t new_offsets, int64_t transient_bytes) const
  {
    const int64_t limit = 256 * 1024 * 1024;
    int64_t bytes = transient_bytes;
    bool available = transient_bytes >= 0 && transient_bytes <= limit;
    for (size_t i = 0; i < states_.size(); ++i) {
      const ColumnState &state = states_[i];
      int64_t page_capacity = static_cast<int64_t>(state.page_.capacity());
      int64_t dictionary_capacity = static_cast<int64_t>(state.dictionary_.capacity());
      int64_t offset_capacity = static_cast<int64_t>(state.dictionary_offsets_.capacity()) * sizeof(uint32_t);
      if (&state == &target) {
        // resize/assign may retain old capacity or grow geometrically. Reserve
        // the larger of the request and a doubled capacity before allocating.
        if (new_page > page_capacity) { page_capacity += std::max(new_page, page_capacity * 2); }
        if (new_dictionary > dictionary_capacity) {
          dictionary_capacity += std::max(new_dictionary, dictionary_capacity * 2);
        }
        if (new_offsets > offset_capacity) { offset_capacity += std::max(new_offsets, offset_capacity * 2); }
      }
      const int64_t values[] = {page_capacity, dictionary_capacity, offset_capacity};
      for (int64_t value : values) {
        if (value < 0 || value > limit - bytes) { return false; }
        bytes += value;
      }
    }
    return available && bytes <= limit;
  }

  bool memory_within_limit() const
  {
    return memory_available(ColumnState(), 0, 0, 0, 0);
  }

  int read_page(ColumnState &s)
  {
    int ret = OB_SUCCESS;
    const Schema &schema = schema_[s.index_];
    bool ready = false;
    while (OB_SUCC(ret) && !ready) {
      owner_.error_location_.byte_offset_ = s.cursor_;
      Page p; int64_t header_size = 0;
      if (OB_FAIL(read_header(s, p, header_size))) {}
      else if (header_size > s.chunk_->end_ - s.cursor_
          || p.compressed_ > s.chunk_->end_ - s.cursor_ - header_size) { ret = OB_INVALID_ARGUMENT; }
      else if (p.kind_ == 3 && (p.def_bytes_ < 0 || p.def_bytes_ > p.compressed_
          || p.def_bytes_ > p.uncompressed_ || (schema.repetition_ == 0 && p.def_bytes_ != 0))) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        const int64_t new_page = p.kind_ == 2 ? 0 : p.uncompressed_;
        const int64_t new_dictionary = p.kind_ == 2 ? p.uncompressed_ : 0;
        const int64_t new_offsets = p.kind_ == 2 && schema.physical_ == 6
            ? p.values_ * static_cast<int64_t>(sizeof(uint32_t)) : 0;
        const int64_t v2_values = p.kind_ == 3 ? p.uncompressed_ - p.def_bytes_ : 0;
        const int64_t transient_bytes = p.compressed_ + v2_values;
        if (!memory_available(s, new_page, new_dictionary, new_offsets, transient_bytes)) {
          ret = OB_INVALID_ARGUMENT;
        }
      }
      if (OB_SUCC(ret)) {
        const int64_t page_offset = s.cursor_;
        std::vector<uint8_t> compressed(p.compressed_);
        if (OB_FAIL(owner_.read_at(s.cursor_ + header_size, p.compressed_, compressed.data()))) {}
        else if (p.has_crc_ && page_crc(compressed.data(), compressed.size()) != p.crc_) { ret = OB_INVALID_ARGUMENT; }
        else {
          s.cursor_ += header_size + p.compressed_;
          if (p.kind_ == 2) {
            if (s.dictionary_seen_ || s.data_seen_ || page_offset != s.chunk_->dict_offset_
                || (p.encoding_ != 0 && p.encoding_ != 2) || p.values_ > PAGE_LIMIT) { ret = OB_INVALID_ARGUMENT; }
            else if (OB_FAIL(decompress(s.chunk_->codec_, compressed.data(), compressed.size(), p.uncompressed_, s.dictionary_))) {}
            else {
              s.dictionary_seen_ = true; s.dictionary_count_ = p.values_;
              ret = validate_dictionary(s);
            }
          } else {
            if ((!s.data_seen_ && page_offset != s.chunk_->data_offset_)
                || p.values_ <= 0 || p.values_ > s.chunk_->values_ - s.emitted_) { ret = OB_INVALID_ARGUMENT; }
            else if (p.encoding_ != 0 && p.encoding_ != 2 && p.encoding_ != 8
                && !(p.encoding_ == 3 && schema.physical_ == 0)) { ret = OB_NOT_SUPPORTED; }
            else if ((p.encoding_ == 2 || p.encoding_ == 8) && !s.dictionary_seen_) { ret = OB_INVALID_ARGUMENT; }
            else {
              s.data_seen_ = true;
              s.page_rows_ = p.values_; s.page_used_ = 0; s.page_nonnull_ = 0;
              s.expected_nonnull_ = -1; s.value_start_ = 0; s.value_pos_ = 0; s.value_bits_ = 0;
              s.encoding_ = p.encoding_; s.legacy_definitions_ = false;
              if (p.kind_ == 0) {
                if (p.rep_encoding_ != 3 && p.rep_encoding_ != 4) { ret = OB_NOT_SUPPORTED; }
                else if (p.def_encoding_ != 3 && p.def_encoding_ != 4) { ret = OB_NOT_SUPPORTED; }
                else if (OB_FAIL(decompress(s.chunk_->codec_, compressed.data(), compressed.size(), p.uncompressed_, s.page_))) {}
                else if (schema.repetition_ == 1) {
                  int64_t length = 0;
                  if (p.def_encoding_ == 3) {
                    if (s.page_.size() < 4) { ret = OB_INVALID_ARGUMENT; }
                    else { length = little(s.page_.data(), 4); s.value_start_ = 4; }
                  } else { length = (p.values_ + 7) / 8; s.legacy_definitions_ = true; }
                  if (OB_SUCC(ret)) {
                    if (length > static_cast<int64_t>(s.page_.size()) - s.value_start_) { ret = OB_INVALID_ARGUMENT; }
                    else {
                      s.definition_start_ = s.value_start_;
                      s.definitions_.reset(s.page_.data() + s.value_start_, length, 1);
                      s.value_start_ += length;
                    }
                  }
                }
              } else if (p.kind_ == 3) {
                if (p.rows_ != p.values_ || p.nulls_ < 0 || p.nulls_ > p.values_
                    || p.rep_bytes_ != 0 || p.def_bytes_ < 0 || p.def_bytes_ > p.compressed_
                    || p.def_bytes_ > p.uncompressed_
                    || (schema.repetition_ == 0 && (p.nulls_ != 0 || p.def_bytes_ != 0))) { ret = OB_INVALID_ARGUMENT; }
                else {
                  std::vector<uint8_t> values;
                  const uint8_t *value_data = compressed.empty() ? nullptr : compressed.data() + p.def_bytes_;
                  const int codec = p.compressed_values_ ? s.chunk_->codec_ : 0;
                  if (OB_FAIL(decompress(codec, value_data,
                      p.compressed_ - p.def_bytes_, p.uncompressed_ - p.def_bytes_, values))) {}
                  else {
                    s.page_.resize(p.uncompressed_);
                    if (p.def_bytes_ > 0) { std::memcpy(s.page_.data(), compressed.data(), p.def_bytes_); }
                    if (!values.empty()) { std::memcpy(s.page_.data() + p.def_bytes_, values.data(), values.size()); }
                    s.value_start_ = p.def_bytes_; s.expected_nonnull_ = p.values_ - p.nulls_;
                    if (schema.repetition_ == 1) { s.definitions_.reset(s.page_.data(), p.def_bytes_, 1); }
                  }
                }
              }
              if (OB_SUCC(ret)) {
                s.value_pos_ = s.value_start_;
                if (p.encoding_ == 2 || p.encoding_ == 8) {
                  // An empty index stream is legal for an entirely NULL page.
                  if (s.value_pos_ < static_cast<int64_t>(s.page_.size())) {
                    const int width = s.page_[s.value_pos_++];
                    if (width > 32) { ret = OB_INVALID_ARGUMENT; }
                    else { s.indexes_.reset(s.page_.data() + s.value_pos_, s.page_.size() - s.value_pos_, width); }
                  } else { s.indexes_.reset(s.page_.data() + s.value_pos_, 0, 0); }
                } else if (p.encoding_ == 3) {
                  if (static_cast<int64_t>(s.page_.size()) - s.value_pos_ < 4) { ret = OB_INVALID_ARGUMENT; }
                  else {
                    const int64_t length = little(s.page_.data() + s.value_pos_, 4); s.value_pos_ += 4;
                    if (length != static_cast<int64_t>(s.page_.size()) - s.value_pos_) { ret = OB_INVALID_ARGUMENT; }
                    else { s.indexes_.reset(s.page_.data() + s.value_pos_, length, 1); }
                  }
                }
                ready = OB_SUCC(ret);
              }
            }
          }
        }
      }
    }
    return ret;
  }

  int validate_dictionary(ColumnState &s)
  {
    const int physical = schema_[s.index_].physical_;
    const int64_t n = s.dictionary_count_;
    const int64_t size = s.dictionary_.size();
    if (physical == 6) {
      if (n > size / 4 || !memory_within_limit()) { return OB_INVALID_ARGUMENT; }
      s.dictionary_offsets_.resize(n);
      int64_t pos = 0;
      for (int64_t i = 0; i < n; ++i) {
        if (size - pos < 4) { return OB_INVALID_ARGUMENT; }
        s.dictionary_offsets_[i] = static_cast<uint32_t>(pos);
        const int64_t length = little(s.dictionary_.data() + pos, 4); pos += 4;
        if (length > FIELD_LIMIT || length > size - pos) { return OB_INVALID_ARGUMENT; }
        pos += length;
      }
      return pos == size ? OB_SUCCESS : OB_INVALID_ARGUMENT;
    }
    const int width = physical == 1 || physical == 4 ? 4 : 8;
    if (physical == 0) { return (n + 7) / 8 == size ? OB_SUCCESS : OB_INVALID_ARGUMENT; }
    return n <= PAGE_LIMIT / width && n * width == size ? OB_SUCCESS : OB_INVALID_ARGUMENT;
  }

  int decode_plain(const Schema &schema, const std::vector<uint8_t> &buffer,
                   int64_t &pos, int64_t &bits, ObObj &obj)
  {
    const int64_t size = buffer.size();
    const int physical = schema.physical_;
    if (physical == 0) {
      const int64_t byte = pos + bits / 8;
      if (byte >= size) { return OB_INVALID_ARGUMENT; }
      obj.set_tinyint((buffer[byte] >> (bits % 8)) & 1); ++bits;
      return OB_SUCCESS;
    }
    if (physical == 6) {
      if (size - pos < 4) { return OB_INVALID_ARGUMENT; }
      const int64_t length = little(buffer.data() + pos, 4); pos += 4;
      if (length > FIELD_LIMIT || length > size - pos) { return OB_INVALID_ARGUMENT; }
      const std::string text(reinterpret_cast<const char *>(buffer.data() + pos), length);
      if (!file_sql_valid_utf8(text)) { return OB_INVALID_ARGUMENT; }
      obj.set_varchar(reinterpret_cast<const char *>(buffer.data() + pos), static_cast<int32_t>(length));
      pos += length;
      return OB_SUCCESS;
    }
    const int width = physical == 1 || physical == 4 ? 4 : 8;
    if (width > size - pos) { return OB_INVALID_ARGUMENT; }
    const uint64_t raw = little(buffer.data() + pos, width); pos += width;
    if (physical == 4 || physical == 5) {
      double value = 0;
      if (physical == 4) { const uint32_t bits = raw; float f = 0; std::memcpy(&f, &bits, 4); value = f; }
      else { std::memcpy(&value, &raw, 8); }
      if (!std::isfinite(value)) { return OB_INVALID_ARGUMENT; }
      obj.set_double(value); return OB_SUCCESS;
    }
    int64_t value = physical == 1 ? static_cast<int64_t>(static_cast<int32_t>(raw)) : static_cast<int64_t>(raw);
    if (schema.sql_type_ == ObMySQLDateType || schema.sql_type_ == ObMySQLDateTimeType) {
      int64_t days = value, time_us = 0;
      const bool timestamp = schema.sql_type_ == ObMySQLDateTimeType;
      if (timestamp) {
        if (schema.timestamp_unit_ == 1) {
          if (value > INT64_MAX / 1000 || value < INT64_MIN / 1000) { return OB_INVALID_ARGUMENT; }
          value *= 1000;
        }
        days = value / 86400000000LL; time_us = value % 86400000000LL;
        if (time_us < 0) { --days; time_us += 86400000000LL; }
      }
      std::string text;
      int ret = epoch_text(days, time_us, timestamp, text);
      if (ret == OB_SUCCESS) { ret = file_sql_convert_scalar(text, schema.sql_type_, obj); }
      return ret;
    }
    obj.set_int(value); return OB_SUCCESS;
  }

  int finish_page(ColumnState &s)
  {
    const Schema &schema = schema_[s.index_];
    if (s.page_used_ == 0) { return OB_SUCCESS; }
    if (s.page_used_ != s.page_rows_ || (s.expected_nonnull_ >= 0 && s.page_nonnull_ != s.expected_nonnull_)) {
      return OB_INVALID_ARGUMENT;
    }
    if (schema.repetition_ == 1 && !s.legacy_definitions_ && !s.definitions_.finished()) { return OB_INVALID_ARGUMENT; }
    if (s.encoding_ == 2 || s.encoding_ == 8 || s.encoding_ == 3) {
      if (!s.indexes_.finished()) { return OB_INVALID_ARGUMENT; }
    } else if (schema.physical_ == 0) {
      if (s.value_pos_ + (s.value_bits_ + 7) / 8 != static_cast<int64_t>(s.page_.size())) { return OB_INVALID_ARGUMENT; }
    } else if (s.value_pos_ != static_cast<int64_t>(s.page_.size())) { return OB_INVALID_ARGUMENT; }
    return OB_SUCCESS;
  }

  int column_next(ColumnState &s)
  {
    int ret = OB_SUCCESS;
    const Schema &schema = schema_[s.index_];
    owner_.error_location_.column_ = s.index_ + 1;
    if (s.page_used_ == s.page_rows_) {
      if (OB_FAIL(finish_page(s))) {}
      else if (OB_FAIL(read_page(s))) {}
    }
    bool present = true;
    if (OB_SUCC(ret) && schema.repetition_ == 1) {
      uint32_t level = 0;
      if (s.legacy_definitions_) {
        const int64_t byte = s.definition_start_ + s.page_used_ / 8;
        level = (s.page_[byte] >> (7 - (s.page_used_ % 8))) & 1;
      } else { ret = s.definitions_.next(level); }
      if (OB_SUCC(ret) && level > 1) { ret = OB_INVALID_ARGUMENT; }
      present = level == 1;
    }
    if (OB_SUCC(ret)) {
      if (!present) { s.value_.set_null(); }
      else {
        ++s.page_nonnull_;
        if (s.encoding_ == 2 || s.encoding_ == 8) {
          uint32_t index = 0;
          if (OB_FAIL(s.indexes_.next(index))) {}
          else if (index >= static_cast<uint64_t>(s.dictionary_count_)) { ret = OB_INVALID_ARGUMENT; }
          else {
            int64_t pos = 0, bits = 0;
            if (schema.physical_ == 6) { pos = s.dictionary_offsets_[index]; }
            else if (schema.physical_ == 0) { bits = index; }
            else { pos = int64_t(index) * ((schema.physical_ == 1 || schema.physical_ == 4) ? 4 : 8); }
            ret = decode_plain(schema, s.dictionary_, pos, bits, s.value_);
          }
        } else if (s.encoding_ == 3) {
          uint32_t value = 0;
          if (OB_FAIL(s.indexes_.next(value))) {}
          else if (value > 1) { ret = OB_INVALID_ARGUMENT; }
          else { s.value_.set_tinyint(value); }
        } else { ret = decode_plain(schema, s.page_, s.value_pos_, s.value_bits_, s.value_); }
      }
    }
    if (OB_SUCC(ret)) {
      ++s.page_used_; ++s.emitted_;
      if (s.page_used_ == s.page_rows_) { ret = finish_page(s); }
      if (OB_SUCC(ret) && s.emitted_ == s.chunk_->values_ && s.cursor_ != s.chunk_->end_) { ret = OB_INVALID_ARGUMENT; }
    }
    return ret;
  }

  int next(ObIArray<ObObj> &row, bool &has_row)
  {
    int ret = OB_SUCCESS;
    row.reuse(); has_row = false;
    if (!initialized_ && OB_FAIL(initialize())) {}
    else if (row_ >= owner_.row_count_) {}
    else {
      while (group_index_ < static_cast<int64_t>(groups_.size()) && group_row_ >= groups_[group_index_].rows_) {
        ++group_index_; group_row_ = 0; start_group();
      }
      if (group_index_ >= static_cast<int64_t>(groups_.size())) { ret = OB_INVALID_ARGUMENT; }
      else {
        owner_.error_location_.line_ = row_ + 1;
        for (size_t i = 0; OB_SUCC(ret) && i < states_.size(); ++i) { ret = column_next(states_[i]); }
        for (size_t i = 0; OB_SUCC(ret) && i < output_states_.size(); ++i) { ret = row.push_back(states_[output_states_[i]].value_); }
        if (OB_SUCC(ret)) { ++row_; ++group_row_; has_row = true; }
      }
    }
    return ret;
  }
};

ParquetFileScanner::ParquetFileScanner() : row_count_(-1), impl_(nullptr) {}
ParquetFileScanner::~ParquetFileScanner() { close(); }

int ParquetFileScanner::read_at(int64_t offset, int64_t size, void *buffer)
{
  int ret = input_.validate_unchanged();
  const int64_t file_size = input_.get_fingerprint().size_;
  error_location_.byte_offset_ = offset;
  if (OB_SUCC(ret) && (offset < 0 || offset > file_size || size < 0 || size > file_size - offset
      || (size != 0 && buffer == nullptr))) { ret = common::OB_INVALID_ARGUMENT; }
  int64_t done = 0;
  while (OB_SUCC(ret) && done < size) {
    const ssize_t n = ::pread(input_.get_fd(), static_cast<char *>(buffer) + done,
                              static_cast<size_t>(size - done), offset + done);
    if (n < 0 && errno == EINTR) { continue; }
    if (n <= 0) { ret = common::OB_INVALID_ARGUMENT; }
    else { done += n; }
  }
  if (OB_SUCC(ret)) { ret = input_.validate_unchanged(); }
  return ret;
}

int ParquetFileScanner::report_error(int ret, const char *detail)
{
  if (ret == common::OB_INVALID_ARGUMENT) { LOG_USER_ERROR(OB_INVALID_ARGUMENT, detail); }
  else if (ret == common::OB_NOT_SUPPORTED) { LOG_USER_ERROR(OB_NOT_SUPPORTED, detail); }
  if (ret != common::OB_SUCCESS) {
    LOG_WARN("Parquet file scan failed", K(ret), K(detail), K(error_location_.line_),
             K(error_location_.column_), K(error_location_.byte_offset_));
  }
  return ret;
}

int ParquetFileScanner::open(const common::ObString &secure_file_priv, const common::ObString &path,
                             const ObFileFingerprint *expected_fingerprint)
{
  int ret = common::OB_SUCCESS;
  close();
  if (OB_FAIL(input_.open(secure_file_priv, path, expected_fingerprint))) {}
  else {
    impl_ = new (std::nothrow) Impl(*this);
    if (impl_ == nullptr) { ret = common::OB_ALLOCATE_MEMORY_FAILED; }
    else { ret = impl_->footer(); }
  }
  if (ret != common::OB_SUCCESS) {
    report_error(ret, ret == common::OB_NOT_SUPPORTED
        ? "Parquet schema, codec or encoding is not supported"
        : "Invalid or truncated Parquet file metadata");
    const ObFileErrorLocation error = error_location_;
    close(); error_location_ = error;
  } else { error_location_ = ObFileErrorLocation(); }
  return ret;
}

int ParquetFileScanner::next_row(common::ObIArray<common::ObObj> &row, bool &has_row)
{
  int ret = common::OB_SUCCESS;
  has_row = false;
  if (impl_ == nullptr) { ret = common::OB_NOT_INIT; }
  else if (OB_FAIL(input_.validate_unchanged())) {}
  else if (OB_FAIL(impl_->next(row, has_row))) {}
  else { ret = input_.validate_unchanged(); }
  return report_error(ret, ret == common::OB_NOT_SUPPORTED
      ? "Parquet data page encoding is not supported"
      : "Invalid Parquet data page, definition levels or dictionary values");
}

int ParquetFileScanner::rescan()
{
  int ret = common::OB_SUCCESS;
  if (impl_ == nullptr) { ret = common::OB_NOT_INIT; }
  else if (OB_FAIL(input_.validate_unchanged())) {}
  else { impl_->reset(); error_location_ = ObFileErrorLocation(); }
  return ret;
}

void ParquetFileScanner::close()
{
  delete impl_; impl_ = nullptr;
  input_.close(); columns_.reuse(); reset_projection();
  error_location_ = ObFileErrorLocation(); row_count_ = -1;
}

} // namespace sql
} // namespace oceanbase
