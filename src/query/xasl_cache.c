/*
 * Copyright (C) 2008 Search Solution Corporation. All rights reserved by Search Solution.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

/*
 * XASL cache.
 */

#ident "$Id$"

#include "config.h"

#include "xasl_cache.h"
#include "perf_monitor.h"
#include "query_executor.h"
#include "list_file.h"
#include "binaryheap.h"
#include "xasl_generation.h"
#include "statistics_sr.h"
#include "thread.h"
#include "query_manager.h"

#define XCACHE_ENTRY_MARK_DELETED	    ((INT32) 0x80000000)
#define XCACHE_ENTRY_TO_BE_RECOMPILED	    ((INT32) 0x40000000)
#define XCACHE_ENTRY_WAS_RECOMPILED	    ((INT32) 0x20000000)
#define XCACHE_ENTRY_SKIP_TO_BE_RECOMPILED  ((INT32) 0x10000000)
#define XCACHE_ENTRY_CLEANUP		    ((INT32) 0x08000000)
#define XCACHE_ENTRY_FLAGS_MASK		    ((INT32) 0xFF000000)

#define XCACHE_ENTRY_FIX_COUNT_MASK	    ((INT32) 0x00FFFFFF)

#define XCACHE_PTR_TO_KEY(ptr) ((XASL_ID *) ptr)
#define XCACHE_PTR_TO_ENTRY(ptr) ((XASL_CACHE_ENTRY *) ptr)

/* xcache statistics. */
typedef struct xcache_stats XCACHE_STATS;
struct xcache_stats
{
  INT64 lookups;
  INT64 hits;
  INT64 miss;
  INT64 recompiles;
  INT64 failed_recompiles;
  INT64 deletes;
  INT64 cleanups;
  INT64 deletes_at_cleanup;
  INT64 fix;
  INT64 unfix;
  INT64 inserts;
  INT64 found_at_insert;
  INT64 rt_checks;
  INT64 rt_true;
};
#define XCACHE_STATS_INITIALIZER { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }


typedef struct xcache_cleanup_candidate XCACHE_CLEANUP_CANDIDATE;
struct xcache_cleanup_candidate
{
  XASL_ID xid;
  struct timeval time_last_used;
};

/* Structure to include all xasl cache global variable. It is easier to visualize the entire system when debugging. */
typedef struct xcache XCACHE;
struct xcache
{
  bool enabled;
  int soft_capacity;
  struct timeval last_cleaned_time;
  int time_threshold;
  LF_HASH_TABLE ht;
  LF_FREELIST freelist;
  volatile INT32 entry_count;
  bool logging_enabled;
  int max_clones;
  INT32 cleanup_flag;
  BINARY_HEAP *cleanup_bh;
  XCACHE_CLEANUP_CANDIDATE *cleanup_array;

  XCACHE_STATS stats;
};

XCACHE xcache_Global = {
  false,			/* enabled */
  0,				/* soft_capacity */
  {0, 0},			/* last_cleaned_time */
  360,				/* time_threshold */
  LF_HASH_TABLE_INITIALIZER,	/* ht */
  LF_FREELIST_INITIALIZER,	/* freelist */
  0,				/* entry_count */
  false,			/* logging_enabled */
  0,				/* max_clones */
  0,				/* cleanup_flag */
  NULL,				/* cleanup_bh */
  NULL,				/* cleanup_array */
  XCACHE_STATS_INITIALIZER
};

/* Create macro's for xcache_Global fields to access them as if they were global variables. */
#define xcache_Enabled xcache_Global.enabled
#define xcache_Soft_capacity xcache_Global.soft_capacity
#define xcache_Time_threshold xcache_Global.time_threshold
#define xcache_Last_cleaned_time xcache_Global.last_cleaned_time
#define xcache_Ht xcache_Global.ht
#define xcache_Ht_freelist xcache_Global.freelist
#define xcache_Entry_count xcache_Global.entry_count
#define xcache_Log xcache_Global.logging_enabled
#define xcache_Max_clones xcache_Global.max_clones
#define xcache_Cleanup_flag xcache_Global.cleanup_flag
#define xcache_Cleanup_bh xcache_Global.cleanup_bh
#define xcache_Cleanup_array xcache_Global.cleanup_array

/* Statistics */
#define XCACHE_STAT_GET(name) ATOMIC_LOAD_64 (&xcache_Global.stats.name)
#define XCACHE_STAT_INC(name) ATOMIC_INC_64 (&xcache_Global.stats.name, 1)

#define TIME_DIFF_SEC(t1, t2) (t1.tv_sec - t2.tv_sec)

/* xcache_Entry_descriptor - used for latch-free hash table.
 * we have to declare member functions before instantiating xcache_Entry_descriptor.
 */
static void *xcache_entry_alloc (void);
static int xcache_entry_free (void *entry);
static int xcache_entry_init (void *entry);
static int xcache_entry_uninit (void *entry);
static int xcache_copy_key (void *src, void *dest);
static int xcache_compare_key (void *key1, void *key2);
static unsigned int xcache_hash_key (void *key, int hash_table_size);

static LF_ENTRY_DESCRIPTOR xcache_Entry_descriptor = {
  offsetof (XASL_CACHE_ENTRY, stack),
  offsetof (XASL_CACHE_ENTRY, next),
  offsetof (XASL_CACHE_ENTRY, del_id),
  offsetof (XASL_CACHE_ENTRY, xasl_id),
  0,				/* No mutex. */

  /* using mutex? */
  LF_EM_NOT_USING_MUTEX,

  xcache_entry_alloc,
  xcache_entry_free,
  xcache_entry_init,
  xcache_entry_uninit,
  xcache_copy_key,
  xcache_compare_key,
  xcache_hash_key,
  NULL,				/* duplicates not accepted. */
};

#define XCACHE_ATOMIC_CAS_CACHE_FLAG(xid, oldcf, newcf) (ATOMIC_CAS_32 (&(xid)->cache_flag, oldcf, newcf))

/* Cleanup */
#define XCACHE_CLEANUP_RATIO 0.2
#define XCACHE_CLEANUP_MIN_NUM_ENTRIES 20
#define XCACHE_CLEANUP_NUM_ENTRIES(capacity) \
  (MAX ((int) (2 * XCACHE_CLEANUP_RATIO * (capacity)), XCACHE_CLEANUP_MIN_NUM_ENTRIES))


/* Recompile threshold */
#define XCACHE_RT_TIMEDIFF_IN_SEC	360	/* 10 minutes */
#define XCACHE_RT_MAX_THRESHOLD		10000	/* 10k pages */
#define XCACHE_RT_FACTOR		10	/* 10x or 0.1x cardinal change */

/* Logging macro's */
#define xcache_check_logging() (xcache_Log = prm_get_bool_value (PRM_ID_XASL_CACHE_LOGGING))
#define xcache_log(...) if (xcache_Log) _er_log_debug (ARG_FILE_LINE, "XASL CACHE: " __VA_ARGS__)
#define xcache_log_error(...) if (xcache_Log) _er_log_debug (ARG_FILE_LINE, "XASL CACHE ERROR: " __VA_ARGS__)

#define XCACHE_LOG_TRAN_TEXT		  "\t tran = %d \n"
#define XCACHE_LOG_TRAN_ARGS(thrd) LOG_FIND_THREAD_TRAN_INDEX (thrd)

#define XCACHE_LOG_ERROR_TEXT		  "\t error_code = %d \n"

#define XCACHE_LOG_ENTRY_PTR_TEXT	  "\t\t entry ptr = %p \n"

#define XCACHE_LOG_SHA1_TEXT		  "\t\t\t sha1 = %08x | %08x | %08x | %08x | %08x \n"
#define XCACHE_LOG_SHA1_ARGS(sha1) SHA1_AS_ARGS (sha1)

#define XCACHE_LOG_TIME_STORED_TEXT	  "\t\t\t time stored = %d sec, %d usec \n"
#define XCACHE_LOG_EXEINFO_TEXT		  "\t\t\t user text = %s \n"					  \
					  "\t\t\t plan text = %s \n"					  \
					  "\t\t\t hash text = %s \n"
#define XCACHE_LOG_CLONE		  "\t\t\t xasl = %p \n"						  \
					  "\t\t\t xasl_buf = %p \n"

#define XCACHE_LOG_XASL_ID_TEXT(msg)									  \
  "\t\t " msg ": \n"											  \
  XCACHE_LOG_SHA1_TEXT											  \
  XCACHE_LOG_TIME_STORED_TEXT
#define XCACHE_LOG_XASL_ID_ARGS(xid)									  \
  SHA1_AS_ARGS (&(xid)->sha1),										  \
  CACHE_TIME_AS_ARGS (&(xid)->time_stored)

#define XCACHE_LOG_ENTRY_TEXT(msg)	 								  \
  "\t " msg ": \n"											  \
  XCACHE_LOG_ENTRY_PTR_TEXT										  \
  XCACHE_LOG_XASL_ID_TEXT ("xasl_id")									  \
  "\t\t sql_info: \n"											  \
  XCACHE_LOG_EXEINFO_TEXT										  \
  "\t\t n_oids = %d \n"
#define XCACHE_LOG_ENTRY_ARGS(xent)									  \
  (xent),												  \
  XCACHE_LOG_XASL_ID_ARGS (&(xent)->xasl_id),								  \
  EXEINFO_AS_ARGS(&(xent)->sql_info),									  \
  (xent)->n_related_objects
#define XCACHE_LOG_CLONE_ARGS(xclone) XASL_CLONE_AS_ARGS (xclone)

#define XCACHE_LOG_OBJECT_TEXT		  "\t\t\t oid = %d|%d|%d \n"					  \
					  "\t\t\t lock = %s \n"						  \
					  "\t\t\t tcard = %d \n"
#define XCACHE_LOG_ENTRY_OBJECT_TEXT(msg)								  \
  "\t\t " msg ": \n"											  \
  XCACHE_LOG_OBJECT_TEXT
#define XCACHE_LOG_ENTRY_OBJECT_ARGS(xent, oidx)							  \
  OID_AS_ARGS (&(xent)->related_objects[oidx].oid),							  \
  LOCK_TO_LOCKMODE_STRING ((xent)->related_objects[oidx].lock),						  \
  (xent)->related_objects[oidx].tcard

static bool xcache_fix (XASL_ID * xid);
static bool xcache_entry_mark_deleted (THREAD_ENTRY * thread_p, XASL_CACHE_ENTRY * xcache_entry);
static void xcache_clone_decache (THREAD_ENTRY * thread_p, XASL_CLONE * xclone);
static void xcache_cleanup (THREAD_ENTRY * thread_p);
static BH_CMP_RESULT xcache_compare_cleanup_candidates (const void *left, const void *right, BH_CMP_ARG ignore_arg);
static bool xcache_check_recompilation_threshold (THREAD_ENTRY * thread_p, XASL_CACHE_ENTRY * xcache_entry);
static void xcache_invalidate_entries (THREAD_ENTRY * thread_p, bool (*invalidate_check) (XASL_CACHE_ENTRY *, void *),
				       void *arg);
static bool xcache_entry_is_related_to_oid (XASL_CACHE_ENTRY * xcache_entry, void *arg);
static XCACHE_CLEANUP_REASON xcache_need_cleanup (void);

/*
 * xcache_initialize () - Initialize XASL cache.
 *
 * return	 : Error Code.
 * thread_p (in) : Thread entry.
 */
