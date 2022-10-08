//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// extendible_hash_table.cpp
//
// Identification: src/container/hash/extendible_hash_table.cpp
//
// Copyright (c) 2015-2021, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "common/exception.h"
#include "common/logger.h"
#include "common/rid.h"
#include "container/hash/extendible_hash_table.h"

namespace bustub {

template <typename KeyType, typename ValueType, typename KeyComparator>
HASH_TABLE_TYPE::ExtendibleHashTable(const std::string &name, BufferPoolManager *buffer_pool_manager,
                                     const KeyComparator &comparator, HashFunction<KeyType> hash_fn)
    : buffer_pool_manager_(buffer_pool_manager), comparator_(comparator), hash_fn_(std::move(hash_fn)) {
  Page *page = buffer_pool_manager->NewPage(&directory_page_id_);
  HashTableDirectoryPage *dir_page = reinterpret_cast<HashTableDirectoryPage *>(page->GetData());
  dir_page->SetPageId(directory_page_id_);

  page_id_t bucket_page_id;
  buffer_pool_manager->NewPage(&bucket_page_id);  // only one page at first
  dir_page->SetBucketPageId(0, bucket_page_id);
  dir_page->SetLocalDepth(0, 0);

  assert(buffer_pool_manager->UnpinPage(directory_page_id_, true));
  assert(buffer_pool_manager->UnpinPage(bucket_page_id, false));
}

/*****************************************************************************
 * HELPERS
 *****************************************************************************/
/**
 * Hash - simple helper to downcast MurmurHash's 64-bit hash to 32-bit
 * for extendible hashing.
 *
 * @param key the key to hash
 * @return the downcast-ed 32-bit hash
 */
template <typename KeyType, typename ValueType, typename KeyComparator>
uint32_t HASH_TABLE_TYPE::Hash(KeyType key) {
  return static_cast<uint32_t>(hash_fn_.GetHash(key));
}

template <typename KeyType, typename ValueType, typename KeyComparator>
inline uint32_t HASH_TABLE_TYPE::KeyToDirectoryIndex(KeyType key, HashTableDirectoryPage *dir_page) {
  uint32_t hash = Hash(key);
  uint32_t mask = dir_page->GetGlobalDepthMask();
  uint32_t bucket_id = hash & mask;
  return bucket_id;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
inline uint32_t HASH_TABLE_TYPE::KeyToPageId(KeyType key, HashTableDirectoryPage *dir_page) {
  uint32_t bucket_id = KeyToDirectoryIndex(key, dir_page);
  uint32_t page_id = dir_page->GetBucketPageId(bucket_id);
  return page_id;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
HashTableDirectoryPage *HASH_TABLE_TYPE::FetchDirectoryPage() {
  Page *page = buffer_pool_manager_->FetchPage(directory_page_id_);
  HashTableDirectoryPage *dir_page = reinterpret_cast<HashTableDirectoryPage *>(page->GetData());
  return dir_page;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
HASH_TABLE_BUCKET_TYPE *HASH_TABLE_TYPE::FetchBucketPage(page_id_t bucket_page_id) {
  Page *page = buffer_pool_manager_->FetchPage(bucket_page_id);
  HASH_TABLE_BUCKET_TYPE *bucket_page = reinterpret_cast<HASH_TABLE_BUCKET_TYPE *>(page->GetData());
  return bucket_page;
}

/*****************************************************************************
 * SEARCH
 *****************************************************************************/
template <typename KeyType, typename ValueType, typename KeyComparator>
bool HASH_TABLE_TYPE::GetValue(Transaction *transaction, const KeyType &key, std::vector<ValueType> *result) {
  table_latch_.WLock();

  auto dir_page = FetchDirectoryPage();
  auto bkt_page_id = KeyToPageId(key, dir_page);
  auto bkt_page = FetchBucketPage(bkt_page_id);
  auto success = bkt_page->GetValue(key, comparator_, result);

  assert(buffer_pool_manager_->UnpinPage(directory_page_id_, false));
  assert(buffer_pool_manager_->UnpinPage(bkt_page_id, false));
  table_latch_.WUnlock();
  return success;
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
template <typename KeyType, typename ValueType, typename KeyComparator>
bool HASH_TABLE_TYPE::Insert(Transaction *transaction, const KeyType &key, const ValueType &value) {
  table_latch_.WLock();

  auto dir_page = FetchDirectoryPage();
  auto bkt_page_id = KeyToPageId(key, dir_page);
  auto bkt_page = FetchBucketPage(bkt_page_id);

  // case 1: bucket splitting, and potentially directory growing
  if (bkt_page->IsFull()) {
    assert(buffer_pool_manager_->UnpinPage(directory_page_id_, false));
    assert(buffer_pool_manager_->UnpinPage(bkt_page_id, false));
    table_latch_.WUnlock();
    return SplitInsert(transaction, key, value);  // leave everything to SplitInsert
  }

  // case 2: no bucket splitting
  auto success = bkt_page->Insert(key, value, comparator_);

  assert(buffer_pool_manager_->UnpinPage(directory_page_id_, false));
  assert(buffer_pool_manager_->UnpinPage(bkt_page_id, true));
  table_latch_.WUnlock();
  return success;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool HASH_TABLE_TYPE::SplitInsert(Transaction *transaction, const KeyType &key, const ValueType &value) {
  table_latch_.WLock();

  auto dir_page = FetchDirectoryPage();
  auto bkt_id = KeyToDirectoryIndex(key, dir_page);
  auto bkt_page_id = static_cast<int>(KeyToPageId(key, dir_page));
  auto bkt_page = FetchBucketPage(bkt_page_id);
  assert(bkt_page->IsFull());  // NOTE: Might wrong, concurrency issue

  // Increment local depth
  dir_page->IncrLocalDepth(bkt_id);

  // Check if hash table has to grow directory?
  if (dir_page->GetLocalDepth(bkt_id) > dir_page->GetGlobalDepth()) {
    dir_page->IncrGlobalDepth();
  }

  // Initialize image bucket page
  auto img_id = dir_page->GetSplitImageIndex(bkt_id);
  page_id_t img_page_id = dir_page->GetBucketPageId(img_id);
  assert(img_page_id == bkt_page_id);
  auto img_page = reinterpret_cast<HASH_TABLE_BUCKET_TYPE *>(buffer_pool_manager_->NewPage(&img_page_id)->GetData());
  assert(img_page_id != bkt_page_id);
  dir_page->SetBucketPageId(img_id, img_page_id);  // add to dir_page
  assert(dir_page->GetLocalDepth(bkt_id) == dir_page->GetLocalDepth(img_id));

  // Rehash all existing k/v pairs
  auto kv_pairs = bkt_page->GetKVPairs();
  bkt_page->Reset();
  for (auto it = kv_pairs->begin(); it != kv_pairs->end(); it++) {
    auto k2v = it.base();
    uint32_t tgt_id = Hash(k2v->first) & dir_page->GetLocalDepthMask(img_id);
    assert(tgt_id == img_id || tgt_id == bkt_id);
    if (tgt_id == img_id) {
      assert(img_page->Insert(k2v->first, k2v->second, comparator_));
    } else {
      assert(bkt_page->Insert(k2v->first, k2v->second, comparator_));
    }
  }

  // Re-organize previous buckets
  uint32_t local_depth = dir_page->GetLocalDepth(img_id);
  uint32_t diff = 1 << local_depth;
  for (uint32_t i = bkt_id - diff; i >= diff; i -= diff) {
    dir_page->SetBucketPageId(i, bkt_page_id);
    dir_page->SetLocalDepth(i, local_depth);
  }
  for (uint32_t i = bkt_id + diff; i < dir_page->Size(); i += diff) {
    dir_page->SetBucketPageId(i, bkt_page_id);
    dir_page->SetLocalDepth(i, local_depth);
  }
  for (uint32_t i = img_id - diff; i >= diff; i -= diff) {
    dir_page->SetBucketPageId(i, img_page_id);
    dir_page->SetLocalDepth(i, local_depth);
  }
  for (uint32_t i = img_id + diff; i < dir_page->Size(); i += diff) {
    dir_page->SetBucketPageId(i, img_page_id);
    dir_page->SetLocalDepth(i, local_depth);
  }

  assert(buffer_pool_manager_->UnpinPage(directory_page_id_, true));
  assert(buffer_pool_manager_->UnpinPage(bkt_page_id, true));
  assert(buffer_pool_manager_->UnpinPage(img_page_id, true));
  table_latch_.WUnlock();
  return Insert(transaction, key, value);
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
template <typename KeyType, typename ValueType, typename KeyComparator>
bool HASH_TABLE_TYPE::Remove(Transaction *transaction, const KeyType &key, const ValueType &value) {
  table_latch_.WLock();

  auto dir_page = FetchDirectoryPage();
  auto bkt_page_id = KeyToPageId(key, dir_page);
  auto bkt_page = FetchBucketPage(bkt_page_id);
  auto success = bkt_page->Remove(key, value, comparator_);

  assert(buffer_pool_manager_->UnpinPage(directory_page_id_, false));
  assert(buffer_pool_manager_->UnpinPage(bkt_page_id, true));
  table_latch_.WUnlock();

  // case 1: Merging must be attempted when a bucket becomes empty
  if (success && bkt_page->IsEmpty()) {
    Merge(transaction, key, value);  // leave everything to Merge
  }

  // case 2: no bucket merging
  return success;
}

/*****************************************************************************
 * MERGE
 *****************************************************************************/
template <typename KeyType, typename ValueType, typename KeyComparator>
void HASH_TABLE_TYPE::Merge(Transaction *transaction, const KeyType &key, const ValueType &value) {
  table_latch_.WLock();

  auto dir_page = FetchDirectoryPage();
  auto bkt_page_id = static_cast<int>(KeyToPageId(key, dir_page));
  auto bkt_page = FetchBucketPage(bkt_page_id);

  auto bkt_id = KeyToDirectoryIndex(key, dir_page);
  auto img_id = dir_page->GetSplitImageIndex(bkt_id);
  if (!bkt_page->IsEmpty() ||                                                // premise 1
      dir_page->GetLocalDepth(bkt_id) == 0 ||                                // premise 2
      dir_page->GetLocalDepth(bkt_id) != dir_page->GetLocalDepth(img_id)) {  // premise 3
    assert(buffer_pool_manager_->UnpinPage(directory_page_id_, false));
    assert(buffer_pool_manager_->UnpinPage(bkt_page_id, false));
    table_latch_.WUnlock();
    return;
  }

  // Delete the empty page
  assert(buffer_pool_manager_->UnpinPage(bkt_page_id, false));
  assert(buffer_pool_manager_->DeletePage(bkt_page_id));

  // Points to image page
  auto img_page_id = dir_page->GetBucketPageId(img_id);
  dir_page->SetBucketPageId(bkt_id, img_page_id);

  // Decrement local depth
  dir_page->DecrLocalDepth(bkt_id);
  dir_page->DecrLocalDepth(img_id);

  assert(dir_page->GetBucketPageId(bkt_id) == dir_page->GetBucketPageId(img_id) &&
         dir_page->GetLocalDepth(bkt_id) == dir_page->GetLocalDepth(img_id));

  // Re-organize previous buckets
  uint32_t local_depth = dir_page->GetLocalDepth(img_id);
  uint32_t diff = 1 << local_depth;
  for (uint32_t i = bkt_id - diff; i >= diff; i -= diff) {
    dir_page->SetBucketPageId(i, img_page_id);
    dir_page->SetLocalDepth(i, local_depth);
  }
  for (uint32_t i = bkt_id + diff; i < dir_page->Size(); i += diff) {
    dir_page->SetBucketPageId(i, img_page_id);
    dir_page->SetLocalDepth(i, local_depth);
  }

  while (dir_page->CanShrink()) {
    dir_page->DecrGlobalDepth();
  }

  assert(buffer_pool_manager_->UnpinPage(directory_page_id_, true));
  table_latch_.WUnlock();
}

/*****************************************************************************
 * GETGLOBALDEPTH - DO NOT TOUCH
 *****************************************************************************/
template <typename KeyType, typename ValueType, typename KeyComparator>
uint32_t HASH_TABLE_TYPE::GetGlobalDepth() {
  table_latch_.RLock();
  HashTableDirectoryPage *dir_page = FetchDirectoryPage();
  uint32_t global_depth = dir_page->GetGlobalDepth();
  assert(buffer_pool_manager_->UnpinPage(directory_page_id_, false, nullptr));
  table_latch_.RUnlock();
  return global_depth;
}

/*****************************************************************************
 * VERIFY INTEGRITY - DO NOT TOUCH
 *****************************************************************************/
template <typename KeyType, typename ValueType, typename KeyComparator>
void HASH_TABLE_TYPE::VerifyIntegrity() {
  table_latch_.RLock();
  HashTableDirectoryPage *dir_page = FetchDirectoryPage();
  dir_page->VerifyIntegrity();
  assert(buffer_pool_manager_->UnpinPage(directory_page_id_, false, nullptr));
  table_latch_.RUnlock();
}

/*****************************************************************************
 * TEMPLATE DEFINITIONS - DO NOT TOUCH
 *****************************************************************************/
template class ExtendibleHashTable<int, int, IntComparator>;

template class ExtendibleHashTable<GenericKey<4>, RID, GenericComparator<4>>;
template class ExtendibleHashTable<GenericKey<8>, RID, GenericComparator<8>>;
template class ExtendibleHashTable<GenericKey<16>, RID, GenericComparator<16>>;
template class ExtendibleHashTable<GenericKey<32>, RID, GenericComparator<32>>;
template class ExtendibleHashTable<GenericKey<64>, RID, GenericComparator<64>>;

}  // namespace bustub