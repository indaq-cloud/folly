/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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

#pragma once

#include <climits> // for PATH_MAX
#include <cstring>
#include <forward_list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <boost/container/flat_map.hpp>
#include <boost/intrusive/avl_set.hpp>
#include <boost/intrusive/list.hpp>
#include <boost/operators.hpp>
#include <glog/logging.h>

#include <folly/Range.h>
#include <folly/experimental/symbolizer/Elf.h>
#include <folly/hash/Hash.h>
#include <folly/memory/ReentrantAllocator.h>

namespace folly {
namespace symbolizer {

/**
 * Number of ELF files loaded by the dynamic loader.
 */
size_t countLoadedElfFiles();

class ElfCacheBase {
 public:
  virtual std::shared_ptr<ElfFile> getFile(StringPiece path) = 0;
  virtual ~ElfCacheBase() {}
};

/**
 * Cache ELF files. Async-signal-safe: does memory allocation via mmap.
 *
 * Not MT-safe. May not be used concurrently from multiple threads.
 */
class SignalSafeElfCache : public ElfCacheBase {
 public:
  std::shared_ptr<ElfFile> getFile(StringPiece path) override;

  using Path = std::basic_string< //
      char,
      std::char_traits<char>,
      reentrant_allocator<char>>;

  struct Entry : boost::intrusive::avl_set_base_hook<> {
    Path path;
    std::shared_ptr<ElfFile> file;
    bool init = false;

    explicit Entry(StringPiece p, reentrant_allocator<char> alloc) noexcept
        : path{p.data(), p.size(), alloc},
          file{std::allocate_shared<ElfFile>(alloc)} {}
    Entry(Entry const&) = delete;
    Entry& operator=(Entry const& that) = delete;

    friend bool operator<(Entry const& a, Entry const& b) noexcept {
      return a.path < b.path;
    }
  };

  struct State {
    reentrant_allocator<void> alloc{
        reentrant_allocator_options().block_size_lg(16).large_size_lg(12)};
    std::forward_list<Entry, reentrant_allocator<Entry>> list{alloc};
    // note: map entry dtors check that they have already been unlinked
    boost::intrusive::avl_set<Entry> map; // must follow list
  };
  Optional<State> state_;
};

/**
 * General-purpose ELF file cache.
 *
 * LRU of given capacity. MT-safe (uses locking). Not async-signal-safe.
 */
class ElfCache : public ElfCacheBase {
 public:
  explicit ElfCache(size_t capacity);

  std::shared_ptr<ElfFile> getFile(StringPiece path) override;

 private:
  std::mutex mutex_;

  typedef boost::intrusive::list_member_hook<> LruLink;

  struct Entry {
    std::string path;
    ElfFile file;
    LruLink lruLink;
  };

  static std::shared_ptr<ElfFile> filePtr(const std::shared_ptr<Entry>& e);

  size_t capacity_;
  std::unordered_map<StringPiece, std::shared_ptr<Entry>, Hash> files_;

  typedef boost::intrusive::list<
      Entry,
      boost::intrusive::member_hook<Entry, LruLink, &Entry::lruLink>,
      boost::intrusive::constant_time_size<false>>
      LruList;
  LruList lruList_;
};
} // namespace symbolizer
} // namespace folly
