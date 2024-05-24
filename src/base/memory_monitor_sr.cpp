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
 * memory_monitor_sr.cpp - Implementation of memory monitor module
 */

#include <stdio.h>
#include <cstring>
#include <algorithm>

#include "error_manager.h"
#include "system_parameter.h"
#include "db.h"
#include "memory_monitor_sr.hpp"

bool is_mem_tracked = false;

namespace cubmem
{
  std::atomic<uint64_t> m_stat_map[10000] = {};
  memory_monitor *mmon_Gl = nullptr;

  memory_monitor::memory_monitor (const char *server_name)
    : m_tag_map {4096},
      m_server_name {server_name},
      m_magic_number {*reinterpret_cast <const int *> ("MMON")},
      m_total_mem_usage {0},
      m_meta_alloc_count {0}
  {
    std::string filecopy (__FILE__);
#if defined(WINDOWS)
    std::string target ("");
    assert (false);
#else
    std::string target ("/src/");
#endif // !WINDOWS

    size_t pos = filecopy.rfind (target);
    if (pos != std::string::npos)
      {
	m_target_pos = pos + target.length();
      }
    else
      {
	m_target_pos = 0;
      }
  }

  size_t memory_monitor::get_alloc_size (char *ptr)
  {
#if defined(WINDOWS)
    size_t alloc_size = 0;
    assert (false);
#else
    size_t alloc_size = malloc_usable_size ((void *)ptr);
#endif // !WINDOWS

    if (alloc_size <= MMON_ALLOC_META_SIZE)
      {
	return alloc_size;
      }

    char *meta_ptr = ptr + alloc_size - MMON_ALLOC_META_SIZE;
    MMON_METAINFO *metainfo = (MMON_METAINFO *) meta_ptr;

    if (metainfo->magic_number == m_magic_number)
      {
	alloc_size = (size_t) metainfo->size - MMON_ALLOC_META_SIZE;
      }

    return alloc_size;
  }

  void memory_monitor::aggregate_server_info (MMON_SERVER_INFO &server_info)
  {
    strncpy (server_info.server_name, m_server_name.c_str (), m_server_name.size () + 1);
    server_info.total_mem_usage = m_total_mem_usage.load ();
    server_info.monitoring_meta_usage = m_meta_alloc_count * MMON_ALLOC_META_SIZE;
    server_info.num_stat = m_tag_map.size ();

    for (auto it = m_tag_map.begin (); it != m_tag_map.end (); ++it)
      {
	server_info.stat_info.push_back (std::make_pair (it->first, m_stat_map[it->second].load ()));
      }

    const auto &comp = [] (const auto& stat_pair1, const auto& stat_pair2)
    {
      return stat_pair1.second > stat_pair2.second;
    };
    std::sort (server_info.stat_info.begin (), server_info.stat_info.end (), comp);
  }

  void memory_monitor::finalize_dump ()
  {
    double mem_usage_ratio = 0.0;
    FILE *outfile_fp = fopen ("finalize_dump.txt", "w+");
    MMON_SERVER_INFO server_info;

    auto MMON_CONVERT_TO_KB_SIZE = [] (uint64_t size)
    {
      return ((size) / 1024);
    };

    aggregate_server_info (server_info);

    fprintf (outfile_fp, "====================cubrid memmon====================\n");
    fprintf (outfile_fp, "Server Name: %s\n", server_info.server_name);
    fprintf (outfile_fp, "Total Memory Usage(KB): %lu\n\n", MMON_CONVERT_TO_KB_SIZE (server_info.total_mem_usage));
    fprintf (outfile_fp, "-----------------------------------------------------\n");

    fprintf (outfile_fp, "\t%-100s | %17s(%s)\n", "File Name", "Memory Usage", "Ratio");

    for (const auto &s_info : server_info.stat_info)
      {
	if (server_info.total_mem_usage != 0)
	  {
	    mem_usage_ratio = s_info.second / (double) server_info.total_mem_usage;
	    mem_usage_ratio *= 100;
	  }
	fprintf (outfile_fp, "\t%-100s | %17lu(%3d%%)\n",s_info.first.c_str (), MMON_CONVERT_TO_KB_SIZE (s_info.second),
		 (int)mem_usage_ratio);
      }
    fprintf (outfile_fp, "-----------------------------------------------------\n");
    fflush (outfile_fp);
    fclose (outfile_fp);
  }
} // namespace cubmem

using namespace cubmem;

int mmon_initialize (const char *server_name)
{
  int error = NO_ERROR;

  assert (server_name != NULL);
  assert (mmon_Gl == nullptr);

  if (prm_get_bool_value (PRM_ID_MEMORY_MONITORING))
    {
      mmon_Gl = new (std::nothrow) memory_monitor (server_name);

      if (mmon_Gl == nullptr)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1, sizeof (memory_monitor));
	  error = ER_OUT_OF_VIRTUAL_MEMORY;
	  return error;
	}
    }
  return error;
}

void mmon_finalize ()
{
  if (mmon_is_mem_tracked ())
    {
#if !defined (NDEBUG)
      mmon_Gl->finalize_dump ();
#endif
      delete mmon_Gl;
      mmon_Gl = nullptr;
    }
}

size_t mmon_get_alloc_size (char *ptr)
{
  if (mmon_is_mem_tracked ())
    {
      return mmon_Gl->get_alloc_size (ptr);
    }
  // unreachable
  return 0;
}

void mmon_aggregate_server_info (MMON_SERVER_INFO &server_info)
{
  if (mmon_is_mem_tracked ())
    {
      mmon_Gl->aggregate_server_info (server_info);
    }
}
