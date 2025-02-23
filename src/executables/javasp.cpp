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


/*
 * javasp.cpp - utility java stored procedure server main routine
 *
 */

#ident "$Id$"

#include "config.h"

#if !defined(WINDOWS)
#include <dlfcn.h>
#include <execinfo.h>
#endif
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#if !defined(WINDOWS)
#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>
#include <netdb.h>
#include <signal.h>
#else /* not WINDOWS */
#include <winsock2.h>
#include <windows.h>
#include <process.h>
#include <io.h>
#endif /* not WINDOWS */

#include "environment_variable.h"
#include "system_parameter.h"
#include "error_code.h"
#include "error_manager.h"
#include "message_catalog.h"
#include "utility.h"
#include "databases_file.h"
#include "object_representation.h"
#include "method_struct_invoke.hpp"
#include "method_connection_java.hpp"

#include "jsp_file.h"
#include "jsp_sr.h"

#include "packer.hpp"

#include <string>
#include <algorithm>
#include <array>
#include <atomic>

#define JAVASP_PING_LEN   PATH_MAX

#define JAVASP_PRINT_ERR_MSG(...) \
  do {\
      fprintf (stderr, __VA_ARGS__);\
  }while (0)

#if defined(WINDOWS)
#define NULL_DEVICE "NUL:"
#else
#define NULL_DEVICE "/dev/null"
#endif

static int javasp_start_server (const JAVASP_SERVER_INFO jsp_info, const std::string &db_name, const std::string &path);
static int javasp_stop_server (const JAVASP_SERVER_INFO jsp_info, const std::string &db_name);
static int javasp_status_server (const JAVASP_SERVER_INFO jsp_info, const std::string &db_name);

static void javasp_dump_status (FILE *fp, JAVASP_STATUS_INFO status_info);
static int javasp_ping_server (const int server_port, const char *db_name, char *buf);
static bool javasp_is_running (const int server_port, const std::string &db_name);

static bool javasp_is_terminated_process (int pid);
static void javasp_terminate_process (int pid);

static int javasp_get_server_info (const std::string &db_name, JAVASP_SERVER_INFO &info);
static int javasp_check_argument (int argc, char *argv[], std::string &command, std::string &db_name);
static int javasp_check_database (const std::string &db_name, std::string &db_path);

static int javasp_get_port_param ();

#if !defined(WINDOWS)
static void javasp_signal_handler (int sig);
#endif

static bool is_signal_handling = false;
static char executable_path[PATH_MAX];

static std::string command;
static std::string db_name;
static JAVASP_SERVER_INFO running_info = JAVASP_SERVER_INFO_INITIALIZER;

/*
 * main() - javasp main function
 */

