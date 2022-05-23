/*
 * Copyright 2008 Search Solution Corporation
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

#ifndef _ATOMIC_REPLICATION_HELPER_HPP_
#define _ATOMIC_REPLICATION_HELPER_HPP_

#include <map>
#include <vector>
#include <set>

#include "log_lsa.hpp"
#include "log_record.hpp"
#include "log_recovery_redo.hpp"
#include "page_buffer.h"
#include "thread_entry.hpp"
#include "vpid_utilities.hpp"

namespace cublog
{
  /*
   * Atomic replication helper class that holds all atomic logs mapped by the transaction id
   */
  class atomic_replication_helper
  {
    public:
      atomic_replication_helper () = default;

      atomic_replication_helper (const atomic_replication_helper &) = delete;
      atomic_replication_helper (atomic_replication_helper &&) = delete;

      ~atomic_replication_helper () = default;

      atomic_replication_helper &operator= (const atomic_replication_helper &) = delete;
      atomic_replication_helper &operator= (atomic_replication_helper &&) = delete;

      template <typename T>
      int add_atomic_replication_unit (THREAD_ENTRY *thread_p, TRANID tranid, log_lsa record_lsa, LOG_RCVINDEX rcvindex,
				       VPID vpid, log_rv_redo_context &redo_context, const log_rv_redo_rec_info<T> &record_info);
      void unfix_atomic_replication_sequence (THREAD_ENTRY *thread_p, TRANID tranid);
      bool is_part_of_atomic_replication (TRANID tranid) const;
#if !defined (NDEBUG)
      bool check_for_page_validity (VPID vpid, TRANID tranid) const;
#endif

    private:

      class atomic_replication_sequence
      {
	public:
	  atomic_replication_sequence () = default;

	  atomic_replication_sequence (const atomic_replication_sequence &) = delete;
	  atomic_replication_sequence (atomic_replication_sequence &&) = delete;

	  ~atomic_replication_sequence () = default;

	  atomic_replication_sequence &operator= (const atomic_replication_sequence &) = delete;
	  atomic_replication_sequence &operator= (atomic_replication_sequence &&) = delete;

	  void unfix_sequence (THREAD_ENTRY *thread_p);
	private:
	  template <typename T>
	  int add_atomic_replication_unit (THREAD_ENTRY *thread_p, log_lsa record_lsa, LOG_RCVINDEX rcvindex, VPID vpid,
					   log_rv_redo_context &redo_context, const log_rv_redo_rec_info<T> &record_info);

	  /*
	   * Atomic replication unit holds the log record information necessary for recovery redo
	   */
	  class atomic_replication_unit
	  {
	    public:
	      atomic_replication_unit () = delete;
	      atomic_replication_unit (log_lsa lsa, VPID vpid, LOG_RCVINDEX rcvindex);

	      atomic_replication_unit (const atomic_replication_unit &) = delete;
	      atomic_replication_unit (atomic_replication_unit &&) = delete;

	      ~atomic_replication_unit ();

	      atomic_replication_unit &operator= (const atomic_replication_unit &) = delete;
	      atomic_replication_unit &operator= (atomic_replication_unit &&) = delete;

	      template <typename T>
	      void apply_log_redo (THREAD_ENTRY *thread_p, log_rv_redo_context &redo_context,
				   const log_rv_redo_rec_info<T> &record_info);
	      int fix_page (THREAD_ENTRY *thread_p);
	      void unfix_page (THREAD_ENTRY *thread_p);
	      PAGE_PTR get_page_ptr ();
	      void set_page_ptr (const PAGE_PTR &ptr);

	      VPID m_vpid;
	    private:
	      log_lsa m_record_lsa;
	      PAGE_PTR m_page_ptr;
	      PGBUF_WATCHER m_watcher;
	      LOG_RCVINDEX m_record_index;
	  };

	  using atomic_unit_vector = std::vector<atomic_replication_unit>;
	  atomic_unit_vector m_units;
	  using vpid_to_page_ptr_map = std::map<VPID, PAGE_PTR>;
	  vpid_to_page_ptr_map m_page_map;
      };

      std::map<TRANID, atomic_replication_sequence> m_sequences_map;
#if !defined (NDEBUG)
      using vpid_set_type = std::set<VPID>;
      std::map<TRANID, vpid_set_type> m_vpid_sets_map;
#endif
  };
}

#endif // _ATOMIC_REPLICATION_HELPER_HPP_