int
xcache_initialize (THREAD_ENTRY * thread_p)
{
  int error_code = NO_ERROR;
  HL_HEAPID save_heapid;

  xcache_Enabled = false;

  xcache_check_logging ();

  xcache_Soft_capacity = prm_get_integer_value (PRM_ID_XASL_CACHE_MAX_ENTRIES);
  xcache_Time_threshold = prm_get_integer_value (PRM_ID_XASL_CACHE_TIME_THRESHOLD_IN_MINUTES) * 60;

  if (xcache_Soft_capacity <= 0)
    {
      xcache_log ("disabled.\n");
      return NO_ERROR;
    }

  xcache_Max_clones = prm_get_integer_value (PRM_ID_XASL_CACHE_MAX_CLONES);

  error_code = lf_freelist_init (&xcache_Ht_freelist, 1, xcache_Soft_capacity, &xcache_Entry_descriptor, &xcache_Ts);
  if (error_code != NO_ERROR)
    {
      xcache_log_error ("could not init freelist.\n");
      ASSERT_ERROR ();
      return error_code;
    }

  error_code = lf_hash_init (&xcache_Ht, &xcache_Ht_freelist, xcache_Soft_capacity, &xcache_Entry_descriptor);
  if (error_code != NO_ERROR)
    {
      lf_freelist_destroy (&xcache_Ht_freelist);
      xcache_log_error ("could not init hash table.\n");
      ASSERT_ERROR ();
      return error_code;
    }

  /* Use global heap */
  save_heapid = db_change_private_heap (thread_p, 0);
  xcache_Cleanup_flag = 0;
  xcache_Cleanup_bh = bh_create (thread_p, XCACHE_CLEANUP_NUM_ENTRIES (xcache_Soft_capacity),
				 sizeof (XCACHE_CLEANUP_CANDIDATE), xcache_compare_cleanup_candidates, NULL);
  (void) db_change_private_heap (thread_p, save_heapid);
  if (xcache_Cleanup_bh == NULL)
    {
      lf_freelist_destroy (&xcache_Ht_freelist);
      lf_hash_destroy (&xcache_Ht);
      xcache_log_error ("could not init hash table.\n");
      ASSERT_ERROR_AND_SET (error_code);
      return error_code;
    }

  xcache_Cleanup_array = malloc (xcache_Soft_capacity * sizeof (XCACHE_CLEANUP_CANDIDATE));
  if (xcache_Cleanup_array == NULL)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1,
	      xcache_Soft_capacity * sizeof (XCACHE_CLEANUP_CANDIDATE));
      error_code = ER_OUT_OF_VIRTUAL_MEMORY;
      return error_code;
    }

  /* set last_cleaned_time as current */
  gettimeofday (&xcache_Last_cleaned_time, NULL);

  xcache_log ("init successful.\n");

  xcache_Enabled = true;
  return NO_ERROR;
}

/*
 * xcache_finalize () - Finalize XASL cache.
 *
 * return	     : Void.
 * thread_entry (in) : Thread entry.
 */
void
xcache_finalize (THREAD_ENTRY * thread_p)
{
  HL_HEAPID save_heapid;

  if (!xcache_Enabled)
    {
      return;
    }

  xcache_check_logging ();
  xcache_log ("finalize.\n");

  lf_freelist_destroy (&xcache_Ht_freelist);
  lf_hash_destroy (&xcache_Ht);

  /* Use global heap */
  save_heapid = db_change_private_heap (thread_p, 0);
  if (xcache_Cleanup_bh != NULL)
    {
      bh_destroy (thread_p, xcache_Cleanup_bh);
      xcache_Cleanup_bh = NULL;
    }
  (void) db_change_private_heap (thread_p, save_heapid);

  xcache_Enabled = false;
}

/*
 * xcache_entry_alloc () - Allocate an XASL cache entry.
 *
 * return : Pointer to allocated memory.
 */
static void *
xcache_entry_alloc (void)
{
  XASL_CACHE_ENTRY *xcache_entry = (XASL_CACHE_ENTRY *) malloc (sizeof (XASL_CACHE_ENTRY));
  if (xcache_entry == NULL)
    {
      return NULL;
    }
  xcache_entry->cache_clones = &xcache_entry->one_clone;
  xcache_entry->one_clone.xasl = NULL;
  xcache_entry->one_clone.xasl_buf = NULL;
  xcache_entry->cache_clones_capacity = 1;
  xcache_entry->n_cache_clones = 0;
  pthread_mutex_init (&xcache_entry->cache_clones_mutex, NULL);
  return xcache_entry;
}

/*
 * xcache_entry_free () - Free an XASL cache entry.
 *
 * return     : NO_ERROR.
 * entry (in) : XASL cache entry pointer.
 */
static int
xcache_entry_free (void *entry)
{
  XASL_CACHE_ENTRY *xcache_entry = (XASL_CACHE_ENTRY *) entry;

  if (xcache_entry->cache_clones != &xcache_entry->one_clone)
    {
      /* Should be already freed? */
      assert (false);
      free (xcache_entry->cache_clones);
    }
  pthread_mutex_destroy (&xcache_entry->cache_clones_mutex);
  free (entry);
  return NO_ERROR;
}

/*
 * xcache_entry_init () - Initialize new XASL cache entry.
 *
 * return     : NO_ERROR.
 * entry (in) : XASL cache entry pointer.
 */
static int
xcache_entry_init (void *entry)
{
  XASL_CACHE_ENTRY *xcache_entry = XCACHE_PTR_TO_ENTRY (entry);
  /* Add here if anything should be initialized. */
  xcache_entry->related_objects = NULL;
  xcache_entry->ref_count = 0;

  xcache_entry->sql_info.sql_hash_text = NULL;
  xcache_entry->sql_info.sql_user_text = NULL;
  xcache_entry->sql_info.sql_plan_text = NULL;

  XASL_ID_SET_NULL (&xcache_entry->xasl_id);
  xcache_entry->stream.xasl_id = NULL;
  xcache_entry->stream.xasl_stream = NULL;

  xcache_entry->free_data_on_uninit = false;
  xcache_entry->initialized = true;

  assert (xcache_entry->n_cache_clones == 0);
  return NO_ERROR;
}

/*
 * xcache_entry_uninit () - Uninitialize XASL cache entry.
 *
 * return     : NO_ERROR.
 * entry (in) : XASL cache entry pointer.
 */
static int
xcache_entry_uninit (void *entry)
{
  XASL_CACHE_ENTRY *xcache_entry = XCACHE_PTR_TO_ENTRY (entry);
  THREAD_ENTRY *thread_p = thread_get_thread_entry_info ();

  /* No fixed count or this is claimed & retired immediately. */
  assert ((xcache_entry->xasl_id.cache_flag & XCACHE_ENTRY_FIX_COUNT_MASK) == 0
	  || ((xcache_entry->xasl_id.cache_flag & XCACHE_ENTRY_FIX_COUNT_MASK) == 1
	      && !xcache_entry->free_data_on_uninit));

  if (!xcache_entry->initialized)
    {
      /* Already uninitialized? */
      assert (false);
      return NO_ERROR;
    }

  if (xcache_entry->free_data_on_uninit)
    {
      xcache_log ("uninit an entry from cache and free its data: \n"
		  XCACHE_LOG_ENTRY_TEXT ("xasl cache entry")
		  XCACHE_LOG_TRAN_TEXT, XCACHE_LOG_ENTRY_ARGS (xcache_entry), XCACHE_LOG_TRAN_ARGS (thread_p));

      if (xcache_entry->related_objects != NULL)
	{
	  free_and_init (xcache_entry->related_objects);
	}

      if (xcache_entry->sql_info.sql_hash_text != NULL)
	{
	  free_and_init (xcache_entry->sql_info.sql_hash_text);
	}

      XASL_ID_SET_NULL (&xcache_entry->xasl_id);

      /* Free XASL clones. */
      assert (xcache_entry->n_cache_clones == 0
	      || (xcache_Max_clones > 0 && xcache_entry->n_cache_clones <= xcache_Max_clones));
      assert (xcache_entry->n_cache_clones == 0 || xcache_entry->cache_clones != NULL);
      while (xcache_entry->n_cache_clones > 0)
	{
	  xcache_clone_decache (thread_p, &xcache_entry->cache_clones[--xcache_entry->n_cache_clones]);
	}
      if (xcache_entry->cache_clones != &xcache_entry->one_clone)
	{
	  /* Free cache clones. */
	  assert (xcache_entry->cache_clones_capacity > 0);
	  free (xcache_entry->cache_clones);
	  xcache_entry->cache_clones = &xcache_entry->one_clone;
	  xcache_entry->one_clone.xasl = NULL;
	  xcache_entry->one_clone.xasl_buf = NULL;
	  xcache_entry->cache_clones_capacity = 1;
	}
      if (xcache_entry->stream.xasl_stream != NULL)
	{
	  free_and_init (xcache_entry->stream.xasl_stream);
	}
    }
  else
    {
      xcache_log ("uninit an entry without freeing its data: \n"
		  XCACHE_LOG_ENTRY_TEXT ("xasl cache entry")
		  XCACHE_LOG_TRAN_TEXT, XCACHE_LOG_ENTRY_ARGS (xcache_entry), XCACHE_LOG_TRAN_ARGS (thread_p));
      xcache_entry->related_objects = NULL;
      xcache_entry->sql_info.sql_hash_text = NULL;
      xcache_entry->sql_info.sql_plan_text = NULL;
      xcache_entry->sql_info.sql_user_text = NULL;
      XASL_ID_SET_NULL (&xcache_entry->xasl_id);

      assert (xcache_entry->n_cache_clones == 0);
    }
  xcache_entry->initialized = false;
  return NO_ERROR;
}

/*
 * xcache_copy_key () - Dummy copy key function; XASL cache entry and its key are initialized before being inserted.
 *
 * return    : NO_ERROR.
 * src (in)  : Dummy key source.
 * dest (in) : Dummy key destination.
 */
static int
xcache_copy_key (void *src, void *dest)
{
  /* Key is already set before insert. */
  XASL_ID *xid = (XASL_ID *) dest;
  THREAD_ENTRY *thread_p = NULL;

#if !defined (NDEBUG)
  assert (xid->cache_flag == 1);	/* One reader, no flags. */
#endif /* !NDEBUG */

  xcache_log ("dummy copy key call: \n"
	      XCACHE_LOG_XASL_ID_TEXT ("key")
	      XCACHE_LOG_TRAN_TEXT, XCACHE_LOG_XASL_ID_ARGS (xid), XCACHE_LOG_TRAN_ARGS (thread_p));

  return NO_ERROR;
}

/*
 * xcache_compare_key () - XASL hash compare key function.
 *
 * return    : 0 for match, != 0 for no match.
 * key1 (in) : Lookup key.
 * key2 (in) : Entry key.
 */