int
main (int argc, char *argv[])
{
  int status = NO_ERROR;
  FILE *redirect = NULL; /* for ping */

#if defined(WINDOWS)
  FARPROC jsp_old_hook = NULL;
#else
  if (os_set_signal_handler (SIGPIPE, SIG_IGN) == SIG_ERR)
    {
      return ER_GENERIC_ERROR;
    }

  os_set_signal_handler (SIGABRT, javasp_signal_handler);
  os_set_signal_handler (SIGILL, javasp_signal_handler);
  os_set_signal_handler (SIGFPE, javasp_signal_handler);
  os_set_signal_handler (SIGBUS, javasp_signal_handler);
  os_set_signal_handler (SIGSEGV, javasp_signal_handler);
  os_set_signal_handler (SIGSYS, javasp_signal_handler);

#endif /* WINDOWS */
  {
    /*
    * COMMON PART FOR PING AND OTHER COMMANDS
    */

    // supress error message
    er_init (NULL_DEVICE, ER_NEVER_EXIT);

    /* check arguments, get command and database name */
    status = javasp_check_argument (argc, argv, command, db_name);
    if (status != NO_ERROR)
      {
	return ER_GENERIC_ERROR;
      }

    /* check database exists and get path name of database */
    std::string pathname;
    status = javasp_check_database (db_name, pathname);
    if (status != NO_ERROR)
      {
	goto exit;
      }

    /* initialize error manager for command */
    if (command.compare ("ping") != 0)
      {
	/* finalize supressing error message for ping */
	er_final (ER_ALL_FINAL);

	/* error message log file */
	char er_msg_file[PATH_MAX];
	snprintf (er_msg_file, sizeof (er_msg_file) - 1, "%s_java.err", db_name.c_str ());
	er_init (er_msg_file, ER_NEVER_EXIT);
      }

    /* try to create info dir and get absolute path for info file; $CUBRID/var/javasp_<db_name>.info */
    JAVASP_SERVER_INFO jsp_info = JAVASP_SERVER_INFO_INITIALIZER;
    status = javasp_get_server_info (db_name, jsp_info);
    if (status != NO_ERROR && command.compare ("start") != 0)
      {
	char info_path[PATH_MAX], err_msg[PATH_MAX + 32];
	javasp_get_info_file (info_path, PATH_MAX, db_name.c_str ());
	snprintf (err_msg, sizeof (err_msg), "Error while opening file (%s)", info_path);
	er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_SP_CANNOT_START_JVM, 1, err_msg);
	goto exit;
      }

#if defined(WINDOWS)
    // socket startup for windows
    windows_socket_startup (jsp_old_hook);
#endif /* WINDOWS */

    /*
    * PROCESS PING
    */
    if (command.compare ("ping") == 0)
      {
	// redirect stderr
	if ((redirect = freopen (NULL_DEVICE, "w", stderr)) == NULL)
	  {
	    assert (false);
	    return ER_GENERIC_ERROR;
	  }

	// check process is running
	if (jsp_info.pid == JAVASP_PID_DISABLED || javasp_is_terminated_process (jsp_info.pid) == true)
	  {
	    // NO_CONNECTION
	    javasp_reset_info (db_name.c_str ());
	    goto exit;
	  }

	char buffer[JAVASP_PING_LEN] = {0};
	if (status == NO_ERROR)
	  {
	    status = javasp_ping_server (jsp_info.port, db_name.c_str (), buffer);
	  }

	if (status == NO_ERROR)
	  {
	    std::string ping_db_name;
	    packing_unpacker unpacker (buffer, JAVASP_PING_LEN);
	    unpacker.unpack_string (ping_db_name);

	    fprintf (stdout, "%s", ping_db_name.c_str ());
	  }
	else
	  {
	    goto exit;
	  }

	return status;
      }

    /*
    * BEGIN TO PROCESS FOR OTHER COMMANDS
    */

    // load system parameter
    sysprm_load_and_init (db_name.c_str (), NULL, SYSPRM_IGNORE_INTL_PARAMS);

    /* javasp command main routine */
    if (command.compare ("start") == 0)
      {
	// check java stored procedure is not enabled
	if (prm_get_bool_value (PRM_ID_JAVA_STORED_PROCEDURE) == false)
	  {
	    fprintf (stdout, "%s system parameter is not enabled\n", prm_get_name (PRM_ID_JAVA_STORED_PROCEDURE));
	    status = ER_SP_CANNOT_START_JVM;
	    goto exit;
	  }

	status = javasp_start_server (jsp_info, db_name, pathname);
	if (status == NO_ERROR)
	  {
	    command = "running";
	    javasp_read_info (db_name.c_str(), running_info);
	    do
	      {
		SLEEP_MILISEC (0, 100);
	      }
	    while (true);
	  }
      }
    else if (command.compare ("stop") == 0)
      {
	status = javasp_stop_server (jsp_info, db_name);
      }
    else if (command.compare ("status") == 0)
      {
	status = javasp_status_server (jsp_info, db_name);
      }
    else
      {
	JAVASP_PRINT_ERR_MSG ("Invalid command: %s\n", command.c_str ());
	status = ER_GENERIC_ERROR;
      }

#if defined(WINDOWS)
    // socket shutdown for windows
    windows_socket_shutdown (jsp_old_hook);
#endif /* WINDOWS */
  }

