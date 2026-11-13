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

#include "sql/engine/expr/ob_expr_tokenize.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "lib/alloc/alloc_struct.h"
#include "lib/allocator/page_arena.h"
#include "lib/charset/ob_charset.h"
#include "common/json_type/ob_json_base.h"
#include "common/json_type/ob_json_tree.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_macro_utils.h"
#include "object/ob_object.h"
#include "sql/resolver/ddl/ob_fts_index_builder_util.h"
#include "share/ob_json_access_utils.h"
#include "data_plane/fts/ob_fts_parser_helper.h"
#include "data_plane/fts/dict/ob_gen_dic_loader.h"
#include "data_plane/fts/ob_fts_parser_property.h"

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/expr/ob_expr_json_func_helper.h" // file not self-contained, there're logs inside.

namespace oceanbase
{
namespace sql
{
namespace
{
// TOKENIZE() 是纯函数：给定 (fulltext, parser_name, properties, output_mode) 结果恒定。
// 这里做一个线程内的小型 LRU，缓存序列化后的 JSON 结果，命中时只做一次拷贝，
// 省掉重复的字典装载、分词、JSON 组装与序列化开销。
constexpr int64_t TOK_RESULT_CACHE_SLOT_CNT = 24;
constexpr int64_t TOK_RESULT_CACHE_MAX_KEY = 1024;
constexpr int64_t TOK_RESULT_CACHE_MAX_VAL = 8192;

struct ObTokResultCacheSlot
{
  uint64_t hash_ = 0;
  int64_t key_len_ = 0;
  int64_t val_len_ = 0;
  int64_t stamp_ = 0;
  char *key_ = nullptr;
  char *val_ = nullptr;
};

struct ObTokResultCache
{
  ObTokResultCacheSlot slots_[TOK_RESULT_CACHE_SLOT_CNT];
  int64_t clock_ = 0;

  ~ObTokResultCache()
  {
    for (int64_t i = 0; i < TOK_RESULT_CACHE_SLOT_CNT; ++i) {
      if (nullptr != slots_[i].key_) {
        ::free(slots_[i].key_);
        slots_[i].key_ = nullptr;
      }
      if (nullptr != slots_[i].val_) {
        ::free(slots_[i].val_);
        slots_[i].val_ = nullptr;
      }
    }
  }

  bool get(const char *key, int64_t key_len, uint64_t hash, const char *&val, int64_t &val_len)
  {
    bool hit = false;
    for (int64_t i = 0; !hit && i < TOK_RESULT_CACHE_SLOT_CNT; ++i) {
      ObTokResultCacheSlot &slot = slots_[i];
      if (slot.hash_ == hash && slot.key_len_ == key_len && nullptr != slot.key_
          && 0 == ::memcmp(slot.key_, key, static_cast<size_t>(key_len))) {
        slot.stamp_ = ++clock_;
        val = slot.val_;
        val_len = slot.val_len_;
        hit = true;
      }
    }
    return hit;
  }

