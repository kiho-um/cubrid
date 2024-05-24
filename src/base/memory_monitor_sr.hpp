/*
 *
 * Copyright 2016 CUBRID Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

/*
 * memory_monitor_sr.hpp - Declaration of APIs and structures, classes
 *                         for memory monitoring module
 */

#ifndef _MEMORY_MONITOR_SR_HPP_
#define _MEMORY_MONITOR_SR_HPP_

#include <malloc.h>
#include <unordered_map>
#include <shared_mutex>
#include <mutex>
#include <string>
#include <atomic>

#include "concurrent_unordered_map.h"
#include "memory_monitor_common.hpp"

#ifndef HAVE_USR_INCLUDE_MALLOC_H
#define HAVE_USR_INCLUDE_MALLOC_H
#endif
// IMPORTANT!!
// This meta size is related with allocation byte align
// Don't adjust it freely
// 4 byte tag + 8 byte size + 4 byte line + 4 byte checksum + 12 byte padding
#define MMON_ALLOC_META_SIZE 16

typedef struct mmon_metainfo   // 16 bytes
{
  uint64_t size;
  int tag_id;
  int magic_number;
} MMON_METAINFO;

namespace cubmem
{
  extern std::atomic<uint64_t> m_stat_map[10000];
  class memory_monitor
  {
    public:
      memory_monitor (const char *server_name);
      ~memory_monitor () {}
      size_t get_alloc_size (char *ptr);
      inline void make_tag_name (char *buf, const char *file, const int line);
      inline void add_stat (char *ptr, size_t size, const char *file, const int line);
      inline void sub_stat (char *ptr);
      void aggregate_server_info (MMON_SERVER_INFO &server_info);
      void finalize_dump ();

    private:
      tbb::concurrent_unordered_map<std::string, int> m_tag_map; // tag name <-> tag id
      std::string m_server_name;
      std::atomic<uint64_t> m_total_mem_usage;
      std::atomic<int> m_meta_alloc_count;
      int m_target_pos;
      const int m_magic_number;
  };
  extern memory_monitor *mmon_Gl;

  inline void memory_monitor::make_tag_name (char *buf, const char *file, const int line)
  {
    sprintf (buf, "%s:%d", file + m_target_pos, line);
  }

  inline void memory_monitor::add_stat (char *ptr, size_t size, const char *file, const int line)
  {
    char tag_name[255];
    char *meta_ptr = NULL;
    MMON_METAINFO *metainfo;

    assert (size >= 0);

    meta_ptr = ptr + (uint64_t) size - MMON_ALLOC_META_SIZE;
    metainfo = (MMON_METAINFO *)meta_ptr;

    metainfo->size = (uint64_t) size;
    m_total_mem_usage += size;

    make_tag_name (tag_name, file, line);
retry:
    const auto tag_search = m_tag_map.find (tag_name);
    if (tag_search != m_tag_map.end ())
      {
	metainfo->tag_id = tag_search->second;
	m_stat_map[metainfo->tag_id] += metainfo->size;
      }
    else
      {
	std::pair<tbb::concurrent_unordered_map<std::string, int>::iterator, bool> tag_map_success;
	metainfo->tag_id = m_tag_map.size ();
	// tag is start with 0
	std::pair <std::string, int> tag_map_entry (tag_name, metainfo->tag_id);
	tag_map_success = m_tag_map.insert (tag_map_entry);
	if (!tag_map_success.second)
	  {
	    goto retry;
	  }
	m_stat_map[metainfo->tag_id] += metainfo->size;
      }

    // put meta info into the alloced chunk
    metainfo->magic_number = m_magic_number;
    m_meta_alloc_count++;
  }

  inline void memory_monitor::sub_stat (char *ptr)
  {
#if defined(WINDOWS)
    size_t alloc_size = 0;
    assert (false);
#else
    size_t alloc_size = malloc_usable_size ((void *)ptr);
#endif // !WINDOWS

    assert (ptr != NULL);

    if (alloc_size >= MMON_ALLOC_META_SIZE)
      {
	char *meta_ptr = ptr + alloc_size - MMON_ALLOC_META_SIZE;
	MMON_METAINFO *metainfo = (MMON_METAINFO *)meta_ptr;

	if (metainfo->magic_number == m_magic_number)
	  {
	    assert ((metainfo->tag_id >= 0 && metainfo->tag_id <= m_tag_map.size()));
	    assert (m_stat_map[metainfo->tag_id].load() >= metainfo->size);
	    assert (m_total_mem_usage >= metainfo->size);

	    m_total_mem_usage -= metainfo->size;
	    m_stat_map[metainfo->tag_id] -= metainfo->size;

	    //memset (meta_ptr, 0, MMON_ALLOC_META_SIZE);
	    metainfo->magic_number = 0;
	    m_meta_alloc_count--;
	    assert (m_meta_alloc_count >= 0);
	  }
      }
  }


} //namespace cubmem

int mmon_initialize (const char *server_name);
void mmon_finalize ();
size_t mmon_get_alloc_size (char *ptr);
void mmon_aggregate_server_info (MMON_SERVER_INFO &server_info);

inline bool mmon_is_mem_tracked ()
{
  return (cubmem::mmon_Gl != nullptr);
}

inline void mmon_add_stat (char *ptr, size_t size, const char *file, const int line)
{
  if (mmon_is_mem_tracked ())
    {
      cubmem::mmon_Gl->add_stat (ptr, size, file, line);
    }
}

inline void mmon_sub_stat (char *ptr)
{
  if (mmon_is_mem_tracked ())
    {
      cubmem::mmon_Gl->sub_stat (ptr);
    }
}
#endif // _MEMORY_MONITOR_SR_HPP_