static int
xcache_compare_key (void *key1, void *key2)
{
  XASL_ID *lookup_key = XCACHE_PTR_TO_KEY (key1);
  XASL_ID *entry_key = XCACHE_PTR_TO_KEY (key2);
  INT32 cache_flag;
  THREAD_ENTRY *thread_p = NULL;

  /* Compare key algorithm depends on sha1 and cache flags.
   * SHA-1 is generated hash based on query hash text.
   * If SHA-1 does not match, the entry does not belong to the same query (so clearly no match).
   *
   * Even if SHA-1 hash matches, the cache flags can still invalidate the entry.
   * 1. If this is a cleanup lookup, the entry must be unfixed and unmarked (its cache_flag must be 0). Successful
   *    cleanup can occur if cache-flag is CASed to XCACHE_ENTRY_MARK_DELETED.
   * 2. Marked deleted entry can only be found with the scope of deleting entry (the lookup key must also be marked as
   *    deleted.
   * 3. Was recompiled entry can never be found. They are followed by another entry with similar SHA-1 which will can
   *    be found.
   * 4. To be recompiled entry cannot be found by its recompiler - lookup key is marked as skip to be recompiled.
   * 5. When all previous flags do not invalidate entry, the thread looking for entry must also increment fix count.
   *    Incrementing fix count can fail if concurrent thread mark entry as deleted.
   */

  if (SHA1Compare (&lookup_key->sha1, &entry_key->sha1) != 0)
    {
      /* Not the same query. */

      xcache_log ("compare keys: sha1 mismatch\n"
		  "\t\t lookup key: \n" XCACHE_LOG_SHA1_TEXT
		  "\t\t  entry key: \n" XCACHE_LOG_SHA1_TEXT
		  XCACHE_LOG_TRAN_TEXT,
		  XCACHE_LOG_SHA1_ARGS (&lookup_key->sha1),
		  XCACHE_LOG_SHA1_ARGS (&entry_key->sha1), XCACHE_LOG_TRAN_ARGS (thread_p));
      return -1;
    }
  /* SHA-1 hash matched. */
  /* Now we matched XASL_ID. */

  if (lookup_key->cache_flag == XCACHE_ENTRY_CLEANUP)
    {
      /* Lookup for cleaning the entry from hash. The entry must not be fixed by another and must not have any
       * flags. */
      if (XCACHE_ATOMIC_CAS_CACHE_FLAG (entry_key, 0, XCACHE_ENTRY_MARK_DELETED))
	{
	  /* Successfully marked for delete. */
	  return 0;
	}
      else
	{
	  /* Entry was fixed or had another flag set. */
	  return -1;
	}
    }

  cache_flag = entry_key->cache_flag;
  if (cache_flag & XCACHE_ENTRY_MARK_DELETED)
    {
      /* The entry was marked for deletion. */
      if (lookup_key->cache_flag == cache_flag)
	{
	  /* The deleter found its entry. */
	  xcache_log ("compare keys: found for delete\n"
		      "\t\t lookup key: \n" XCACHE_LOG_SHA1_TEXT
		      "\t\t  entry key: \n" XCACHE_LOG_SHA1_TEXT
		      XCACHE_LOG_TRAN_TEXT,
		      XCACHE_LOG_SHA1_ARGS (&lookup_key->sha1),
		      XCACHE_LOG_SHA1_ARGS (&entry_key->sha1), XCACHE_LOG_TRAN_ARGS (thread_p));
	  return 0;
	}
      else
	{
	  /* This is not the deleter. Ignore this entry - it will be removed from hash. */

	  xcache_log ("(tran=%d) compare keys: skip deleted\n"
		      "\t\t lookup key: \n" XCACHE_LOG_SHA1_TEXT
		      "\t\t  entry key: \n" XCACHE_LOG_SHA1_TEXT
		      XCACHE_LOG_TRAN_TEXT,
		      XCACHE_LOG_SHA1_ARGS (&lookup_key->sha1),
		      XCACHE_LOG_SHA1_ARGS (&entry_key->sha1), XCACHE_LOG_TRAN_ARGS (thread_p));
	  return -1;
	}
    }

  if (cache_flag & XCACHE_ENTRY_WAS_RECOMPILED)
    {
      /* Go to another entry. */

      xcache_log ("compare keys: skip recompiled\n"
		  "\t\t lookup key: \n" XCACHE_LOG_SHA1_TEXT
		  "\t\t  entry key: \n" XCACHE_LOG_SHA1_TEXT
		  XCACHE_LOG_TRAN_TEXT,
		  XCACHE_LOG_SHA1_ARGS (&lookup_key->sha1),
		  XCACHE_LOG_SHA1_ARGS (&entry_key->sha1), XCACHE_LOG_TRAN_ARGS (thread_p));
      return -1;
    }

  if ((cache_flag & XCACHE_ENTRY_TO_BE_RECOMPILED) && (lookup_key->cache_flag & XCACHE_ENTRY_SKIP_TO_BE_RECOMPILED))
    {
      /* We are trying to insert a new entry to replace the entry to be recompiled. Skip this. */

      xcache_log ("compare keys: skip to be recompiled\n"
		  "\t\t lookup key: \n" XCACHE_LOG_SHA1_TEXT
		  "\t\t  entry key: \n" XCACHE_LOG_SHA1_TEXT
		  XCACHE_LOG_TRAN_TEXT,
		  XCACHE_LOG_SHA1_ARGS (&lookup_key->sha1),
		  XCACHE_LOG_SHA1_ARGS (&entry_key->sha1), XCACHE_LOG_TRAN_ARGS (thread_p));
      return -1;
    }

  /* The entry is what we are looking for. One last step, we must increment read counter in cache flag. */
  if (!xcache_fix (entry_key))
    {
      /* Entry was deleted. */

      xcache_log ("compare keys: could not fix\n"
		  "\t\t lookup key: \n" XCACHE_LOG_SHA1_TEXT
		  "\t\t  entry key: \n" XCACHE_LOG_SHA1_TEXT
		  XCACHE_LOG_TRAN_TEXT,
		  XCACHE_LOG_SHA1_ARGS (&lookup_key->sha1),
		  XCACHE_LOG_SHA1_ARGS (&entry_key->sha1), XCACHE_LOG_TRAN_ARGS (thread_p));
      return -1;
    }

  /* Successfully marked as reader. */
  xcache_log ("compare keys: key matched and fixed\n"
	      "\t\t lookup key: \n" XCACHE_LOG_SHA1_TEXT
	      "\t\t  entry key: \n" XCACHE_LOG_SHA1_TEXT
	      XCACHE_LOG_TRAN_TEXT,
	      XCACHE_LOG_SHA1_ARGS (&lookup_key->sha1),
	      XCACHE_LOG_SHA1_ARGS (&entry_key->sha1), XCACHE_LOG_TRAN_ARGS (thread_p));
  return 0;
}

/*
 * xcache_hash_key () - Hash index based on key SHA-1.
 *
 * return		: Hash index.
 * key (in)	        : Key value.
 * hash_table_size (in) : Hash size.
 */
static unsigned int
xcache_hash_key (void *key, int hash_table_size)
{
  XASL_ID *xasl_id = XCACHE_PTR_TO_KEY (key);
  unsigned int hash_index = ((unsigned int) xasl_id->sha1.h[0]) % hash_table_size;

  xcache_log ("hash index: \n"
	      XCACHE_LOG_SHA1_TEXT
	      "\t\t hash index value = %d \n"
	      XCACHE_LOG_TRAN_TEXT,
	      XCACHE_LOG_SHA1_ARGS (&xasl_id->sha1),
	      hash_index, XCACHE_LOG_TRAN_ARGS (thread_get_thread_entry_info ()));

  return hash_index;
}

/*
 * xcache_find_sha1 () - Lookup XASL cache by SHA-1.
 *
 * return	      : Error code.
 * thread_p (in)      : Thread entry.
 * sha1 (in)	      : SHA-1 hash.
 * xcache_entry (out) : XASL cache entry if found.
 * rt_check (out)     : True if recompile is needed (due to recompile threshold).
 */
int
xcache_find_sha1 (THREAD_ENTRY * thread_p, const SHA1Hash * sha1, XASL_CACHE_ENTRY ** xcache_entry, bool * rt_check)
{
  XASL_ID lookup_key;
  int error_code = NO_ERROR;
  LF_TRAN_ENTRY *t_entry = thread_get_tran_entry (thread_p, THREAD_TS_XCACHE);

  assert (xcache_entry != NULL && *xcache_entry == NULL);

  if (!xcache_Enabled)
    {
      return NO_ERROR;
    }

  xcache_check_logging ();

  XCACHE_STAT_INC (lookups);
  perfmon_inc_stat (thread_p, PSTAT_PC_NUM_LOOKUP);

  XASL_ID_SET_NULL (&lookup_key);
  lookup_key.sha1 = *sha1;

  error_code = lf_hash_find (t_entry, &xcache_Ht, &lookup_key, (void **) xcache_entry);
  if (error_code != NO_ERROR)
    {
      ASSERT_ERROR ();
      xcache_log_error ("error finding cache entry: \n"
			XCACHE_LOG_SHA1_TEXT
			XCACHE_LOG_ERROR_TEXT
			XCACHE_LOG_TRAN_TEXT,
			XCACHE_LOG_SHA1_ARGS (&lookup_key.sha1), error_code, XCACHE_LOG_TRAN_ARGS (thread_p));

      return error_code;
    }
  if (*xcache_entry == NULL)
    {
      /* No match! */
      XCACHE_STAT_INC (miss);
      perfmon_inc_stat (thread_p, PSTAT_PC_NUM_MISS);
      xcache_log ("could not find cache entry: \n"
		  XCACHE_LOG_SHA1_TEXT
		  XCACHE_LOG_TRAN_TEXT, XCACHE_LOG_SHA1_ARGS (&lookup_key.sha1), XCACHE_LOG_TRAN_ARGS (thread_p));

      return NO_ERROR;
    }
  /* Found a match. */
  /* We have incremented fix count, we don't need lf_tran anymore. */
  lf_tran_end_with_mb (t_entry);

  perfmon_inc_stat (thread_p, PSTAT_PC_NUM_HIT);
  XCACHE_STAT_INC (hits);

  assert (*xcache_entry != NULL);

  if (rt_check)
    {
      /* Check if query should be recompile. */
      *rt_check = xcache_check_recompilation_threshold (thread_p, *xcache_entry);
      if (*rt_check)
	{
	  /* We need to recompile. */
	  xcache_unfix (thread_p, *xcache_entry);
	  *xcache_entry = NULL;

	  return NO_ERROR;
	}
    }

  assert (*xcache_entry != NULL);
  xcache_log ("found cache entry by sha1: \n"
	      XCACHE_LOG_ENTRY_TEXT ("entry")
	      XCACHE_LOG_TRAN_TEXT, XCACHE_LOG_ENTRY_ARGS (*xcache_entry), XCACHE_LOG_TRAN_ARGS (thread_p));

  return NO_ERROR;
}

/*
 * xcache_find_xasl_id () - Find XASL cache entry by XASL_ID. Besides matching SHA-1, we have to match time_stored.
 *
 * return	      : NO_ERROR.
 * thread_p (in)      : Thread entry.
 * xid (in)	      : XASL_ID.
 * xcache_entry (out) : XASL cache entry if found.
 * xclone (out)	      : XASL_CLONE (obtained from cache or loaded).
 */