exit:

  if (command.compare ("ping") == 0)
    {
      if (status != NO_ERROR)
	{
	  fprintf (stdout, "ERROR");
	}
      else
	{
	  fprintf (stdout, "NO_CONNECTION");
	}

      if (redirect)
	{
	  fclose (redirect);
	}
    }
  else
    {
      if (er_has_error ())
	{
	  JAVASP_PRINT_ERR_MSG ("%s\n", er_msg ());
	}
    }

  return status;
}

#if !defined(WINDOWS)
static void javasp_signal_handler (int sig)
{
  JAVASP_SERVER_INFO jsp_info = JAVASP_SERVER_INFO_INITIALIZER;

  if (os_set_signal_handler (sig, SIG_DFL) == SIG_ERR)
    {
      return;
    }

  if (is_signal_handling == true)
    {
      return;
    }

  int status = javasp_get_server_info (db_name, jsp_info); // if failed,
  if (status == NO_ERROR && jsp_info.pid != JAVASP_PID_DISABLED)
    {
      (void) envvar_bindir_file (executable_path, PATH_MAX, UTIL_JAVASP_NAME);
      if (command.compare ("running") != 0 || db_name.empty ())
	{
	  return;
	}

      if (running_info.pid == jsp_info.pid && running_info.port == jsp_info.port)
	{
	  is_signal_handling = true;
	}
      else
	{
	  return;
	}

      int pid = fork ();
      if (pid == 0) // child
	{
	  execl (executable_path, UTIL_JAVASP_NAME, "start", db_name.c_str (), NULL);
	  exit (0);
	}
      else
	{
	  // error handling in parent
	  std::string err_msg;

	  void *addresses[64];
	  int nn_addresses = backtrace (addresses, sizeof (addresses) / sizeof (void *));
	  char **symbols = backtrace_symbols (addresses, nn_addresses);

	  err_msg += "pid (";
	  err_msg += std::to_string (pid);
	  err_msg += ")\n";

	  for (int i = 0; i < nn_addresses; i++)
	    {
	      err_msg += symbols[i];
	      if (i < nn_addresses - 1)
		{
		  err_msg += "\n";
		}
	    }
	  free (symbols);

	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_SP_SERVER_CRASHED, 1, err_msg.c_str ());

	  exit (1);
	}
    }
  else
    {
      // resume signal hanlding
      os_set_signal_handler (sig, javasp_signal_handler);
      is_signal_handling = false;
    }
}
#endif

static int
javasp_get_port_param ()
{
  int prm_port = 0;
#if defined (WINDOWS)
  const bool is_uds_mode = false;
#else
  const bool is_uds_mode = prm_get_bool_value (PRM_ID_JAVA_STORED_PROCEDURE_UDS);
#endif
  prm_port = (is_uds_mode) ? JAVASP_PORT_UDS_MODE : prm_get_integer_value (PRM_ID_JAVA_STORED_PROCEDURE_PORT);
  return prm_port;
}

static int
javasp_start_server (const JAVASP_SERVER_INFO jsp_info, const std::string &db_name, const std::string &path)
{
  int status = NO_ERROR;

  if (jsp_info.pid != JAVASP_PID_DISABLED && javasp_is_running (jsp_info.port, db_name))
    {
      status = ER_GENERIC_ERROR;
    }
  else
    {
#if !defined(WINDOWS)
      /* create a new session */
      setsid ();
#endif
      er_clear (); // clear error before string JVM
      status = jsp_start_server (db_name.c_str (), path.c_str (), javasp_get_port_param ());

      if (status == NO_ERROR)
	{
	  JAVASP_SERVER_INFO jsp_new_info { getpid(), jsp_server_port () };

	  javasp_unlink_info (db_name.c_str ());
	  if ((javasp_open_info_dir () && javasp_write_info (db_name.c_str (), jsp_new_info)))
	    {
	      /* succeed */
	    }
	  else
	    {
	      /* failed to write info file */
	      char info_path[PATH_MAX], err_msg[PATH_MAX + 32];
	      javasp_get_info_file (info_path, PATH_MAX, db_name.c_str ());
	      snprintf (err_msg, sizeof (err_msg), "Error while writing to file: (%s)", info_path);
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_SP_CANNOT_START_JVM, 1, err_msg);
	      status = ER_SP_CANNOT_START_JVM;
	    }
	}
    }

  return status;
}

