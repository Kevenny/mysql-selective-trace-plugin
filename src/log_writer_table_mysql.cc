/* Copyright (C) 2026 selective_trace plugin authors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/*
  ---------------------------------------------------------------------
  ETAPA 4 — confirmed against a real MySQL 8.0.40 source tree (this repo's
  Docker environment, see docker/Dockerfile + scripts/build.sh) on
  2026-08-06: service names, DECLARE_BOOL_METHOD signatures and the
  registry acquire/release pattern below were cross-checked against
  include/mysql/components/services/mysql_command_services.h and the real
  reference plugin plugin/test_services/test_services_command_services.cc
  (which uses this exact service set from a plain st_mysql_plugin, not a
  component — same situation as this file). Notably: mysql_command_thread
  really exists (session-thread init/end for a plugin-owned background
  thread that never went through the server's own thread machinery — our
  exact situation), sql_errno() writes the error number through an
  out-parameter rather than returning it, and service pointer variables
  are typed SERVICE_TYPE_NO_CONST(name) (mutable), not SERVICE_TYPE(name)
  (const), in that reference code — matched here. Not yet exercised at
  runtime (that needs an actual server to INSTALL PLUGIN into — Etapa 5),
  but the code now compiles clean against the real headers.
  ---------------------------------------------------------------------

  log_writer_table_mysql — TABLE output mode. Architecture mirrors the
  MariaDB writer: a bounded queue + a dedicated background thread that
  drains it and runs the INSERTs, so the audit callback (running on the
  user's statement thread) never blocks on I/O. The self-log loop is
  broken the same way: the audit callback ignores every event raised by
  the writer thread (table_writer_is_self()).

  The log table (mysql.selective_trace_events) is created lazily by the
  writer thread and recreated if an INSERT fails because it went missing.
*/

#include <mysql/plugin.h>
#include <mysql/service_plugin_registry.h>
#include <mysql/components/services/mysql_command_services.h>

#include <pthread.h>
#include <deque>
#include <string>
#include <cstdio>

#include "log_writer_table_mysql.h"
#include "writer_recycle_policy.h"