int
xcache_find_xasl_id (THREAD_ENTRY * thread_p, const XASL_ID * xid, XASL_CACHE_ENTRY ** xcache_entry,
		     XASL_CLONE * xclone)
{
  int error_code = NO_ERROR;
  HL_HEAPID save_heapid = 0;
  int oid_index;
  int lock_result;
  bool use_xasl_clone = false;

  assert (xid != NULL);
  assert (xcache_entry != NULL && *xcache_entry == NULL);
  assert (xclone != NULL);

  error_code = xcache_find_sha1 (thread_p, &xid->sha1, xcache_entry, NULL);
  if (error_code != NO_ERROR)
    {
      ASSERT_ERROR ();
      return error_code;
    }
  if (*xcache_entry == NULL)
    {
      /* No entry was found. */
      return NO_ERROR;
    }
  if ((*xcache_entry)->xasl_id.time_stored.sec != xid->time_stored.sec
      || (*xcache_entry)->xasl_id.time_stored.usec != xid->time_stored.usec)
    {
      /* We don't know if this XASL cache entry is good for us. We need to restart by recompiling. */
      xcache_log ("could not get cache entry because time_stored mismatch \n"
		  XCACHE_LOG_ENTRY_TEXT ("entry")
		  XCACHE_LOG_XASL_ID_TEXT ("lookup xasl_id")
		  XCACHE_LOG_TRAN_TEXT,
		  XCACHE_LOG_ENTRY_ARGS (*xcache_entry),
		  XCACHE_LOG_XASL_ID_ARGS (xid), XCACHE_LOG_TRAN_ARGS (thread_p));
      xcache_unfix (thread_p, *xcache_entry);
      *xcache_entry = NULL;

      /* TODO:
       * The one reason we cannot accept this cache entry is because one of the referenced classes might have suffered
       * a schema change. Or maybe a serial may have been altered, although I am not sure this can actually affect our
       * plan.
       * Instead of using time_stored, we could find another way to identify if an XASL cache entry is still usable.
       * Something that could detect if classes have been modified (and maybe serials).
       */

      return NO_ERROR;
    }
  else
    {
      xcache_log ("found cache entry by xasl_id: \n"
		  XCACHE_LOG_ENTRY_TEXT ("entry")
		  XCACHE_LOG_XASL_ID_TEXT ("lookup xasl_id")
		  XCACHE_LOG_TRAN_TEXT,
		  XCACHE_LOG_ENTRY_ARGS (*xcache_entry),
		  XCACHE_LOG_XASL_ID_ARGS (xid), XCACHE_LOG_TRAN_ARGS (thread_p));
    }

  assert ((*xcache_entry) != NULL);

  /* Get lock on all classes in xasl cache entry. */
  /* The reason we need to do the locking here is to confirm the entry validity. Without the locks, we cannot guarantee
   * the entry will remain valid (somebody holding SCH_M_LOCK may invalidate it). Moreover, in most cases, the
   * transaction did not have locks up to this point (because it executes a prepared query).
   * So, we have to get all locks and then check the entry validity.
   */
  for (oid_index = 0; oid_index < (*xcache_entry)->n_related_objects; oid_index++)
    {
      if ((*xcache_entry)->related_objects[oid_index].lock <= NULL_LOCK)
	{
	  /* No lock. */
	  continue;
	}
      lock_result =
	lock_scan (thread_p, &(*xcache_entry)->related_objects[oid_index].oid, LK_UNCOND_LOCK,
		   (*xcache_entry)->related_objects[oid_index].lock);
      if (lock_result != LK_GRANTED)
	{
	  ASSERT_ERROR_AND_SET (error_code);
	  xcache_unfix (thread_p, *xcache_entry);
	  *xcache_entry = NULL;
	  xcache_log ("could not get cache entry because lock on oid failed: \n"
		      XCACHE_LOG_ENTRY_TEXT ("entry")
		      XCACHE_LOG_ENTRY_OBJECT_TEXT ("object that could not be locked")
		      XCACHE_LOG_TRAN_TEXT,
		      XCACHE_LOG_ENTRY_ARGS (*xcache_entry),
		      XCACHE_LOG_ENTRY_OBJECT_ARGS (*xcache_entry, oid_index), XCACHE_LOG_TRAN_ARGS (thread_p));

	  return error_code;
	}
    }

  /* Check the entry is still valid.  Uses atomic to prevent any code reordering */
  if (ATOMIC_INC_32 (&((*xcache_entry)->xasl_id.cache_flag), 0) & XCACHE_ENTRY_MARK_DELETED)
    {
      /* Someone has marked entry as deleted. */
      xcache_log ("could not get cache entry because it was deleted until locked: \n"
		  XCACHE_LOG_ENTRY_TEXT ("entry")
		  XCACHE_LOG_TRAN_TEXT, XCACHE_LOG_ENTRY_ARGS (*xcache_entry), XCACHE_LOG_TRAN_ARGS (thread_p));

      xcache_unfix (thread_p, *xcache_entry);
      *xcache_entry = NULL;
      return NO_ERROR;
    }

  assert ((*xcache_entry) != NULL);

  if (xcache_uses_clones ())
    {
      use_xasl_clone = true;
      /* Try to fetch a cached clone. */
      if ((*xcache_entry)->cache_clones == NULL)
	{
	  assert_release (false);
	  /* Fall through. */
	}
      else
	{
	  (void) pthread_mutex_lock (&(*xcache_entry)->cache_clones_mutex);
	  assert ((*xcache_entry)->n_cache_clones <= xcache_Max_clones);
	  if ((*xcache_entry)->n_cache_clones > 0)
	    {
	      /* A clone is available. */
	      *xclone = (*xcache_entry)->cache_clones[--(*xcache_entry)->n_cache_clones];
	      (void) pthread_mutex_unlock (&(*xcache_entry)->cache_clones_mutex);

	      assert (xclone->xasl != NULL && xclone->xasl_buf != NULL);

	      xcache_log ("found cached clone: \n"
			  XCACHE_LOG_ENTRY_TEXT ("entry")
			  XCACHE_LOG_XASL_ID_TEXT ("lookup xasl_id")
			  XCACHE_LOG_CLONE
			  XCACHE_LOG_TRAN_TEXT,
			  XCACHE_LOG_ENTRY_ARGS (*xcache_entry),
			  XCACHE_LOG_XASL_ID_ARGS (xid),
			  XCACHE_LOG_CLONE_ARGS (xclone), XCACHE_LOG_TRAN_ARGS (thread_p));
	      return NO_ERROR;
	    }
	  (void) pthread_mutex_unlock (&(*xcache_entry)->cache_clones_mutex);
	}
      /* Clone not found. */
      /* When clones are activated, we use global heap to generate the XASL's; this way, other threads can use the
       * clone. */
      save_heapid = db_change_private_heap (thread_p, 0);
    }
  error_code =
    stx_map_stream_to_xasl (thread_p, &xclone->xasl, use_xasl_clone, (*xcache_entry)->stream.xasl_stream,
			    (*xcache_entry)->stream.xasl_stream_size, &xclone->xasl_buf);
  if (save_heapid != 0)
    {
      /* Restore heap id. */
      (void) db_change_private_heap (thread_p, save_heapid);
    }
  if (error_code != NO_ERROR)
    {
      ASSERT_ERROR ();
      assert (xclone->xasl == NULL && xclone->xasl_buf == NULL);
      xcache_unfix (thread_p, *xcache_entry);
      *xcache_entry = NULL;

      xcache_log_error ("could not load XASL tree and buffer: \n"
			XCACHE_LOG_XASL_ID_TEXT ("xasl_id")
			XCACHE_LOG_TRAN_TEXT, XCACHE_LOG_XASL_ID_ARGS (xid), XCACHE_LOG_TRAN_ARGS (thread_p));

      return error_code;
    }
  assert (xclone->xasl != NULL && xclone->xasl_buf != NULL);

  xcache_log ("loaded xasl clone: \n"
	      XCACHE_LOG_ENTRY_TEXT ("entry")
	      XCACHE_LOG_XASL_ID_TEXT ("lookup xasl_id")
	      XCACHE_LOG_CLONE
	      XCACHE_LOG_TRAN_TEXT,
	      XCACHE_LOG_ENTRY_ARGS (*xcache_entry),
	      XCACHE_LOG_XASL_ID_ARGS (xid), XCACHE_LOG_CLONE_ARGS (xclone), XCACHE_LOG_TRAN_ARGS (thread_p));

  return NO_ERROR;
}

/*
 * xcache_fix () - Fix XASL cache entry by incrementing fix count in cache flag.
 *
 * return       : Error code.
 * xid (in/out) : XASL_ID.
 */
static bool
xcache_fix (XASL_ID * xid)
{
  INT32 cache_flag;

  assert (xid != NULL);

  XCACHE_STAT_INC (fix);

  do
    {
      cache_flag = xid->cache_flag;
#if defined (SA_MODE)
      assert ((cache_flag & XCACHE_ENTRY_FIX_COUNT_MASK) == 0);
#endif /* SA_MODE */
      if (cache_flag & XCACHE_ENTRY_MARK_DELETED)
	{
	  /* It was deleted. */
	  return false;
	}
    }
  while (!XCACHE_ATOMIC_CAS_CACHE_FLAG (xid, cache_flag, cache_flag + 1));

  /* Success */
  return true;
}

/*
 * xcache_unfix () - Unfix XASL cache entry by decrementing fix count in cache flag. If we are last to use entry
 *		     remove it from hash.
 *
 * return	         : Void.
 * thread_p (in)         : Thread entry.
 * xcache_entry (in/out) : XASL cache entry.
 */
void
xcache_unfix (THREAD_ENTRY * thread_p, XASL_CACHE_ENTRY * xcache_entry)
{
  INT32 cache_flag = 0;
  INT32 new_cache_flag = 0;
  LF_TRAN_ENTRY *t_entry = thread_get_tran_entry (thread_p, THREAD_TS_XCACHE);
  int error_code = NO_ERROR;
  int success = 0;
  struct timeval time_last_used;

  assert (xcache_entry != NULL);
  assert (xcache_Enabled);

  /* Mark last used. We need to do an atomic operation here. We cannot set both tv_sec and tv_usec and we don't have to.
   * Setting tv_sec is enough.
   */
  (void) gettimeofday (&time_last_used, NULL);
  ATOMIC_TAS_32 (&xcache_entry->time_last_used.tv_sec, time_last_used.tv_sec);

  XCACHE_STAT_INC (unfix);
  ATOMIC_INC_64 (&xcache_entry->ref_count, 1);

  /* Decrement the number of users. */
  do
    {
      cache_flag = xcache_entry->xasl_id.cache_flag;
      new_cache_flag = cache_flag;

      /* There should be at least one fix. */
      assert ((new_cache_flag & XCACHE_ENTRY_FIX_COUNT_MASK) != 0);

      /* Unfix */
      new_cache_flag = cache_flag - 1;
      if (new_cache_flag == XCACHE_ENTRY_TO_BE_RECOMPILED)
	{
	  /* We are the last to have fixed to be recompiled entry. This is an invalid state.
	   * The recompiler should set the entry as "was recompiled" before unfixing it!
	   */
	  assert (false);
	  xcache_log_error ("unexpected cache_flag = XCACHE_ENTRY_TO_BE_RECOMPILED on unfix: \n"
			    XCACHE_LOG_ENTRY_TEXT ("invalid entry")
			    XCACHE_LOG_TRAN_TEXT,
			    XCACHE_LOG_ENTRY_ARGS (xcache_entry), XCACHE_LOG_TRAN_ARGS (thread_p));
	  /* Delete the entry. */
	  new_cache_flag = XCACHE_ENTRY_MARK_DELETED;
	}
      else if (new_cache_flag == XCACHE_ENTRY_MARK_DELETED)
	{
	  /* If entry is marked as deleted and we are the last thread to have fixed this entry, we must remove it. */
	}
      else if (new_cache_flag == XCACHE_ENTRY_WAS_RECOMPILED)
	{
	  /* This the last thread to have fixed the entry and we should mark it as deleted and remove it. */
	  new_cache_flag = XCACHE_ENTRY_MARK_DELETED;
	}
    }
  while (!XCACHE_ATOMIC_CAS_CACHE_FLAG (&xcache_entry->xasl_id, cache_flag, new_cache_flag));

  xcache_log ("unfix entry: \n"
	      XCACHE_LOG_ENTRY_TEXT ("entry")
	      XCACHE_LOG_TRAN_TEXT, XCACHE_LOG_ENTRY_ARGS (xcache_entry), XCACHE_LOG_TRAN_ARGS (thread_p));

  if (new_cache_flag == XCACHE_ENTRY_MARK_DELETED)
    {
      /* I am last user after object was marked as deleted. */
      xcache_log ("delete entry from hash after unfix: \n"
		  XCACHE_LOG_ENTRY_TEXT ("entry")
		  XCACHE_LOG_TRAN_TEXT, XCACHE_LOG_ENTRY_ARGS (xcache_entry), XCACHE_LOG_TRAN_ARGS (thread_p));
      /* No need to acquire the clone mutex, since I'm the unique user. */
      while (xcache_entry->n_cache_clones > 0)
	{
	  xcache_clone_decache (thread_p, &xcache_entry->cache_clones[--xcache_entry->n_cache_clones]);
	}

      error_code = lf_hash_delete (t_entry, &xcache_Ht, &xcache_entry->xasl_id, &success);
      if (error_code != NO_ERROR)
	{
	  /* Errors are not expected. */
	  assert (false);
	  return;
	}
      if (success == 0)
	{
	  /* Failure is not expected. */
	  assert (false);
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
	  return;
	}
    }
}

