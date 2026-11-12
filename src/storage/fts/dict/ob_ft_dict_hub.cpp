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

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/dict/ob_ft_dict_hub.h"

#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "storage/fts/dict/ob_ft_cache_container.h"
#include "storage/fts/dict/ob_ft_dict_def.h"
#include "storage/fts/dict/ob_ft_range_dict.h"
namespace oceanbase
{
namespace storage
{
int ObFTDictHub::init()
{
  static constexpr int K_MAX_DICT_BUCKET = 128; // for now, only built-in dicts.
  int ret = OB_SUCCESS;
  if (OB_FAIL(dict_map_.create(K_MAX_DICT_BUCKET, "dict_map"))) {
  } else if (OB_FAIL(rw_dict_lock_.init(K_MAX_DICT_BUCKET))) {
  } else {
    is_inited_ = true;
  }
  return ret;
};

int ObFTDictHub::destroy()
{
  int ret = OB_SUCCESS;
  {
    lib::ObMutexGuard guard(shared_dict_mutex_);
    for (int64_t i = 0; i < MAX_SHARED_DICT_CNT; ++i) {
      if (OB_NOT_NULL(shared_dicts_[i])) {
        ATOMIC_STORE(&shared_dicts_[i], nullptr);
      }
      if (OB_NOT_NULL(shared_containers_[i])) {
        OB_DELETEx(ObFTCacheRangeContainer, &shared_dict_alloc_, shared_containers_[i]);
        shared_containers_[i] = nullptr;
      }
    }
    shared_dict_alloc_.reset();
  }
  is_inited_ = false;
  return ret;
}

int ObFTDictHub::get_shared_dict(const ObFTDictDesc &desc, ObIFTDict *&dict)
{
  int ret = OB_SUCCESS;
  dict = nullptr;
  const int64_t idx = static_cast<int64_t>(desc.type_);
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(idx <= 0 || idx >= MAX_SHARED_DICT_CNT)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid dict type for shared dict", K(ret), K(idx), K(desc.type_));
  } else if (OB_NOT_NULL(dict = ATOMIC_LOAD(&shared_dicts_[idx]))) {
    // fast path: the shared dict is already built and published.
  } else {
    lib::ObMutexGuard guard(shared_dict_mutex_);
    if (OB_NOT_NULL(dict = ATOMIC_LOAD(&shared_dicts_[idx]))) {
      // built by another thread while waiting for the mutex.
    } else if (OB_FAIL(build_shared_dict(desc, dict))) {
      LOG_WARN("failed to build shared dict", K(ret), K(desc.type_));
    }
  }
  return ret;
}

int ObFTDictHub::build_shared_dict(const ObFTDictDesc &desc, ObIFTDict *&dict)
{
  int ret = OB_SUCCESS;
  ObFTCacheRangeContainer *container = nullptr;
  ObIFTDict *new_dict = nullptr;
  const int64_t idx = static_cast<int64_t>(desc.type_);

  if (OB_ISNULL(container = OB_NEWx(ObFTCacheRangeContainer, &shared_dict_alloc_, shared_dict_alloc_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(load_cache(desc, *container))) {
    if (OB_ENTRY_NOT_EXIST != ret) {
      LOG_WARN("failed to load dict cache", K(ret), K(desc.type_));
    } else {
      ret = OB_SUCCESS;
      if (OB_FAIL(build_cache(desc, *container))) {
        LOG_WARN("failed to build dict cache", K(ret), K(desc.type_));
      }
    }
  }

  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(new_dict = OB_NEWx(ObFTRangeDict, &shared_dict_alloc_, shared_dict_alloc_, container, desc))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(new_dict->init())) {
    LOG_WARN("failed to init range dict", K(ret), K(desc.type_));
  } else {
    // publish only after the dict is fully initialized; readers use ATOMIC_LOAD.
    shared_containers_[idx] = container;
    ATOMIC_STORE(&shared_dicts_[idx], new_dict);
    dict = new_dict;
  }

  if (OB_FAIL(ret)) {
    if (OB_NOT_NULL(new_dict)) {
      OB_DELETEx(ObIFTDict, &shared_dict_alloc_, new_dict);
      new_dict = nullptr;
    }
    if (OB_NOT_NULL(container)) {
      OB_DELETEx(ObFTCacheRangeContainer, &shared_dict_alloc_, container);
      container = nullptr;
    }
  }
  return ret;
}
int ObFTDictHub::build_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  ObFTDictInfo info;
  container.reset();

  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    ObBucketHashWLockGuard guard(rw_dict_lock_, key.hash());

    // try if valid with no recursive lock
    if (OB_FAIL(get_dict_info(key, info))) {
      if (OB_HASH_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
        ret = OB_ENTRY_NOT_EXIST;
      } else {
      }
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
      } else {
      }
    }

    if (OB_FAIL(ret)) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        if (OB_FAIL(ObFTRangeDict::build_cache_from_ik_dict(desc, container))) {
        } else if (FALSE_IT(info.range_count_ = container.get_handles().size())) {
        } else if (OB_FAIL(put_dict_info(key, info))) {
        }
      }
    }
  }
  return ret;
}

int ObFTDictHub::load_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfo info;
  container.reset();
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    {
      ObBucketHashRLockGuard guard(rw_dict_lock_, key.hash());
      if (OB_FAIL(get_dict_info(key, info))) {
        if (OB_HASH_NOT_EXIST == ret) {
          // dict not exist, make new one, by caller
          ret = OB_ENTRY_NOT_EXIST;
        } else {
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
      } else {
      }
    }
  }

  return ret;
}


int ObFTDictHub::get_dict_info(const ObFTDictInfoKey &key, ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;

  if (OB_FAIL(dict_map_.get_refactored(key, info))) {
    if (OB_HASH_NOT_EXIST != ret) {
    }
  }

  return ret;
}

int ObFTDictHub::put_dict_info(const ObFTDictInfoKey &key, const ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;
  const int cover_exist_flag = 1;
  if (OB_FAIL(dict_map_.set_refactored(key, info, cover_exist_flag))) {
  }

  return ret;
}
} //  namespace storage
} //  namespace oceanbase