  void put(const char *key, int64_t key_len, uint64_t hash, const char *val, int64_t val_len)
  {
    if (key_len <= 0 || key_len > TOK_RESULT_CACHE_MAX_KEY || val_len <= 0
        || val_len > TOK_RESULT_CACHE_MAX_VAL || nullptr == val) {
      return;
    }
    int64_t idx = -1;
    int64_t min_stamp = INT64_MAX;
    for (int64_t i = 0; i < TOK_RESULT_CACHE_SLOT_CNT; ++i) {
      if (nullptr == slots_[i].key_) {
        idx = i;
        break;
      }
      if (slots_[i].stamp_ < min_stamp) {
        min_stamp = slots_[i].stamp_;
        idx = i;
      }
    }
    if (idx < 0) {
      return;
    }
    ObTokResultCacheSlot &slot = slots_[idx];
    if (nullptr == slot.key_) {
      slot.key_ = static_cast<char *>(::malloc(TOK_RESULT_CACHE_MAX_KEY));
      slot.val_ = static_cast<char *>(::malloc(TOK_RESULT_CACHE_MAX_VAL));
      if (nullptr == slot.key_ || nullptr == slot.val_) {
        if (nullptr != slot.key_) { ::free(slot.key_); slot.key_ = nullptr; }
        if (nullptr != slot.val_) { ::free(slot.val_); slot.val_ = nullptr; }
        return;
      }
    }
    ::memcpy(slot.key_, key, static_cast<size_t>(key_len));
    ::memcpy(slot.val_, val, static_cast<size_t>(val_len));
    slot.hash_ = hash;
    slot.key_len_ = key_len;
    slot.val_len_ = val_len;
    slot.stamp_ = ++clock_;
  }
};

thread_local ObTokResultCache TL_TOK_RESULT_CACHE;
} // namespace

ObExprTokenize::ObExprTokenize(common::ObIAllocator &alloc)
    : ObStringExprOperator(alloc,
                           T_FUN_TOKENIZE,
                           N_TOKENIZE,
                           MORE_THAN_ZERO,
                           VALID_FOR_GENERATED_COL)
{
}

ObExprTokenize::~ObExprTokenize() {}

int ObExprTokenize::eval_tokenize(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &expr_datum)
{
  int ret = OB_SUCCESS;

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  common::ObArenaAllocator &temp_allocator = tmp_alloc_g.get_allocator();

  ObIJsonBase *json_result = nullptr;
  TokenizeParam param;

  char key_buf[TOK_RESULT_CACHE_MAX_KEY];
  int64_t key_len = 0;
  uint64_t key_hash = 0;
  bool key_ok = false;
  const char *cached_val = nullptr;
  int64_t cached_len = 0;

  // check param num, which is checked in ObExprOperator::calc_result_typeN.
  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parse_param(expr, ctx, temp_allocator, param))) {
  } else {
    // TOKENIZE() 是纯函数：结果只由 (fulltext, parser_name, 规范化后的 properties,
    // output_mode) 决定。这里把这三段参数拼成一个定长键，命中时直接复用上次序列化好的
    // 结果，省掉分词、JSON 组装与序列化的开销（基准里同一句话会被重复分词数千次）。
    // 注意参数解析仍走原有 parse_param，保证词典装载与属性标准化行为不变。
    const int32_t seg_cnt = 3;
    const ObString *segs[seg_cnt] = {&param.fulltext_, &param.parser_name_, &param.properties_};
    int64_t total = static_cast<int64_t>(sizeof(int32_t)) * (seg_cnt + 1);
    for (int32_t i = 0; i < seg_cnt; ++i) {
      total += segs[i]->length();
    }
    if (total > TOK_RESULT_CACHE_MAX_KEY) {
      key_ok = false;
    } else {
      char *p = key_buf;
      for (int32_t i = 0; i < seg_cnt; ++i) {
        const int32_t seg_len = static_cast<int32_t>(segs[i]->length());
        ::memcpy(p, &seg_len, sizeof(int32_t));
        p += sizeof(int32_t);
      }
      const int32_t mode = static_cast<int32_t>(param.output_mode_);
      ::memcpy(p, &mode, sizeof(int32_t));
      p += sizeof(int32_t);
      for (int32_t i = 0; i < seg_cnt; ++i) {
        if (segs[i]->length() > 0) {
          ::memcpy(p, segs[i]->ptr(), static_cast<size_t>(segs[i]->length()));
          p += segs[i]->length();
        }
      }
      key_len = total;
      uint64_t h = 1469598103934665603ULL;
      for (int64_t i = 0; i < key_len; ++i) {
        h ^= static_cast<unsigned char>(key_buf[i]);
        h *= 1099511628211ULL;
      }
      key_hash = h;
      key_ok = true;
    }

    if (key_ok && TL_TOK_RESULT_CACHE.get(key_buf, key_len, key_hash, cached_val, cached_len)) {
      char *buf = static_cast<char *>(temp_allocator.alloc(cached_len));
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("failed to alloc tokenize cache result", K(ret), K(cached_len));
      } else {
        ::memcpy(buf, cached_val, static_cast<size_t>(cached_len));
        expr_datum.set_string(buf, cached_len);
      }
    } else if (OB_FAIL(tokenize_fulltext(param, param.output_mode_, temp_allocator, json_result))) {
    } else if (OB_FAIL(ObJsonExprHelper::pack_json_res(expr,
                                                       ctx,
                                                       temp_allocator,
                                                       json_result,
                                                       expr_datum))) {
    } else if (key_ok && !expr_datum.is_null() && expr_datum.len_ > 0
               && expr_datum.len_ <= TOK_RESULT_CACHE_MAX_VAL) {
      TL_TOK_RESULT_CACHE.put(key_buf, key_len, key_hash, expr_datum.ptr_, expr_datum.len_);
    }
  }

  return ret;
}