/*
 * xcache_entry_mark_deleted () - Mark XASL cache entry for delete.
 *
 * return	     : True if no one else is using the entry and can be removed from hash.
 * thread_p (in)     : Thread entry.
 * xcache_entry (in) : XASL cache entry.
 */
static bool
xcache_entry_mark_deleted (THREAD_ENTRY * thread_p, XASL_CACHE_ENTRY * xcache_entry)
{
  LF_TRAN_ENTRY *t_entry = thread_get_tran_entry (thread_p, THREAD_TS_XCACHE);
  INT32 cache_flag = 0;
  INT32 new_cache_flag;
  int error_code = NO_ERROR;
  int success = 0;

  /* Mark for delete. We must successfully set XCACHE_ENTRY_MARK_DELETED flag. */
  do
    {
      cache_flag = xcache_entry->xasl_id.cache_flag;
      if (cache_flag & XCACHE_ENTRY_MARK_DELETED)
	{
	  /* Cleanup could have marked this entry for delete. */
	  xcache_log ("tried to mark entry as deleted, but somebody else already marked it: \n"
		      XCACHE_LOG_ENTRY_TEXT ("entry")
		      XCACHE_LOG_TRAN_TEXT, XCACHE_LOG_ENTRY_ARGS (xcache_entry), XCACHE_LOG_TRAN_ARGS (thread_p));
	  return false;
	}
      if (cache_flag & XCACHE_ENTRY_TO_BE_RECOMPILED)
	{
	  /* Somebody is compiling the entry? I think the locks have been messed up. */
	  xcache_log_error ("tried to mark entry as deleted, but it was marked as to be recompiled: \n"
			    XCACHE_LOG_ENTRY_TEXT ("entry")
			    XCACHE_LOG_TRAN_TEXT,
			    XCACHE_LOG_ENTRY_ARGS (xcache_entry), XCACHE_LOG_TRAN_ARGS (thread_p));
	  assert (false);
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
	  return false;
	}

      new_cache_flag = cache_flag;
      if (new_cache_flag & XCACHE_ENTRY_WAS_RECOMPILED)
	{
	  /* This can happen. Somebody recompiled the entry and it was not (yet) removed. We will replace the flag
	   * with XCACHE_ENTRY_MARK_DELETED. */
	  new_cache_flag &= ~XCACHE_ENTRY_WAS_RECOMPILED;
	}
      new_cache_flag = new_cache_flag | XCACHE_ENTRY_MARK_DELETED;
    }
  while (!XCACHE_ATOMIC_CAS_CACHE_FLAG (&xcache_entry->xasl_id, cache_flag, new_cache_flag));

  xcache_log ("marked entry as deleted: \n"
	      XCACHE_LOG_ENTRY_TEXT ("entry")
	      XCACHE_LOG_TRAN_TEXT, XCACHE_LOG_ENTRY_ARGS (xcache_entry), XCACHE_LOG_TRAN_ARGS (thread_p));

  XCACHE_STAT_INC (deletes);
  perfmon_inc_stat (thread_p, PSTAT_PC_NUM_DELETE);
  ATOMIC_INC_32 (&xcache_Entry_count, -1);

  /* The entry can be deleted if the only fixer is this transaction. */
  return (new_cache_flag == XCACHE_ENTRY_MARK_DELETED);
}

static XCACHE_CLEANUP_REASON
xcache_need_cleanup (void)
{
  struct timeval current_time;
  if (xcache_Soft_capacity < xcache_Entry_count)
    {
      return XCACHE_CLEANUP_FULL;
    }
  else
    {
      gettimeofday (&current_time, NULL);
      if (TIME_DIFF_SEC (current_time, xcache_Last_cleaned_time) > xcache_Time_threshold)
	{
	  return XCACHE_CLEANUP_TIMEOUT;
	}
    }

  return XCACHE_CLEANUP_NONE;
}

/*
 * xcache_insert () - Insert or recompile XASL cache entry.
 *
 * return	      : Error code.
 * thread_p (in)      : Thread entry.
 * context (in)	      : Compile context (sql_info & recompile_xasl).
 * stream (in)	      : XASL stream.
 * n_oid (in)	      : Related objects count.
 * class_oids (in)    : Related objects OID's.
 * class_locks (in)   : Related objects locks.
 * tcards (in)	      : Related objects cardinality.
 * xcache_entry (out) : XASL cache entry.
 */
int
xcache_insert (THREAD_ENTRY * thread_p, const COMPILE_CONTEXT * context, XASL_STREAM * stream,
	       int n_oid, const OID * class_oids, const int *class_locks, const int *tcards,
	       XASL_CACHE_ENTRY ** xcache_entry)
{
  int error_code = NO_ERROR;
  LF_TRAN_ENTRY *t_entry = thread_get_tran_entry (thread_p, THREAD_TS_XCACHE);
  int inserted = 0;
  XASL_ID xid;
  INT32 cache_flag;
  INT32 new_cache_flag;
  XASL_CACHE_ENTRY *to_be_recompiled = NULL;
  XCACHE_RELATED_OBJECT *related_objects = NULL;
  char *sql_hash_text = NULL;
  char *sql_user_text = NULL;
  char *sql_plan_text = NULL;
  struct timeval time_stored;
  int sql_hash_text_len = 0, sql_user_text_len = 0, sql_plan_text_len = 0;
  char *strbuf = NULL;

  assert (xcache_entry != NULL && *xcache_entry == NULL);
  assert (stream != NULL);
  assert (stream->xasl_stream != NULL || !context->recompile_xasl);

  if (!xcache_Enabled)
    {
      return NO_ERROR;
    }

  xcache_check_logging ();

  XASL_ID_SET_NULL (&xid);
  xid.sha1 = context->sha1;

  XCACHE_STAT_INC (inserts);

  /* Allocate XASL cache entry data. */
  if (n_oid > 0)
    {
      int index;
      related_objects = (XCACHE_RELATED_OBJECT *) malloc (n_oid * sizeof (XCACHE_RELATED_OBJECT));
      if (related_objects == NULL)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1,
		  n_oid * sizeof (XCACHE_RELATED_OBJECT));
	  error_code = ER_OUT_OF_VIRTUAL_MEMORY;
	  goto error;
	}
      for (index = 0; index < n_oid; index++)
	{
	  related_objects[index].oid = class_oids[index];
	  related_objects[index].lock = (LOCK) class_locks[index];
	  related_objects[index].tcard = tcards[index];
	}
    }

  sql_hash_text_len = strlen (context->sql_hash_text) + 1;
  if (context->sql_user_text != NULL)
    {
      sql_user_text_len = strlen (context->sql_user_text) + 1;
    }
  if (context->sql_plan_text != NULL)
    {
      sql_plan_text_len = strlen (context->sql_plan_text) + 1;
    }
  strbuf = (char *) malloc (sql_hash_text_len + sql_user_text_len + sql_plan_text_len);
  if (strbuf == NULL)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1,
	      sql_hash_text_len + sql_user_text_len + sql_plan_text_len);
      error_code = ER_OUT_OF_VIRTUAL_MEMORY;
      goto error;
    }

  memcpy (strbuf, context->sql_hash_text, sql_hash_text_len);
  sql_hash_text = strbuf;
  strbuf += sql_hash_text_len;

  if (sql_user_text_len > 0)
    {
      memcpy (strbuf, context->sql_user_text, sql_user_text_len);
      sql_user_text = strbuf;
      strbuf += sql_user_text_len;
    }

  if (sql_plan_text_len > 0)
    {
      memcpy (strbuf, context->sql_plan_text, sql_plan_text_len);
      sql_plan_text = strbuf;
    }

  /* save stored time */
  (void) gettimeofday (&time_stored, NULL);
  CACHE_TIME_MAKE (&stream->xasl_id->time_stored, &time_stored);


  /* We need to do a loop here for recompile_xasl case. It will break after the first iteration if recompile_xasl flag
   * is false.
   *
   * When we want to recompile the XASL cache entry, we try to avoid blocking others from using existing cache entry.
   * If an entry exists, the recompiler mark it "to be recompiled". Concurrent transactions can find and use this
   * entry. After adding new entry, the original is marked as "was recompiled". This entry can no longer be found
   * but is still valid if it was previously obtained.
   *
   * Things get messy if there are at least two concurrent recompilers. We assume that this does not (or should not)
   * happen in real-world scenarios. But if it happens, we need to make it work.
   * Multiple recompilers can loop here several times. One entry can be recompiled by one thread at a time. Others will
   * loop until current recompiler finishes.
   */
  while (true)
    {
      /* Claim a new entry from freelist to initialize. */
      *xcache_entry = (XASL_CACHE_ENTRY *) lf_freelist_claim (t_entry, &xcache_Ht_freelist);
      if (*xcache_entry == NULL)
	{
	  ASSERT_ERROR_AND_SET (error_code);
	  return error_code;
	}

      /* Initialize xcache_entry stuff. */
      XASL_ID_COPY (&(*xcache_entry)->xasl_id, stream->xasl_id);
      (*xcache_entry)->xasl_id.sha1 = context->sha1;
      (*xcache_entry)->xasl_id.cache_flag = 1;	/* Start with fix count = 1. */
      (*xcache_entry)->n_related_objects = n_oid;
      (*xcache_entry)->related_objects = related_objects;
      (*xcache_entry)->sql_info.sql_hash_text = sql_hash_text;
      (*xcache_entry)->sql_info.sql_user_text = sql_user_text;
      (*xcache_entry)->sql_info.sql_plan_text = sql_plan_text;
      (*xcache_entry)->stream = *stream;
      (*xcache_entry)->time_last_rt_check = time_stored;
      (*xcache_entry)->time_last_used = time_stored;

      /* Now that new entry is initialized, we can try to insert it. */
      error_code = lf_hash_insert_given (t_entry, &xcache_Ht, &xid, (void **) xcache_entry, &inserted);
      if (error_code != NO_ERROR)
	{
	  xcache_log_error ("error inserting new entry: \n",
			    XCACHE_LOG_ENTRY_TEXT ("entry"),
			    XCACHE_LOG_ERROR_TEXT
			    XCACHE_LOG_TRAN_TEXT,
			    XCACHE_LOG_ENTRY_ARGS (*xcache_entry), XCACHE_LOG_TRAN_ARGS (thread_p), error_code);
	  ASSERT_ERROR ();
	  goto error;
	}
      assert (*xcache_entry != NULL);

      /* We have incremented fix count, we don't need lf_tran anymore. */
      lf_tran_end_with_mb (t_entry);

      if (inserted)
	{
	  (*xcache_entry)->free_data_on_uninit = true;
	  perfmon_inc_stat (thread_p, PSTAT_PC_NUM_ADD);
	}
      else
	{
	  XCACHE_STAT_INC (found_at_insert);
	}

      if (inserted || !context->recompile_xasl)
	{
	  /* The entry is accepted. */

	  if (to_be_recompiled != NULL)
	    {
	      assert (context->recompile_xasl);
	      /* Now that we inserted new cache entry, we can mark the old entry as recompiled. */
	      do
		{
		  cache_flag = to_be_recompiled->xasl_id.cache_flag;
		  if ((cache_flag & XCACHE_ENTRY_FLAGS_MASK) != XCACHE_ENTRY_TO_BE_RECOMPILED)
		    {
		      /* Unexpected flags. */
		      assert (false);
		      xcache_log_error ("unexpected flag for entry to be recompiled: \n"
					XCACHE_LOG_ENTRY_TEXT ("entry to be recompiled"),
					"\t cache_flag = %d\n"
					XCACHE_LOG_TRAN_TEXT,
					XCACHE_LOG_ENTRY_ARGS (to_be_recompiled),
					cache_flag, XCACHE_LOG_TRAN_ARGS (thread_p));
		      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_QPROC_INVALID_XASLNODE, 0);
		      error_code = ER_QPROC_INVALID_XASLNODE;
		      goto error;
		    }
		  new_cache_flag = (cache_flag & XCACHE_ENTRY_FIX_COUNT_MASK) | XCACHE_ENTRY_WAS_RECOMPILED;
		}
	      while (!XCACHE_ATOMIC_CAS_CACHE_FLAG (&to_be_recompiled->xasl_id, cache_flag, new_cache_flag));
	      /* We marked the entry as recompiled. */
	      xcache_log ("marked entry as recompiled: \n"
			  XCACHE_LOG_ENTRY_TEXT ("entry")
			  XCACHE_LOG_TRAN_TEXT,
			  XCACHE_LOG_ENTRY_ARGS (to_be_recompiled), XCACHE_LOG_TRAN_ARGS (thread_p));
	      xcache_unfix (thread_p, to_be_recompiled);
	      to_be_recompiled = NULL;
	    }
	  else if (inserted)
	    {
	      /* new entry added */
	      ATOMIC_INC_32 (&xcache_Entry_count, 1);
	    }

	  xcache_log ("successful find or insert: \n"
		      XCACHE_LOG_ENTRY_TEXT ("entry found or inserted")
		      "\t found or inserted = %s \n"
		      "\t recompile xasl = %s \n"
		      XCACHE_LOG_TRAN_TEXT,
		      XCACHE_LOG_ENTRY_ARGS (*xcache_entry),
		      inserted ? "inserted" : "found",
		      context->recompile_xasl ? "true" : "false", XCACHE_LOG_TRAN_ARGS (thread_p));
	  break;
	}

      assert (!inserted && context->recompile_xasl);
      assert (to_be_recompiled == NULL);
      /* We want to refresh the xasl cache entry, not to use existing. */
      /* Mark existing as to be recompiled. */
      do
	{
	  cache_flag = (*xcache_entry)->xasl_id.cache_flag;
	  if (cache_flag & XCACHE_ENTRY_MARK_DELETED)
	    {
	      /* Deleted? We certainly did not expect. */
	      assert (false);
	      xcache_log_error ("(recompile) entry is marked as deleted: \n"
				XCACHE_LOG_ENTRY_TEXT ("entry"),
				XCACHE_LOG_TRAN_TEXT,
				XCACHE_LOG_ENTRY_ARGS (*xcache_entry), XCACHE_LOG_TRAN_ARGS (thread_p));
	      xcache_unfix (thread_p, *xcache_entry);
	      *xcache_entry = NULL;
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_QPROC_INVALID_XASLNODE, 0);
	      error_code = ER_QPROC_INVALID_XASLNODE;
	      goto error;
	    }
	  if (cache_flag & (XCACHE_ENTRY_TO_BE_RECOMPILED | XCACHE_ENTRY_WAS_RECOMPILED))
	    {
	      /* Somebody else recompiles this entry. Loop again. */
	      xcache_log ("(recompile) entry is recompiled by somebody else: \n"
			  XCACHE_LOG_ENTRY_TEXT ("entry"),
			  XCACHE_LOG_TRAN_TEXT, XCACHE_LOG_ENTRY_ARGS (*xcache_entry), XCACHE_LOG_TRAN_ARGS (thread_p));
	      XCACHE_STAT_INC (failed_recompiles);
	      xcache_unfix (thread_p, *xcache_entry);
	      *xcache_entry = NULL;
	      break;
	    }
	  /* Set XCACHE_ENTRY_TO_BE_RECOMPILED to be recompiled flag. */
	  new_cache_flag = cache_flag | XCACHE_ENTRY_TO_BE_RECOMPILED;
	}
      while (!XCACHE_ATOMIC_CAS_CACHE_FLAG (&(*xcache_entry)->xasl_id, cache_flag, new_cache_flag));

      if (*xcache_entry != NULL)
	{
	  /* We have marked this entry to be recompiled. We have to insert new and then we will mark it as recompiled.
	   */
	  to_be_recompiled = *xcache_entry;
	  *xcache_entry = NULL;
	  XCACHE_STAT_INC (recompiles);

	  xcache_log ("(recompile) we marked entry to be recompiled: \n"
		      XCACHE_LOG_ENTRY_TEXT ("entry")
		      XCACHE_LOG_TRAN_TEXT, XCACHE_LOG_ENTRY_ARGS (to_be_recompiled), XCACHE_LOG_TRAN_ARGS (thread_p));

	  /* We have to "inherit" the time_stored from this entry. The new XASL cache entry should be usable by anyone
	   * that cached this entry on client. Currently, xcache_find_xasl_id uses time_stored to match the entries.
	   */
	  stream->xasl_id->time_stored = to_be_recompiled->xasl_id.time_stored;

	  /* On next find or insert, we want to skip the to be recompiled entry. */
	  xid.cache_flag = XCACHE_ENTRY_SKIP_TO_BE_RECOMPILED;
	  /* We don't unfix yet. */
	}
      else
	{
#if defined (SERVER_MODE)
	  /* Failed marking entry for recompile; we need to try again, but just to avoid burning CPU sleep a little
	   * first.
	   */
	  thread_sleep (1);
#endif /* SERVER_MODE */
	}
      /* Try to insert again. */
    }
  /* Found or inserted entry. */
  assert (*xcache_entry != NULL);

  if (!inserted)
    {
      /* Free allocated resources. */
      if (related_objects)
	{
	  free (related_objects);
	}
      if (sql_hash_text)
	{
	  free (sql_hash_text);
	}
      free_and_init (stream->xasl_stream);
    }
  else
    {
      if (xcache_need_cleanup () != XCACHE_CLEANUP_NONE && xcache_Cleanup_flag == 0)
	{
	  /* Try to clean up some of the oldest entries. */
	  xcache_cleanup (thread_p);
	}

      /* XASL stream was used. Remove from argument. */
      stream->xasl_stream = NULL;
    }

  return NO_ERROR;