static int
javasp_stop_server (const JAVASP_SERVER_INFO jsp_info, const std::string &db_name)
{
  SOCKET socket = INVALID_SOCKET;
  int status = NO_ERROR;

  socket = jsp_connect_server (db_name.c_str (), jsp_info.port);
  if (socket != INVALID_SOCKET)
    {
      cubmethod::header header (DB_EMPTY_SESSION, SP_CODE_UTIL_TERMINATE_SERVER, 0);
      status = mcon_send_data_to_java (socket, header);

      javasp_reset_info (db_name.c_str ());
      jsp_disconnect_server (socket);

      if (jsp_info.pid != -1 && !javasp_is_terminated_process (jsp_info.pid))
	{
	  javasp_terminate_process (jsp_info.pid);
	}

      javasp_reset_info (db_name.c_str ());
    }

  return status;
}

static int
javasp_status_server (const JAVASP_SERVER_INFO jsp_info, const std::string &db_name)
{
  int status = NO_ERROR;
  SOCKET socket = INVALID_SOCKET;
  cubmem::block buffer;

  socket = jsp_connect_server (db_name.c_str(), jsp_info.port);
  if (socket != INVALID_SOCKET)
    {
      cubmethod::header header (DB_EMPTY_SESSION, SP_CODE_UTIL_STATUS, 0);
      status = mcon_send_data_to_java (socket, header);
      if (status != NO_ERROR)
	{
	  goto exit;
	}

      status = cubmethod::mcon_read_data_from_java (socket, buffer);
      if (status != NO_ERROR)
	{
	  goto exit;
	}

      int num_args = 0;
      JAVASP_STATUS_INFO status_info;

      status_info.pid = jsp_info.pid;

      packing_unpacker unpacker (buffer.ptr, buffer.dim);

      unpacker.unpack_int (status_info.port);
      unpacker.unpack_string (status_info.db_name);
      unpacker.unpack_int (num_args);
      std::string arg;
      for (int i = 0; i < num_args; i++)
	{
	  unpacker.unpack_string (arg);
	  status_info.vm_args.push_back (arg);
	}

      javasp_dump_status (stdout, status_info);
    }

exit:
  jsp_disconnect_server (socket);

  if (buffer.ptr)
    {
      free_and_init (buffer.ptr);
    }

  return status;
}

static int
javasp_ping_server (const int server_port, const char *db_name, char *buf)
{
  int status = NO_ERROR;
  SOCKET socket = INVALID_SOCKET;
  cubmem::block ping_blk {0, NULL};

  socket = jsp_connect_server (db_name, server_port);
  if (socket != INVALID_SOCKET)
    {
      cubmethod::header header (DB_EMPTY_SESSION, SP_CODE_UTIL_PING, 0);
      status = mcon_send_data_to_java (socket, header);
      if (status != NO_ERROR)
	{
	  goto exit;
	}

      status = cubmethod::mcon_read_data_from_java (socket, ping_blk);
      if (status != NO_ERROR)
	{
	  goto exit;
	}
      memcpy (buf, ping_blk.ptr, ping_blk.dim);
    }

exit:
  if (ping_blk.is_valid ())
    {
      delete [] ping_blk.ptr;
    }

  jsp_disconnect_server (socket);
  return er_errid ();
}

