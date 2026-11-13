/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _OCEANBASE_STORAGE_FTS_OB_IK_FT_PARSER_H_
#define _OCEANBASE_STORAGE_FTS_OB_IK_FT_PARSER_H_

#include "lib/allocator/ob_allocator.h"
#include "storage/fts/dict/ob_ft_cache_container.h"
#include "storage/fts/dict/ob_ft_dict.h"
#include "storage/fts/dict/ob_ft_dict_def.h"
#include "storage/fts/ik/ob_ik_processor.h"
#include "data_plane/fts/ob_fts_parser.h"

#include <cstdint>
namespace oceanbase
{
namespace storage
{
class ObFTDictHub;

// Token cache slot; defined in the parser's translation unit.
class ObIKTokenCacheEntry;

class ObIKFTParser final : public ObITokenIterator
{
public:
  ObIKFTParser(ObIAllocator &allocator, ObFTDictHub *hub)
      : allocator_(allocator),
        is_inited_(false),
        coll_type_(ObCollationType::CS_TYPE_INVALID),
        ctx_(nullptr),
        hub_(hub),
        cache_entry_(nullptr),
        segmenter_cnt_(0),
        dict_main_(nullptr),
        dict_quan_(nullptr),
        dict_stop_(nullptr)
  {
    for (int64_t i = 0; i < SEGMENTER_COUNT; ++i) {
      seg_arr_[i] = nullptr;
    }
  }

  virtual ~ObIKFTParser() { reset(); }

  int init(const ObFTParserParam &param);

  int get_next_token(const char *&word,
                     int64_t &word_len,
                     int64_t &char_cnt,
                     int64_t &word_freq) override;

  // Non-null enables recording the produced token stream into the given cache
  // slot.  The slot is only published once the whole text has been consumed.
  void set_cache_entry(ObIKTokenCacheEntry *entry) { cache_entry_ = entry; }

  VIRTUAL_TO_STRING_KV(K(is_inited_));

private:
  int produce();

  int process_next_batch();

  int process_one_char(TokenizeContext &ctx,
                       const char *ch,
                       const uint8_t char_len,
                       const ObFTCharUtil::CharType type);

private:
  int init_dict(const ObFTParserParam &param);

  int init_segmenter(const ObFTParserParam &param);

  int init_ctx(const ObFTParserParam &param);

  void reset();

  void record_token(const char *word, const int64_t word_len, const int64_t char_cnt);

private:
  static constexpr int SEGMENT_LIMIT = 1000;
  // IK parser always owns at most 4 segmenters (letter/quantifier/cjk/surrogate).
  static constexpr int64_t SEGMENTER_COUNT = 4;
  ObIAllocator &allocator_;
  bool is_inited_;

  ObCollationType coll_type_;
  TokenizeContext *ctx_;
  ObFTDictHub *hub_;
  // The segmenters are owned through this fixed size array.  They used to live
  // in an ObList and were copied into the array afterwards, so every parser
  // used to pay four list node allocations just to iterate four pointers.
  ObIIKProcessor *seg_arr_[SEGMENTER_COUNT];
  int64_t segmenter_cnt_;
  // Per-thread cache slot owned by ObIKFTParserDesc::segment(), may be null.
  ObIKTokenCacheEntry *cache_entry_;

  // Borrowed from ObFTDictHub, shared by all parsers and must not be freed here.
  ObIFTDict *dict_main_;
  ObIFTDict *dict_quan_;
  ObIFTDict *dict_stop_;

  DISABLE_COPY_ASSIGN(ObIKFTParser);
};

class ObIKFTParserDesc final : public ObIFTParserDesc
{
public:
  ObIKFTParserDesc() {}
  virtual ~ObIKFTParserDesc() = default;
  virtual int segment(ObFTParserParam *param, ObITokenIterator *&iter) const override;
  virtual void free_token_iter(ObFTParserParam *param, ObITokenIterator *&iter) const override;
  virtual int get_add_word_flag(ObAddWordFlag &flag) const override;
};

} //  namespace storage
} //  namespace oceanbase

#endif // _OCEANBASE_STORAGE_FTS_OB_IK_FT_PARSER_H_