error:
  assert (error_code != NO_ERROR);
  ASSERT_ERROR ();
  if ((*xcache_entry) != NULL)
    {
      (void) lf_freelist_retire (t_entry, &xcache_Ht_freelist, *xcache_entry);
    }
  if (to_be_recompiled)
    {
      /* Remove to be recompiled flag. */
      do
	{
	  cache_flag = to_be_recompiled->xasl_id.cache_flag;
	  new_cache_flag = cache_flag & (~XCACHE_ENTRY_TO_BE_RECOMPILED);
	}
      while (!XCACHE_ATOMIC_CAS_CACHE_FLAG (&to_be_recompiled->xasl_id, cache_flag, new_cache_flag));
      xcache_unfix (thread_p, to_be_recompiled);
    }
  if (related_objects)
    {
      free (related_objects);
    }
  if (sql_hash_text)
    {
      free (sql_hash_text);
    }
  return error_code;
}

/*
 * xcache_invalidate_entries () - Invalidate all cache entries which pass the invalidation check. If there is no
 *				  invalidation check, all cache entries are removed.
 *
 * return		 : Void.
 * thread_p (in)	 : Thread entry.
 * invalidate_check (in) : Invalidation check function.
 * arg (in)		 : Argument for invalidation check function.
 */
static void
xcache_invalidate_entries (THREAD_ENTRY * thread_p, bool (*invalidate_check) (XASL_CACHE_ENTRY *, void *), void *arg)
{
#define XCACHE_DELETE_XIDS_SIZE 1024
  LF_HASH_TABLE_ITERATOR iter;
  LF_TRAN_ENTRY *t_entry = thread_get_tran_entry (thread_p, THREAD_TS_XCACHE);
  XASL_CACHE_ENTRY *xcache_entry = NULL;
  bool can_delete = false;
  int success;
  XASL_ID delete_xids[XCACHE_DELETE_XIDS_SIZE];
  int n_delete_xids = 0;
  int xid_index = 0;
  bool finished = false;

  if (!xcache_Enabled)
    {
      return;
    }

  while (!finished)
    {
      /* Create iterator. */
      lf_hash_create_iterator (&iter, t_entry, &xcache_Ht);

      /* Iterate through hash, check entry OID's and if one matches the argument, mark the entry for delete and save
       * it in delete_xids buffer. We cannot delete them from hash while iterating, because the one lock-free
       * transaction can be used for one hash entry only.
       */
      while (true)
	{
	  xcache_entry = (XASL_CACHE_ENTRY *) lf_hash_iterate (&iter);
	  if (xcache_entry == NULL)
	    {
	      finished = true;
	      break;
	    }

	  /* Check invalidation conditions. */
	  if (invalidate_check == NULL || invalidate_check (xcache_entry, arg))
	    {
	      /* Mark entry as deleted. */
	      if (xcache_entry_mark_deleted (thread_p, xcache_entry))
		{
		  /* 
		   * Successfully marked for delete. Save it to delete after the iteration.
		   * No need to acquire the clone mutex, since I'm the unique user.
		   */
		  while (xcache_entry->n_cache_clones > 0)
		    {
		      xcache_clone_decache (thread_p, &xcache_entry->cache_clones[--xcache_entry->n_cache_clones]);
		    }
		  delete_xids[n_delete_xids++] = xcache_entry->xasl_id;
		}
	    }

	  if (n_delete_xids == XCACHE_DELETE_XIDS_SIZE)
	    {
	      /* Full buffer. Interrupt iteration and we'll start over. */
	      lf_tran_end_with_mb (t_entry);

	      xcache_log ("xcache_remove_by_oid full buffer\n" XCACHE_LOG_TRAN_TEXT, XCACHE_LOG_TRAN_ARGS (thread_p));

	      break;
	    }
	}

      /* Remove collected entries. */
      for (xid_index = 0; xid_index < n_delete_xids; xid_index++)
	{
	  if (lf_hash_delete (t_entry, &xcache_Ht, &delete_xids[xid_index], &success) != NO_ERROR)
	    {
	      assert (false);
	    }
	  if (success == 0)
	    {
	      /* I don't think this is expected. */
	      assert (false);
	    }
	}
      n_delete_xids = 0;
    }

#undef XCACHE_DELETE_XIDS_SIZE
}

/*
 * xcache_entry_is_related_to_oid () - Is XASL cache entry related to the OID given as argument.
 *
 * return	     : True if entry is related, false otherwise.
 * xcache_entry (in) : XASL cache entry.
 * arg (in)	     : Pointer to OID.
 */
static bool
xcache_entry_is_related_to_oid (XASL_CACHE_ENTRY * xcache_entry, void *arg)
{
  OID *related_to_oid = (OID *) arg;
  int oid_idx = 0;

  assert (xcache_entry != NULL);
  assert (related_to_oid != NULL);

  for (oid_idx = 0; oid_idx < xcache_entry->n_related_objects; oid_idx++)
    {
      if (OID_EQ (&xcache_entry->related_objects[oid_idx].oid, related_to_oid))
	{
	  /* Found relation. */
	  return true;
	}
    }
  /* Not related. */
  return false;
}

/*
 * xcache_remove_by_oid () - Remove all XASL cache entries related to given object.
 *
 * return	 : Void.
 * thread_p (in) : Thread entry.
 * oid (in)	 : Object ID.
 */
void
xcache_remove_by_oid (THREAD_ENTRY * thread_p, OID * oid)
{
  xcache_check_logging ();

  xcache_log ("remove all entries: \n"
	      "\t OID = %d|%d|%d \n" XCACHE_LOG_TRAN_TEXT, OID_AS_ARGS (oid), XCACHE_LOG_TRAN_ARGS (thread_p));
  xcache_invalidate_entries (thread_p, xcache_entry_is_related_to_oid, oid);
}