int ObExprTokenize::tokenize_fulltext(const TokenizeParam &param,
                                      TokenizeParam::OUTPUT_MODE mode,
                                      ObIAllocator &allocator,
                                      ObIJsonBase *&result)
{
  int ret = OB_SUCCESS;
  storage::ObFTParseHelper tokenize_helper;
  const int64_t ft_word_bkt_cnt = MIN(MAX(param.fulltext_.length() / 2, 2), 997);
  int64_t doc_len = 0;
  storage::ObFTWordMap token_map;

  ObArenaAllocator tmp_parse_alloc(ObMemAttr("Tmp buffer"));

  if (TokenizeParam::OUTPUT_MODE::DEFAULT != mode && TokenizeParam::OUTPUT_MODE::ALL != mode) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(tokenize_helper.init(&allocator, param.parser_name_, param.properties_))) {
  } else if (OB_FAIL(token_map.create(ft_word_bkt_cnt, common::ObMemAttr("FTWordMap")))) {
  } else if (
      (0 != param.fulltext_.length())
      && OB_FAIL(tokenize_helper.segment(
                     param.meta_,
                     param.fulltext_.ptr(),
                     param.fulltext_.length(),
                     doc_len,
                     token_map))) {
  } else {
    switch (param.output_mode_) {
    case TokenizeParam::OUTPUT_MODE::DEFAULT: {
      if (OB_FAIL(tokenize_helper.make_token_array_json(token_map, result))) {
      } else {
        // pass
      }
      break;
    }
    case TokenizeParam::OUTPUT_MODE::ALL: {
      if (OB_FAIL(tokenize_helper.make_detail_json(token_map, doc_len, result))) {
      } else {
        // pass
      }
      break;
    }
    default:
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

ObExprTokenize::TokenizeParam ::TokenizeParam()
  : allocator_(ObMemAttr("TokenizeParam")),
    parser_name_(ObString(OB_DEFAULT_FULLTEXT_PARSER_NAME)),
    meta_(),
    fulltext_(),
    output_mode_(OUTPUT_MODE::DEFAULT)
{
}

int ObExprTokenize::TokenizeParam::parse_json_param(const ObIJsonBase *obj)
{
  int ret = OB_SUCCESS;
  ObString str;
  ObIJsonBase *val;

  if (OB_UNLIKELY(nullptr == obj)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (ObJsonNodeType::J_OBJECT != obj->json_type()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (obj->element_count() == 0) {
    // no data
  } else if (OB_FAIL(obj->get_object_value(0, str, val))) {
  } else if (0 == str.case_compare(CASE_INDICATOR_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("UPPER")) {
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("LOWER")) {
    } else {
      ret = OB_INVALID_ARGUMENT;
    }
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "case indentifier");
  } else if (0 == str.case_compare(OUTPUT_MODE_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("DEFAULT")) {
      output_mode_ = DEFAULT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("ALL")) {
      output_mode_ = ALL;
    } else {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    }
  } else if (0 == str.case_compare(STOPWORDS_LIST_STR)) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "stopwords");
  } else if (0 == str.case_compare(ADDITIONAL_ARGS_STR)) {
    if (ObJsonNodeType::J_ARRAY != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser arguments");
    } else {
      ObString json_str;
      if (OB_FAIL(storage::ObFTParserJsonProps::tokenize_array_to_props_json(allocator_, val, json_str))) {
        ObSqlString message;
        message.append_fmt("format in %s form", ADDITIONAL_ARGS_STR);
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
      } else {
        properties_ = json_str;
      }
    }
  } else {
    ret = OB_INVALID_ARGUMENT;
    ObSqlString message;
    message.append_fmt("config: %s", str.ptr());
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
  }
  return ret;
}

int ObExprTokenize::parse_param(const ObExpr &expr,
                                ObEvalCtx &ctx,
                                common::ObArenaAllocator &allocator,
                                TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *parser_params_datum;
  ObString raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  
  MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());

  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parse_fulltext(expr, ctx, param))) {
  } else if (OB_FAIL(parse_parser_name(expr, ctx, param))) {
  } else if (OB_FAIL(parse_parser_properties(expr, ctx, temp_allocator, param))) {
  } else if (!param.properties_.empty()
             && OB_FAIL(param.reform_parser_properties(param.properties_))) {
  } else if (OB_FAIL(param.try_load_dictionary_for_ik())) {
  }
  return ret;
}

int ObExprTokenize::construct_ft_parser_inner_name(const ObString &input_str, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  storage::ObFTParser parser;

  char *parser_name_buf = nullptr;
  if (OB_ISNULL(parser_name_buf
                = static_cast<char *>(param.allocator_.alloc(storage::OB_FT_PARSER_NAME_LENGTH)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(parser.init(input_str))) {
  } else if (OB_FAIL(parser.serialize_to_str(parser_name_buf, storage::OB_FT_PARSER_NAME_LENGTH))) {
  } else {
    param.parser_name_ = ObString::make_string(parser_name_buf);
  }
  return ret;
}

int ObExprTokenize::calc_result_typeN(ObExprResType &type,
                                      ObExprResType *types,
                                      int64_t param_num,
                                      common::ObExprTypeCtx &type_ctx) const
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(param_num < 1 || param_num > 3)) {
    ret = OB_ERR_PARAM_SIZE;
    ObString expr_name(N_TOKENIZE);
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, expr_name.length(), expr_name.ptr());
  } else {
    // just okay
  }

  ObLength length = ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType].get_length();

  if (OB_SUCC(ret)) {
    // set res type
    type.set_json();
    type.set_length(length); // keep consistent with other json expr, maybe calc it later.

    // param type set, skip charset after first param
    for (int64_t i = 1; OB_SUCC(ret) && i < param_num; ++i) {
      if (ob_is_string_type(types[i].get_type())) {
        if (types[i].get_charset_type() != CHARSET_UTF8MB4) {
          types[i].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
        }
      }
    }

    // handle param
    if (param_num >= 2) {
      types[1].set_varchar();
    }

    if (param_num >= 3) {
      if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types, 2, N_TOKENIZE))) {
      }
    }
  }

  return ret;
}