namespace selective_trace {

#define LOG_TABLE_FQN "mysql.selective_trace_events"

static const char CREATE_LOG_TABLE_SQL[]=
  "CREATE TABLE IF NOT EXISTS " LOG_TABLE_FQN " ("
  " `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
  " `ts` DATETIME(3) NOT NULL,"
  " `conn_id` BIGINT UNSIGNED NOT NULL,"
  " `query_id` BIGINT UNSIGNED NOT NULL,"
  " `user` VARCHAR(384) NOT NULL DEFAULT '',"
  " `db` VARCHAR(192) NOT NULL DEFAULT '',"
  " `tables_involved` TEXT NOT NULL,"
  " `command` VARCHAR(32) NOT NULL DEFAULT '',"
  " `duration_ms` DOUBLE NULL,"
  " `error_code` INT NOT NULL DEFAULT 0,"
  " `query` MEDIUMTEXT NOT NULL,"
  " KEY `idx_selective_trace_ts` (`ts`)"
  ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";

static const size_t QUEUE_MAX_EVENTS= 10000;

static pthread_mutex_t q_mutex= PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cond= PTHREAD_COND_INITIALIZER;
static std::deque<std::string> *queue= NULL;
static int thread_running= 0;
static int stop_requested= 0;
static pthread_t writer_tid;

static unsigned long insert_failures= 0;
static unsigned long dropped_events= 0;
static unsigned int last_logged_errno= 0;

/*
  Bounds how many inserts a single internal connection (and its
  server-side THD) accumulates before being torn down — without this the
  server's RSS climbs without bound under sustained tracing. See
  writer_recycle_policy.h for the full explanation and measurements.

  Writer thread only, except recycles() which SHOW STATUS reads without
  synchronization — same benign unsynchronized-counter pattern already
  used by insert_failures/dropped_events above.
*/
static WriterRecyclePolicy recycle_policy;

/* ------------------------------------------------------------------------
   mysql_command_services plumbing — writer thread only. Acquired once per
   writer-thread lifetime (not once per statement): the registry lookup
   is not free, and the writer thread lives for the whole plugin lifetime.
   ------------------------------------------------------------------------ */

static SERVICE_TYPE(registry) *reg_srv= NULL;
static my_h_service h_factory= NULL;
static my_h_service h_options= NULL;
static my_h_service h_query= NULL;
static my_h_service h_query_result= NULL;
static my_h_service h_field_info= NULL;
static my_h_service h_error_info= NULL;
static my_h_service h_thread= NULL;

/* SERVICE_TYPE_NO_CONST (mutable), matching
   plugin/test_services/test_services_command_services.cc — SERVICE_TYPE
   (const-qualified) is for callers that only ever read a cached pointer,
   which is also what we do here, but the real reference code uses the
   NO_CONST variant for these plugin-owned statics, so this mirrors it. */
static SERVICE_TYPE_NO_CONST(mysql_command_factory) *com_factory= NULL;
static SERVICE_TYPE_NO_CONST(mysql_command_options) *com_options= NULL;
static SERVICE_TYPE_NO_CONST(mysql_command_query) *com_query= NULL;
static SERVICE_TYPE_NO_CONST(mysql_command_query_result) *com_query_result= NULL;
static SERVICE_TYPE_NO_CONST(mysql_command_field_info) *com_field_info= NULL;
static SERVICE_TYPE_NO_CONST(mysql_command_error_info) *com_error_info= NULL;
static SERVICE_TYPE_NO_CONST(mysql_command_thread) *com_thread= NULL;

static MYSQL_H command_h= NULL;         /* writer thread only */
static int services_acquired= 0;
static int command_thread_inited= 0;

bool table_writer_is_self()
{
  bool self;
  pthread_mutex_lock(&q_mutex);
  self= thread_running && pthread_equal(pthread_self(), writer_tid);
  pthread_mutex_unlock(&q_mutex);
  return self;
}

/* writer thread only. Acquires the named services once; NULL-checks every
   handle so a name mismatch against the real headers fails loudly (one
   error line) instead of crashing the server. */
static bool acquire_services()
{
  if (services_acquired)
    return com_factory && com_query && com_error_info && com_thread;

  reg_srv= mysql_plugin_registry_acquire();
  if (reg_srv == NULL)
  {
    fprintf(stderr, "selective_trace: mysql_plugin_registry_acquire failed\n");
    return false;
  }

  struct { const char *name; my_h_service *out; } wanted[]=
  {
    { "mysql_command_factory",       &h_factory },
    { "mysql_command_options",       &h_options },
    { "mysql_command_query",         &h_query },
    { "mysql_command_query_result",  &h_query_result },
    { "mysql_command_field_info",    &h_field_info },
    { "mysql_command_error_info",    &h_error_info },
    { "mysql_command_thread",        &h_thread }
  };
  for (size_t i= 0; i < sizeof(wanted) / sizeof(wanted[0]); i++)
  {
    if (reg_srv->acquire(wanted[i].name, wanted[i].out))
    {
      fprintf(stderr, "selective_trace: could not acquire service '%s'"
              " (name/signature drifted from the headers this was written"
              " against — see log_writer_table_mysql.cc header comment)\n",
              wanted[i].name);
      *wanted[i].out= NULL;
    }
  }

  com_factory=
    reinterpret_cast<SERVICE_TYPE_NO_CONST(mysql_command_factory) *>(h_factory);
  com_options=
    reinterpret_cast<SERVICE_TYPE_NO_CONST(mysql_command_options) *>(h_options);
  com_query=
    reinterpret_cast<SERVICE_TYPE_NO_CONST(mysql_command_query) *>(h_query);
  com_query_result=
    reinterpret_cast<SERVICE_TYPE_NO_CONST(mysql_command_query_result) *>(
        h_query_result);
  com_field_info=
    reinterpret_cast<SERVICE_TYPE_NO_CONST(mysql_command_field_info) *>(
        h_field_info);
  com_error_info=
    reinterpret_cast<SERVICE_TYPE_NO_CONST(mysql_command_error_info) *>(
        h_error_info);
  com_thread=
    reinterpret_cast<SERVICE_TYPE_NO_CONST(mysql_command_thread) *>(h_thread);

  services_acquired= 1;
  return com_factory && com_query && com_error_info && com_thread;
}

static void release_services()
{
  if (!services_acquired || reg_srv == NULL)
    return;
  if (h_factory)      reg_srv->release(h_factory);
  if (h_options)       reg_srv->release(h_options);
  if (h_query)         reg_srv->release(h_query);
  if (h_query_result)  reg_srv->release(h_query_result);
  if (h_field_info)    reg_srv->release(h_field_info);
  if (h_error_info)    reg_srv->release(h_error_info);
  if (h_thread)        reg_srv->release(h_thread);
  mysql_plugin_registry_release(reg_srv);
  reg_srv= NULL;
  services_acquired= 0;
  com_factory= NULL; com_options= NULL; com_query= NULL;
  com_query_result= NULL; com_field_info= NULL; com_error_info= NULL;
  com_thread= NULL;
}

static void close_conn()
{
  if (command_h != NULL && com_factory != NULL)
  {
    com_factory->close(command_h);
    command_h= NULL;
  }
}

/* writer thread only. mysql_command_error_info::sql_errno() writes the
   errno through an out-parameter and returns bool (true = failed to
   retrieve it) — it does not return the errno value directly. */
static unsigned int last_errno()
{
  unsigned int err= 0;
  if (com_error_info == NULL || command_h == NULL)
    return 0;
  if (com_error_info->sql_errno(command_h, &err))
    return 0;
  return err;
}

/* writer thread only. Same out-parameter convention as sql_errno(). The
   returned pointer is only valid until the next call on this command
   handle — copy it before logging if that ever changes. */
static const char *last_errmsg()
{
  char *msg= NULL;
  if (com_error_info == NULL || command_h == NULL)
    return "(no connection)";
  if (com_error_info->sql_error(command_h, &msg) || msg == NULL)
    return "(error text unavailable)";
  return msg;
}

/* writer thread only */
static bool ensure_conn()
{
  if (command_h != NULL)
    return true;

  if (!acquire_services())
    return false;

  if (!command_thread_inited)
  {
    /* Register this OS thread with the command services runtime before
       the first init()/connect() call — analogous to my_thread_init() for
       the mysql_real_connect_local() path in the MariaDB plugin.
       mysql_command_thread::init() returns bool, true = failure. */
    if (com_thread->init())
    {
      fprintf(stderr, "selective_trace: mysql_command_thread->init failed\n");
      return false;
    }
    command_thread_inited= 1;
  }

  if (com_factory->init(&command_h))
  {
    fprintf(stderr, "selective_trace: mysql_command_factory->init failed\n");
    command_h= NULL;
    return false;
  }

  /*
    Pin a known sql_mode on the writer session, matching the MariaDB
    writer: sql_escape_append() escapes with backslashes, which is only
    valid when NO_BACKSLASH_ESCAPES is off server-wide. Without this a
    server started with NO_BACKSLASH_ESCAPES would make our escaping
    ineffective — a "'" in the traced query text could break out of the
    INSERT string literal (SQL injection into the log table).
  */
  static const char set_mode[]= "SET SESSION sql_mode=''";

  if (com_factory->connect(command_h))
  {
    fprintf(stderr, "selective_trace: internal command connection failed\n");
    close_conn();
    return false;
  }

  /*
    SECURITY: this MUST be fatal, not just logged. sql_escape_append()'s
    backslash escaping is only effective when NO_BACKSLASH_ESCAPES is off
    for this session. If the SET fails and the ambient sql_mode (inherited
    from GLOBAL) has NO_BACKSLASH_ESCAPES on, escaping silently becomes a
    no-op and any traced query text containing a bare "'" would break out
    of the INSERT's string literal — SQL injection running with the
    internal writer connection's privileges. Refuse to insert rather than
    insert unsafely.
  */
  if (com_query->query(command_h, set_mode, sizeof(set_mode) - 1))
  {
    fprintf(stderr,
            "selective_trace: could not set writer sql_mode — refusing to"
            " insert (would risk SQL injection via unescaped ' in traced"
            " query text)\n");
    close_conn();
    return false;
  }
  if (com_query->query(command_h, CREATE_LOG_TABLE_SQL,
                       sizeof(CREATE_LOG_TABLE_SQL) - 1))
    fprintf(stderr, "selective_trace: could not create " LOG_TABLE_FQN
            " (errno %u: %s)\n", last_errno(), last_errmsg());

  return true;
}

/* writer thread only */
static void run_insert(const std::string &sql)
{
  if (!ensure_conn())
  {
    insert_failures++;
    return;
  }

  if (com_query->query(command_h, sql.data(), sql.size()) == 0)
    return;

  unsigned int err= last_errno();
  if (err == 1146 || err == 1049)       /* table/schema went missing */
  {
    com_query->query(command_h, CREATE_LOG_TABLE_SQL,
                     sizeof(CREATE_LOG_TABLE_SQL) - 1);
    if (com_query->query(command_h, sql.data(), sql.size()) == 0)
      return;
    err= last_errno();
  }
  else if (err == 2006 || err == 2013)  /* connection gone: retry once */
  {
    close_conn();
    /* out-of-band drop: the retry gets a brand new THD with a fresh
       arena, so the periodic recycle counter restarts from here (this
       is not counted as one of our own recycles) */
    recycle_policy.note_connection_reset();
    if (ensure_conn() &&
        com_query->query(command_h, sql.data(), sql.size()) == 0)
      return;
    err= last_errno();
  }

  insert_failures++;
  if (err != last_logged_errno)         /* don't flood the error log */
  {
    last_logged_errno= err;
    fprintf(stderr, "selective_trace: INSERT into " LOG_TABLE_FQN
            " failed (errno %u: %s)\n", err, last_errmsg());
  }
}

static void *writer_thread_func(void *arg __attribute__((unused)))
{
  std::deque<std::string> batch;

  /* A restarted writer thread always starts on a fresh connection, so
     drop any insert count left over from the previous run (the total
     recycle count is deliberately kept — it is a since-plugin-load
     statistic). */
  recycle_policy.note_connection_reset();

  for (;;)
  {
    pthread_mutex_lock(&q_mutex);
    while (queue->empty() && !stop_requested)
      pthread_cond_wait(&q_cond, &q_mutex);
    batch.swap(*queue);
    int stopping= stop_requested;
    pthread_mutex_unlock(&q_mutex);

    for (size_t i= 0; i < batch.size(); i++)
    {
      /* never let an exception kill the writer thread (and the server) */
      try
      {
        run_insert(batch[i]);
      }
      catch (...)
      {
        insert_failures++;
      }

      /* Recycle between inserts, never mid-insert. close_conn() only
         closes; the next run_insert() reopens lazily via ensure_conn(),
         so a writer that goes idle right here holds no connection at
         all until the next event arrives. */
      if (recycle_policy.note_insert())
        close_conn();
    }
    batch.clear();

    if (stopping)
      break;
  }

  close_conn();
  if (command_thread_inited && com_thread != NULL)
  {
    com_thread->end();
    command_thread_inited= 0;
  }
  release_services();
  return NULL;
}

/* caller must hold q_mutex */
static bool start_thread_locked()
{
  if (thread_running)
    return true;
  stop_requested= 0;
  if (pthread_create(&writer_tid, NULL, writer_thread_func, NULL) != 0)
  {
    fprintf(stderr, "selective_trace: could not start table writer thread\n");
    return false;
  }
  thread_running= 1;
  return true;
}

void table_writer_init()
{
  pthread_mutex_lock(&q_mutex);
  if (queue == NULL)
  {
    try
    {
      queue= new (std::nothrow) std::deque<std::string>();
    }
    catch (...)
    {
      queue= NULL;              /* enqueue() will refuse and drop */
    }
  }
  pthread_mutex_unlock(&q_mutex);
}

void table_writer_shutdown()
{
  pthread_t tid;
  int join_it= 0;

  pthread_mutex_lock(&q_mutex);
  if (thread_running)
  {
    stop_requested= 1;
    tid= writer_tid;
    join_it= 1;
    pthread_cond_signal(&q_cond);
  }
  pthread_mutex_unlock(&q_mutex);

  if (join_it)
    pthread_join(tid, NULL);

  pthread_mutex_lock(&q_mutex);
  thread_running= 0;
  delete queue;
  queue= NULL;
  pthread_mutex_unlock(&q_mutex);
}

bool table_writer_enqueue(std::string *sql)
{
  bool ok= false;
  pthread_mutex_lock(&q_mutex);
  if (queue != NULL && start_thread_locked())
  {
    if (queue->size() >= QUEUE_MAX_EVENTS)
      dropped_events++;
    else
    {
      try
      {
        queue->push_back(std::string());
        queue->back().swap(*sql);        /* swap: noexcept */
        pthread_cond_signal(&q_cond);
        ok= true;
      }
      catch (...)
      {
        dropped_events++;       /* out of memory: drop this event */
      }
    }
  }
  pthread_mutex_unlock(&q_mutex);
  return ok;
}

unsigned long table_writer_failures()
{
  return insert_failures;
}

unsigned long table_writer_reconnects()
{
  return recycle_policy.recycles();
}

unsigned long table_writer_dropped()
{
  return dropped_events;
}

} /* namespace selective_trace */