/*
 * xcache_drop_all () - Remove all entries from XASL cache.
 *
 * return	 : Void.
 * thread_p (in) : Thread entry.
 */
void
xcache_drop_all (THREAD_ENTRY * thread_p)
{
  xcache_check_logging ();

  xcache_log ("drop all queries \n" XCACHE_LOG_TRAN_TEXT, XCACHE_LOG_TRAN_ARGS (thread_p));
  xcache_invalidate_entries (thread_p, NULL, NULL);
}

/*
 * xcache_dump () - Dump XASL cache.
 *
 * return	 : Void.
 * thread_p (in) : Thread entry.
 * fp (out)	 : Output.
 */
void
xcache_dump (THREAD_ENTRY * thread_p, FILE * fp)
{
  LF_HASH_TABLE_ITERATOR iter;
  LF_TRAN_ENTRY *t_entry = thread_get_tran_entry (thread_p, THREAD_TS_XCACHE);
  XASL_CACHE_ENTRY *xcache_entry = NULL;
  int oid_index;
  char *sql_id = NULL;

  assert (fp);

  fprintf (fp, "\n");

  if (!xcache_Enabled)
    {
      fprintf (fp, "XASL cache is disabled.\n");
      return;
    }

  /* NOTE: While dumping information, other threads are still free to modify the existing entries. */

  fprintf (fp, "XASL cache\n");
  fprintf (fp, "Stats: \n");
  fprintf (fp, "Max size:                   %d\n", xcache_Soft_capacity);
  fprintf (fp, "Current entry count:        %d\n", ATOMIC_INC_32 (&xcache_Entry_count, 0));
  fprintf (fp, "Lookups:                    %ld\n", XCACHE_STAT_GET (lookups));
  fprintf (fp, "Hits:                       %ld\n", XCACHE_STAT_GET (hits));
  fprintf (fp, "Miss:                       %ld\n", XCACHE_STAT_GET (miss));
  fprintf (fp, "Inserts:                    %ld\n", XCACHE_STAT_GET (inserts));
  fprintf (fp, "Found at insert:            %ld\n", XCACHE_STAT_GET (found_at_insert));
  fprintf (fp, "Recompiles:                 %ld\n", XCACHE_STAT_GET (recompiles));
  fprintf (fp, "Failed recompiles:          %ld\n", XCACHE_STAT_GET (failed_recompiles));
  fprintf (fp, "Deletes:                    %ld\n", XCACHE_STAT_GET (deletes));
  fprintf (fp, "Fix:                        %ld\n", XCACHE_STAT_GET (fix));
  fprintf (fp, "Unfix:                      %ld\n", XCACHE_STAT_GET (unfix));
  fprintf (fp, "Cache cleanups:             %ld\n", XCACHE_STAT_GET (cleanups));
  fprintf (fp, "Deletes at cleanup:	    %ld\n", XCACHE_STAT_GET (deletes_at_cleanup));
  /* add overflow, RT checks. */

  fprintf (fp, "\nEntries:\n");
  lf_hash_create_iterator (&iter, t_entry, &xcache_Ht);
  while ((xcache_entry = (XASL_CACHE_ENTRY *) lf_hash_iterate (&iter)) != NULL)
    {
      fprintf (fp, "\n");
      fprintf (fp, "  XASL_ID = { \n");
      fprintf (fp, "              sha1 = { %08x %08x %08x %08x %08x }, \n", SHA1_AS_ARGS (&xcache_entry->xasl_id.sha1));
      fprintf (fp, "	          time_stored = %d sec, %d usec \n",
	       xcache_entry->xasl_id.time_stored.sec, xcache_entry->xasl_id.time_stored.usec);
      fprintf (fp, "            } \n");
      fprintf (fp, "  fix_count = %d \n", xcache_entry->xasl_id.cache_flag & XCACHE_ENTRY_FIX_COUNT_MASK);
      fprintf (fp, "  cache flags = %08x \n", xcache_entry->xasl_id.cache_flag & XCACHE_ENTRY_FLAGS_MASK);
      fprintf (fp, "  reference count = %ld \n", ATOMIC_INC_64 (&xcache_entry->ref_count, 0));
      fprintf (fp, "  time second last used = %lld \n", (long long) xcache_entry->time_last_used.tv_sec);
      if (xcache_uses_clones ())
	{
	  fprintf (fp, "  clone count = %d \n", xcache_entry->n_cache_clones);
	}
      fprintf (fp, "  sql info: \n");

      qmgr_get_sql_id (thread_p, &sql_id, xcache_entry->sql_info.sql_hash_text,
		       strlen (xcache_entry->sql_info.sql_hash_text));
      fprintf (fp, "    SQL_ID = %s \n", sql_id ? sql_id : "(UNKNOWN)");
      if (sql_id != NULL)
	{
	  free_and_init (sql_id);
	}

      fprintf (fp, "    sql user text = %s \n", xcache_entry->sql_info.sql_user_text);
      fprintf (fp, "    sql hash text = %s \n", xcache_entry->sql_info.sql_hash_text);
      if (prm_get_bool_value (PRM_ID_SQL_TRACE_EXECUTION_PLAN) == true)
	{
	  fprintf (fp, "    sql plan text = %s \n",
		   xcache_entry->sql_info.sql_plan_text ? xcache_entry->sql_info.sql_plan_text : "(NONE)");
	}

      fprintf (fp, "  OID_LIST (count = %d): \n", xcache_entry->n_related_objects);
      for (oid_index = 0; oid_index < xcache_entry->n_related_objects; oid_index++)
	{
	  fprintf (fp, "    OID = %d|%d|%d, LOCK = %s, TCARD = %8d \n",
		   OID_AS_ARGS (&xcache_entry->related_objects[oid_index].oid),
		   LOCK_TO_LOCKMODE_STRING (xcache_entry->related_objects[oid_index].lock),
		   xcache_entry->related_objects[oid_index].tcard);
	}
    }

  /* TODO: add more */
}

/*
 * xcache_can_entry_cache_list () - Can entry cache list files?
 *
 * return	     : True/false.
 * xcache_entry (in) : XASL cache entry.
 */
bool
xcache_can_entry_cache_list (XASL_CACHE_ENTRY * xcache_entry)
{
  if (!xcache_Enabled)
    {
      return false;
    }
  return (xcache_entry != NULL && (xcache_entry->xasl_id.cache_flag & XCACHE_ENTRY_FLAGS_MASK) == 0);
}

/*
 * xcache_clone_decache () - Free cached XASL clone resources.
 *
 * return	   : Void.
 * thread_p (in)   : Thread entry.
 * xclone (in/out) : XASL cache clone.
 */
static void
xcache_clone_decache (THREAD_ENTRY * thread_p, XASL_CLONE * xclone)
{
  HL_HEAPID save_heapid = db_change_private_heap (thread_p, 0);
  XASL_SET_FLAG (xclone->xasl, XASL_DECACHE_CLONE);
  qexec_clear_xasl (thread_p, xclone->xasl, true);
  stx_free_additional_buff (thread_p, xclone->xasl_buf);
  stx_free_xasl_unpack_info (xclone->xasl_buf);
  db_private_free (thread_p, xclone->xasl_buf);
  xclone->xasl_buf = NULL;
  xclone->xasl = NULL;
  (void) db_change_private_heap (thread_p, save_heapid);
}

/*
 * xcache_retire_clone () - Retire XASL clone. If clones caches are enabled, first try to cache it in xcache_entry.
 *
 * return	     : Void.
 * thread_p (in)     : Thread entry.
 * xcache_entry (in) : XASL cache entry.
 * xclone (in)	     : XASL clone.
 */
void
xcache_retire_clone (THREAD_ENTRY * thread_p, XASL_CACHE_ENTRY * xcache_entry, XASL_CLONE * xclone)
{
  /* Free XASL. Be sure that was already cleared to avoid memory leaks. */
  assert (xclone->xasl->status == XASL_CLEARED || xclone->xasl->status == XASL_INITIALIZED);

  if (xcache_uses_clones ())
    {
      pthread_mutex_lock (&xcache_entry->cache_clones_mutex);
      if (xcache_entry->n_cache_clones < xcache_Max_clones)
	{
	  if (xcache_entry->n_cache_clones == xcache_entry->cache_clones_capacity
	      && xcache_entry->cache_clones_capacity < xcache_Max_clones)
	    {
	      /* Extend cache clone buffer. */
	      XASL_CLONE *new_clones = NULL;
	      int new_capacity = MIN (xcache_Max_clones, xcache_entry->cache_clones_capacity * 2);
	      if (xcache_entry->cache_clones == &xcache_entry->one_clone)
		{
		  assert (xcache_entry->cache_clones_capacity == 1);
		  new_clones = (XASL_CLONE *) malloc (new_capacity * sizeof (XASL_CLONE));
		  if (new_clones != NULL)
		    {
		      new_clones[0].xasl = xcache_entry->cache_clones[0].xasl;
		      new_clones[0].xasl_buf = xcache_entry->cache_clones[0].xasl_buf;
		    }
		}
	      else
		{
		  new_clones = (XASL_CLONE *) realloc (xcache_entry->cache_clones, new_capacity * sizeof (XASL_CLONE));
		}
	      if (new_clones == NULL)
		{
		  /* Out of memory? */
		  er_set (ER_NOTIFICATION_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1,
			  new_capacity * sizeof (XASL_CLONE));
		  assert (false);
		  pthread_mutex_unlock (&xcache_entry->cache_clones_mutex);

		  /* Free the clone. */
		  xcache_clone_decache (thread_p, xclone);
		  return;
		}
	      xcache_entry->cache_clones = new_clones;
	      xcache_entry->cache_clones_capacity = new_capacity;
	    }
	  assert (xcache_entry->cache_clones_capacity > xcache_entry->n_cache_clones);
	  xcache_entry->cache_clones[xcache_entry->n_cache_clones++] = *xclone;
	  pthread_mutex_unlock (&xcache_entry->cache_clones_mutex);

	  xclone->xasl = NULL;
	  xclone->xasl_buf = NULL;
	  return;
	}
      pthread_mutex_unlock (&xcache_entry->cache_clones_mutex);

      /* No more room. */
      xcache_clone_decache (thread_p, xclone);
      return;
    }

  stx_free_additional_buff (thread_p, xclone->xasl_buf);
  stx_free_xasl_unpack_info (xclone->xasl_buf);
  db_private_free (thread_p, xclone->xasl_buf);
  xclone->xasl_buf = NULL;
  xclone->xasl = NULL;
}

/*
 * xcache_cleanup () - Cleanup xasl cache when soft capacity is exceeded.
 *
 * return	 : Void.
 * thread_p (in) : Thread entry.
 */