static void
javasp_dump_status (FILE *fp, JAVASP_STATUS_INFO status_info)
{
  if (status_info.port == JAVASP_PORT_UDS_MODE)
    {
      fprintf (fp, "Java Stored Procedure Server (%s, pid %d, UDS)\n", status_info.db_name.c_str (), status_info.pid);
    }
  else
    {
      fprintf (fp, "Java Stored Procedure Server (%s, pid %d, port %d)\n", status_info.db_name.c_str (), status_info.pid,
	       status_info.port);
    }
  auto vm_args_len = status_info.vm_args.size();
  if (vm_args_len > 0)
    {
      fprintf (fp, "Java VM arguments :\n");
      fprintf (fp, " -------------------------------------------------\n");
      for (int i = 0; i < (int) vm_args_len; i++)
	{
	  fprintf (fp, "  %s\n", status_info.vm_args[i].c_str());
	}
      fprintf (fp, " -------------------------------------------------\n");
    }
}

static bool
javasp_is_running (const int server_port, const std::string &db_name)
{
  // check server running
  bool result = false;
  char buffer[JAVASP_PING_LEN] = {0};
  if (javasp_ping_server (server_port, db_name.c_str (), buffer) == NO_ERROR)
    {
      if (db_name.compare (0, db_name.size (), buffer) == 0)
	{
	  result = true;
	}
    }
  return result;
}

/*
 * javasp_is_terminated_process () -
 *   return:
 *   pid(in):
 *   TODO there exists same function in file_io.c and util_service.c
 */
static bool
javasp_is_terminated_process (int pid)
{
#if defined(WINDOWS)
  HANDLE h_process;

  h_process = OpenProcess (PROCESS_QUERY_INFORMATION, FALSE, pid);
  if (h_process == NULL)
    {
      return true;
    }
  else
    {
      CloseHandle (h_process);
      return false;
    }
#else /* WINDOWS */
  if (kill (pid, 0) == -1)
    {
      return true;
    }
  else
    {
      return false;
    }
#endif /* WINDOWS */
}

static void
javasp_terminate_process (int pid)
{
#if defined(WINDOWS)
  HANDLE phandle;

  phandle = OpenProcess (PROCESS_TERMINATE, FALSE, pid);
  if (phandle)
    {
      TerminateProcess (phandle, 0);
      CloseHandle (phandle);
    }
#else /* ! WINDOWS */
  kill (pid, SIGTERM);
#endif /* ! WINDOWS */
}

static int
javasp_get_server_info (const std::string &db_name, JAVASP_SERVER_INFO &info)
{
  if (javasp_open_info_dir ()
      && javasp_read_info (db_name.c_str(), info))
    {
      return NO_ERROR;
    }
  else
    {
      return ER_GENERIC_ERROR;
    }
}

static int
javasp_check_database (const std::string &db_name, std::string &path)
{
  int status = NO_ERROR;

  /* check database exists */
  DB_INFO *db = cfg_find_db (db_name.c_str ());
  if (db == NULL)
    {
      status = ER_GENERIC_ERROR;
    }
  else
    {
      path.assign (db->pathname);
      cfg_free_directory (db);
    }

  return status;
}

static int
javasp_check_argument (int argc, char *argv[], std::string &command, std::string &db_name)
{
  int status = NO_ERROR;

  /* check argument number */
  if (argc == 3)
    {
      command.assign (argv[1]);
      db_name.assign (argv[2]);
    }
  else if (argc == 2)
    {
      command.assign ("start");
      db_name.assign (argv[1]);
    }
  else
    {
      status = ER_GENERIC_ERROR;
      JAVASP_PRINT_ERR_MSG ("Invalid number of arguments: %d\n", argc);
    }

  if (status == NO_ERROR)
    {
      /* check command */
      std::array<std::string, 5> commands = {"start", "stop", "restart", "status", "ping"};
      auto it = find (commands.begin(), commands.end(), command);
      if (it == commands.end())
	{
	  status = ER_GENERIC_ERROR;
	  JAVASP_PRINT_ERR_MSG ("Invalid command: %s\n", command.c_str ());
	}
    }

  return status;
}