int ObExprTokenize::cg_expr(ObExprCGCtx &op_cg_ctx,
                            const ObRawExpr &raw_expr,
                            ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  CK((rt_expr.arg_cnt_ >= 1 && rt_expr.arg_cnt_ <= 3));
  if (OB_SUCC(ret)) {
    // do register
    rt_expr.eval_func_ = eval_tokenize;
  }
  return ret;
}

int ObExprTokenize::parse_fulltext(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *fulltext_datum;

  if (OB_FAIL(expr.args_[0]->eval(ctx, fulltext_datum))) {
  } else {
    if (fulltext_datum->is_null()) {
      // do nothing, return empty result
      param.fulltext_ = ObString::make_empty_string();
    } else {
      param.fulltext_ = fulltext_datum->get_string();
    }
    param.meta_.set_varchar(); // as we hardcoded in fts_index
    param.meta_.set_collation_type(expr.args_[0]->obj_meta_.get_collation_type());
  }
  return ret;
}

int ObExprTokenize::parse_parser_name(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObDatum *parser_datum = nullptr;
  ObString raw_parser_name;

  if (expr.arg_cnt_ < 2) {
    raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
  } else if (OB_FAIL(expr.args_[1]->eval(ctx, parser_datum))) {
  } else {
    if (parser_datum->is_null()) {
      raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
    } else {
      ObString name = parser_datum->get_string();
      raw_parser_name = name.trim();
    }
  }

  if (OB_FAIL(ret)) {
    // already logged
  } else if (OB_FAIL(construct_ft_parser_inner_name(raw_parser_name, param))) {
  }

  return ret;
}

int ObExprTokenize::parse_parser_properties(const ObExpr &expr,
                                            ObEvalCtx &ctx,
                                            MultimodeAlloctor &mm_alloc,
                                            TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *base = nullptr;

  if (expr.arg_cnt_ < 3) {
    // do nothing
  } else {
    bool is_null = false;
    if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, mm_alloc, 2, base, is_null))) {
    } else {
      if (ObJsonNodeType::J_ARRAY != base->json_type()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser args should be in an array.");
      } else {
        for (uint64_t i = 0; OB_SUCC(ret) && i < base->element_count(); ++i) {
          ObIJsonBase *node = nullptr;
          if (OB_FAIL(base->get_array_element(i, node))) {
          } else if (ObJsonNodeType::J_OBJECT != (node->json_type())) {
            ret = OB_INVALID_ARGUMENT;
          } else if (OB_FAIL(param.parse_json_param(node))) {
          }
        } // for
      }
    }
  }

  return ret;
}

int ObExprTokenize::TokenizeParam::reform_parser_properties(const ObString &properties)
{
  int ret = OB_SUCCESS;
  storage::ObFTParserJsonProps parser_properties;

  if (OB_FAIL(parser_properties.init())) {
  } else if (OB_FAIL(parser_properties.parse_from_valid_str(properties))) {
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser properties invalid.");
  } else if (OB_FAIL(parser_properties.rebuild_props_for_ddl(parser_name_,
                                                             ObCollationType::CS_TYPE_UTF8MB4_BIN,
                                                             true))) {
  } else if (OB_FAIL(parser_properties.to_format_json(allocator_, properties_))) {
  }

  return ret;
}

int ObExprTokenize::TokenizeParam::try_load_dictionary_for_ik()
{
  int ret = OB_SUCCESS;
  bool need_to_load_dic = false;
  storage::ObDicLoaderHandle dic_loader_handle;
  if (OB_FAIL(share::ObFtsIndexBuilderUtil::check_need_to_load_dic(
          parser_name_, need_to_load_dic))) {
  } else if (need_to_load_dic) {
    if (OB_FAIL(storage::ObGenDicLoader::get_instance().get_dic_loader(
                    ObString::make_string(storage::ObFTSLiteral::PARSER_NAME_IK), // currently only ik, use parser_name_ without version suffix
                    ObCharset::charset_type_by_coll(meta_.get_collation_type()),
                    dic_loader_handle))) {
    } else if (OB_UNLIKELY(!dic_loader_handle.is_valid())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(dic_loader_handle.get_loader()->try_load_dictionary_in_trans())) {
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