static void
xcache_cleanup (THREAD_ENTRY * thread_p)
{
  LF_HASH_TABLE_ITERATOR iter;
  LF_TRAN_ENTRY *t_entry = thread_get_tran_entry (thread_p, THREAD_TS_XCACHE);
  XASL_CACHE_ENTRY *xcache_entry = NULL;
  XCACHE_CLEANUP_CANDIDATE candidate;
  struct timeval current_time;
  int need_cleanup;
  int candidate_index;
  int success, count;
  int cleanup_count;
  BINARY_HEAP *bh = NULL;
  int save_max_capacity = 0;

  /* We can allow only one cleanup process at a time. There is no point in duplicating this work. Therefore, anyone
   * trying to do the cleanup should first try to set xcache_Cleanup_flag. */
  if (!ATOMIC_CAS_32 (&xcache_Cleanup_flag, 0, 1))
    {
      /* Somebody else does the cleanup. */
      return;
    }

  need_cleanup = xcache_need_cleanup ();
  if (need_cleanup == XCACHE_CLEANUP_NONE)
    {
      /* Already cleaned up. */
      if (!ATOMIC_CAS_32 (&xcache_Cleanup_flag, 1, 0))
	{
	  assert_release (false);
	}
      return;
    }

  xcache_log ("cleanup start: entries = %d \n"
	      XCACHE_LOG_TRAN_TEXT, xcache_Entry_count, XCACHE_LOG_TRAN_ARGS (thread_p));

  if (need_cleanup == XCACHE_CLEANUP_FULL)	/* cleanup because there are too many entries */
    {
      cleanup_count = (int) (XCACHE_CLEANUP_RATIO * xcache_Soft_capacity) + (xcache_Entry_count - xcache_Soft_capacity);
      /* Start cleanup. */
      perfmon_inc_stat (thread_p, PSTAT_PC_NUM_FULL);

      if (cleanup_count <= 0)
	{
	  /* Not enough to cleanup */
	  if (!ATOMIC_CAS_32 (&xcache_Cleanup_flag, 1, 0))
	    {
	      assert_release (false);
	    }
	  return;
	}
      /* Can we use preallocated binary heap? */
      if (cleanup_count <= xcache_Cleanup_bh->max_capacity)
	{
	  bh = xcache_Cleanup_bh;
	  /* Hack binary heap max capacity to the desired cleanup count. */
	  save_max_capacity = bh->max_capacity;
	  bh->max_capacity = cleanup_count;
	}
      else
	{
	  /* We need a larger binary heap. */
	  bh =
	    bh_create (thread_p, cleanup_count, sizeof (XCACHE_CLEANUP_CANDIDATE), xcache_compare_cleanup_candidates,
		       NULL);
	  if (bh == NULL)
	    {
	      /* Not really expected */
	      assert (false);
	      if (!ATOMIC_CAS_32 (&xcache_Cleanup_flag, 1, 0))
		{
		  assert_release (false);
		}
	      return;
	    }
	}

      /* The cleanup is a two-step process:
       * 1. Iterate through hash and select candidates for cleanup. The least recently used entries are sorted into a binary
       *    heap.
       *    NOTE: the binary heap does not story references to hash entries; it stores copies from the candidate keys and
       *    last used timer of course to sort the candidates.
       * 2. Remove collected candidates from hash. Entries must be unfix and no flags must be set.
       */

      assert (bh->element_count == 0);
      bh->element_count = 0;

      /* Collect candidates for cleanup. */
      lf_hash_create_iterator (&iter, t_entry, &xcache_Ht);

      while ((xcache_entry = (XASL_CACHE_ENTRY *) lf_hash_iterate (&iter)) != NULL)
	{
	  candidate.xid = xcache_entry->xasl_id;
	  candidate.time_last_used = xcache_entry->time_last_used;

	  if (candidate.xid.cache_flag & XCACHE_ENTRY_FLAGS_MASK)
	    {
	      /* Either marked for delete or recompile, or already recompiled. Not a valid candidate. */
	      continue;
	    }

	  (void) bh_try_insert (bh, &candidate, NULL);
	}

      count = bh->element_count;
    }
  else
    {
      count = 0;
      /* Collect candidates for cleanup. */
      lf_hash_create_iterator (&iter, t_entry, &xcache_Ht);
      gettimeofday (&current_time, NULL);

      while ((xcache_entry = (XASL_CACHE_ENTRY *) lf_hash_iterate (&iter)) != NULL && count < xcache_Soft_capacity)
	{
	  candidate.xid = xcache_entry->xasl_id;
	  candidate.time_last_used = xcache_entry->time_last_used;

	  if (candidate.xid.cache_flag & XCACHE_ENTRY_FLAGS_MASK
	      || TIME_DIFF_SEC (current_time, candidate.time_last_used) <= xcache_Time_threshold)
	    {
	      continue;
	    }
	  xcache_Cleanup_array[count] = candidate;
	  count++;
	}
    }

  xcache_log ("cleanup collected entries = %d \n" XCACHE_LOG_TRAN_TEXT, count, XCACHE_LOG_TRAN_ARGS (thread_p));

  /* Remove candidates from cache. */
  for (candidate_index = 0; candidate_index < count; candidate_index++)
    {
      if (need_cleanup == XCACHE_CLEANUP_FULL)	/* binary heap for candidates */
	{
	  /* Get candidate at candidate_index. */
	  bh_element_at (bh, candidate_index, &candidate);
	}
      else
	{
	  candidate = xcache_Cleanup_array[candidate_index];
	}
      /* Set intention to cleanup the entry. */
      candidate.xid.cache_flag = XCACHE_ENTRY_CLEANUP;

      /* Try delete. Would be better to decache the clones here. For simplicity, since is not an usual case,
       * clone decache is postponed - is decached when retired list will be cleared.
       */
      if (lf_hash_delete (t_entry, &xcache_Ht, &candidate.xid, &success) != NO_ERROR)
	{
	  ASSERT_ERROR ();
	  xcache_log_error ("failed hash delete: \n"
			    XCACHE_LOG_XASL_ID_TEXT ("xasl id")
			    XCACHE_LOG_TRAN_TEXT,
			    XCACHE_LOG_XASL_ID_ARGS (&candidate.xid), XCACHE_LOG_TRAN_ARGS (thread_p));
	  continue;
	}
      if (success)
	{
	  xcache_log ("cleanup: candidate was removed from hash"
		      XCACHE_LOG_XASL_ID_TEXT ("xasl id")
		      XCACHE_LOG_TRAN_TEXT, XCACHE_LOG_XASL_ID_ARGS (&candidate.xid), XCACHE_LOG_TRAN_ARGS (thread_p));

	  XCACHE_STAT_INC (deletes_at_cleanup);
	  perfmon_inc_stat (thread_p, PSTAT_PC_NUM_DELETE);
	  ATOMIC_INC_32 (&xcache_Entry_count, -1);
	}
      else
	{
	  xcache_log ("cleanup: candidate was not removed from hash"
		      XCACHE_LOG_XASL_ID_TEXT ("xasl id")
		      XCACHE_LOG_TRAN_TEXT, XCACHE_LOG_XASL_ID_ARGS (&candidate.xid), XCACHE_LOG_TRAN_ARGS (thread_p));
	}
    }
  if (need_cleanup == XCACHE_CLEANUP_FULL)
    {
      /* Reset binary heap. */
      bh->element_count = 0;

      if (bh != xcache_Cleanup_bh)
	{
	  /* Destroy bh */
	  bh_destroy (thread_p, bh);
	}
      else
	{
	  /* Reset binary heap max capacity. */
	  xcache_Cleanup_bh->max_capacity = save_max_capacity;
	}
    }

  xcache_log ("cleanup finished: entries = %d \n"
	      XCACHE_LOG_TRAN_TEXT, xcache_Entry_count, XCACHE_LOG_TRAN_ARGS (thread_p));

  XCACHE_STAT_INC (cleanups);

  gettimeofday (&xcache_Last_cleaned_time, NULL);
  if (!ATOMIC_CAS_32 (&xcache_Cleanup_flag, 1, 0))
    {
      assert_release (false);
    }
}

/*
 * xcache_compare_cleanup_candidates () - Compare cleanup candidates by their time_last_used. Oldest candidates are
 *					  considered "greater".
 *
 * return	   : BH_CMP_RESULT:
 *		     BH_GT if left is older.
 *		     BH_LT if right is older.
 *		     BH_EQ if left and right are equal.
 * left (in)	   : Left XCACHE cleanup candidate.
 * right (in)	   : Right XCACHE cleanup candidate.
 * ignore_arg (in) : Ignored.
 */
static BH_CMP_RESULT
xcache_compare_cleanup_candidates (const void *left, const void *right, BH_CMP_ARG ignore_arg)
{
  struct timeval left_timeval = ((XCACHE_CLEANUP_CANDIDATE *) left)->time_last_used;
  struct timeval right_timeval = ((XCACHE_CLEANUP_CANDIDATE *) right)->time_last_used;

  /* Lesser means placed in binary heap. So return BH_LT for older timeval. */
  if (left_timeval.tv_sec < right_timeval.tv_sec)
    {
      return BH_LT;
    }
  else if (left_timeval.tv_sec == right_timeval.tv_sec)
    {
      return BH_EQ;
    }
  else
    {
      return BH_GT;
    }
}

/*
 * xcache_check_recompilation_threshold () - Check if one of the related classes suffered big changes and if we should
 *					     try to recompile the query.
 *
 * return	     : True to recompile query, false otherwise.
 * thread_p (in)     : Thread entry.
 * xcache_entry (in) : XASL cache entry.
 */
static bool
xcache_check_recompilation_threshold (THREAD_ENTRY * thread_p, XASL_CACHE_ENTRY * xcache_entry)
{
  long save_secs = xcache_entry->time_last_rt_check.tv_sec;
  struct timeval crt_time;
  int relobj;
  CLS_INFO *cls_info_p = NULL;
  int npages;
  bool recompile = false;

  (void) gettimeofday (&crt_time, NULL);
  if (crt_time.tv_sec - xcache_entry->time_last_rt_check.tv_sec < XCACHE_RT_TIMEDIFF_IN_SEC)
    {
      /* Too soon. */
      return false;
    }
  if (!ATOMIC_CAS_32 (&xcache_entry->time_last_rt_check.tv_sec, save_secs, crt_time.tv_sec))
    {
      /* Somebody else started the check. */
      return false;
    }

  for (relobj = 0; relobj < xcache_entry->n_related_objects; relobj++)
    {
      if (xcache_entry->related_objects[relobj].tcard < 0)
	{
	  assert (xcache_entry->related_objects[relobj].tcard == XASL_CLASS_NO_TCARD
		  || xcache_entry->related_objects[relobj].tcard == XASL_SERIAL_OID_TCARD);
	  continue;
	}

      if (xcache_entry->related_objects[relobj].tcard >= XCACHE_RT_MAX_THRESHOLD)
	{
	  continue;
	}

      cls_info_p = catalog_get_class_info (thread_p, &xcache_entry->related_objects[relobj].oid, NULL);
      if (cls_info_p == NULL)
	{
	  /* Is this acceptable? */
	  return false;
	}
      if (HFID_IS_NULL (&cls_info_p->ci_hfid))
	{
	  /* Is this expected?? */
	  catalog_free_class_info (cls_info_p);
	  continue;
	}
      assert (!VFID_ISNULL (&cls_info_p->ci_hfid.vfid));

      if (file_get_num_user_pages (thread_p, &cls_info_p->ci_hfid.vfid, &npages) != NO_ERROR)
	{
	  ASSERT_ERROR ();
	  return false;
	}
      if (npages > XCACHE_RT_FACTOR * xcache_entry->related_objects[relobj].tcard
	  || npages < xcache_entry->related_objects[relobj].tcard / XCACHE_RT_FACTOR)
	{
	  cls_info_p->ci_time_stamp = stats_get_time_stamp ();
	  if (catalog_update_class_info (thread_p, &xcache_entry->related_objects[relobj].oid, cls_info_p, NULL, true)
	      == NULL)
	    {
	      /* Failed?? */
	    }
	  else
	    {
	      recompile = true;
	    }
	}
      catalog_free_class_info (cls_info_p);
    }
  return recompile;
}

/*
 * xcache_get_entry_count () - Returns the number of xasl cache entries
 *					     
 *
 * return : the number of xasl cache entries
 */
int
xcache_get_entry_count (void)
{
  return xcache_Global.entry_count;
}

/*
 * xcache_uses_clones () - Check whether XASL clones are used
 *
 * return : True, if XASL clones are used, false otherwise
 */
bool
xcache_uses_clones (void)
{
  return xcache_Max_clones > 0;
}
