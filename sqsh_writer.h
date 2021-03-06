/*
Copyright (C) 2016, 2017, 2018  Charles Cagle

This file is part of archive2sqfs.

archive2sqfs is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, version 3.

archive2sqfs is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with archive2sqfs.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef LSL_SQSH_WRITER_H
#define LSL_SQSH_WRITER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "adler_wrapper.h"
#include "block_report.h"
#include "bounded_work_queue.h"
#include "compressor.h"
#include "fragment_entry.h"
#include "fstream_util.h"
#include "metadata_writer.h"
#include "pending_write.h"
#include "sqsh_defs.h"

#define SQFS_BLOCK_LOG_DEFAULT 17

struct fragment_index
{
  uint32_t fragment = SQFS_FRAGMENT_NONE;
  uint32_t offset;
};

struct sqfs_super
{
  uint16_t block_log = SQFS_BLOCK_LOG_DEFAULT;
  uint16_t flags = 0;

  meta_address root_inode;
  uint64_t bytes_used = 0;
  uint64_t id_table_start = 0;
  uint64_t xattr_table_start = SQFS_TABLE_NOT_PRESENT;
  uint64_t inode_table_start = 0;
  uint64_t directory_table_start = 0;
  uint64_t fragment_table_start = 0;
  uint64_t lookup_table_start = SQFS_TABLE_NOT_PRESENT;
};

static inline auto thread_count()
{
  auto tc = std::thread::hardware_concurrency();
  return 2 + (tc > 0 ? tc : 4);
}

struct sqsh_writer
{
  // owned by client thread.
  uint32_t next_inode = 1;
  struct sqfs_super super;

  bool const single_threaded;
  bool const dedup_enabled;
  std::thread thread;
  std::string const outfilepath;

  std::unique_ptr<compressor> const comp;
  metadata_writer dentry_writer;
  metadata_writer inode_writer;

  std::vector<char> current_block;
  std::vector<char> current_fragment;
  uint32_t fragment_count = 0;

  std::unordered_map<uint32_t, uint16_t> ids;
  std::unordered_map<uint16_t, uint32_t> rids;

  std::unordered_map<uint32_t, fragment_index> fragment_indices;
  std::unordered_map<uint32_t, adler_wrapper> fragmented_checksums;
  std::unordered_map<decltype(fragmented_checksums[0].sum),
                     std::vector<uint32_t>>
      fragmented_duplicates;

  // owned by writer thread.
  std::unordered_map<uint32_t, block_report> reports;

  std::unordered_map<uint32_t, adler_wrapper> blocked_checksums;
  std::unordered_map<decltype(blocked_checksums[0].sum),
                     std::vector<uint32_t>>
      blocked_duplicates;

  // shared by client and writer threads.
  std::fstream outfile;
  std::mutex outfile_mutex;

  bounded_work_queue<std::unique_ptr<pending_write>> writer_queue;
  std::atomic<bool> writer_failed{false};

  std::vector<fragment_entry> fragments;
  std::mutex fragments_mutex;
  std::condition_variable fragments_cv;

  uint32_t next_inode_number() { return next_inode++; }
  std::size_t block_size() const { return std::size_t(1) << super.block_log; }

  void write_header();
  uint16_t id_lookup(uint32_t);
  optional<fragment_index> dedup_fragment_index(uint32_t);
  bool dedup_fragment(uint32_t);
  void put_fragment(uint32_t);
  void flush_fragment();
  void write_tables();
  void enqueue_block(uint32_t);
  void enqueue_dedup(uint32_t);
  void enqueue_fragment();
  void enqueue(std::unique_ptr<pending_write> &&);
  void writer_thread();
  bool finish_data();
  void push_fragment_entry(fragment_entry);
  optional<fragment_entry> get_fragment_entry(uint32_t);
  std::vector<char> read_bytes(decltype(outfile)::pos_type, std::streamsize);
  void drop_bytes(decltype(outfile)::off_type);

  auto launch_policy()
  {
    return single_threaded ? std::launch::deferred : std::launch::async;
  }

  template <typename C> auto write_bytes(C const & c)
  {
    std::lock_guard<decltype(outfile_mutex)> lock(outfile_mutex);
    auto const tell = outfile.tellp();
    outfile.write(c.data(), c.size());
    return tell;
  }

  sqsh_writer(std::string path, int blog, std::string comptype,
              bool disable_threads = false, bool enable_dedup = false)
      : single_threaded(disable_threads), dedup_enabled(enable_dedup),
        outfilepath(path), comp(get_compressor_for(comptype)),
        dentry_writer(*comp), inode_writer(*comp),
        outfile(path, std::ios_base::binary | std::ios_base::in |
                          std::ios_base::out | std::ios_base::trunc),
        writer_queue(thread_count())
  {
    super.block_log = blog;
    outfile.exceptions(std::ios_base::failbit);
    outfile.seekp(SQFS_SUPER_SIZE);
    if (!single_threaded)
      thread = std::thread(&sqsh_writer::writer_thread, this);
  }

  ~sqsh_writer()
  {
    if (thread.joinable())
      finish_data();
  }
};

#endif
