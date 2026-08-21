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
  selective_trace — selective query tracing for MySQL 8.0+

  Traces (logs) queries touching a configurable set of schemas and/or
  tables, filtered by command type — a low-overhead, partial alternative
  to general_log (which is all-or-nothing). Ported from the MariaDB
  plugin of the same name; see CLAUDE.md and docs/DECISIONS.md for the
  API differences that shaped this file.

  Event flow per statement (see docs/RESEARCH_NOTES_MYSQL.md):

    MYSQL_AUDIT_GENERAL_LOG        statement dispatch starts: stamp the
                                   per-connection state.
    MYSQL_AUDIT_TABLE_ACCESS_*     one event per table touched by DML
                                   (READ/INSERT/UPDATE/DELETE only — see
                                   the "DDL gap" note below): match
                                   against the filter, accumulate names.
    MYSQL_AUDIT_GENERAL_STATUS     statement finished: decide (table
                                   match OR session-schema match OR
                                   connection match), apply min_duration,
                                   assemble the output and write it.

  *** Known architectural gap vs. the MariaDB plugin (read before relying
  on schema/table filtering for DDL) ***
  MySQL 8.0's TABLE_ACCESS_CLASS only fires for MYSQL_AUDIT_TABLE_ACCESS_
  {READ,INSERT,UPDATE,DELETE} — i.e. DML. CREATE/ALTER/DROP/TRUNCATE/
  RENAME TABLE do not raise a TABLE_ACCESS event, so this plugin cannot
  see the table name of a DDL statement (unlike the MariaDB build, whose
  TABLE_LOCK event covers table-opening generically). DDL/TCL/other
  statements can only be selected by two things: (1) the connection
  filter (selective_trace_connections — always exact regardless of
  command), and (2) a best-effort *current schema* tracked from Init DB
  commands and "USE x" statements (see track_current_db() below) matched
  against selective_trace_schemas. schema.table:ddl-style entries will
  never match a DDL statement, because there is no table-level DDL event
  to match them against. This is a MySQL Audit API limitation, not a bug
  in this plugin — see docs/DECISIONS.md for the full writeup and for
  what would be needed to close the gap (e.g. combining with the
  PARSE_CLASS event, or with a table_definition_cache hook — future
  work, not attempted here).

  Filters are immutable FilterRules snapshots swapped under a write lock;
  the hot path only takes read locks, so queries never serialize.
*/

#define PLUGIN_VERSION      0x0100
#define PLUGIN_STR_VERSION  "1.0.0"

#include <mysql/plugin.h>
#include <mysql/plugin_audit.h>
#include <mysql/psi/mysql_thread.h>
#include <mysql/psi/mysql_rwlock.h>  /* mysql_thread.h does NOT pull this in */
#include <mysqld_error.h>
#include <template_utils.h>   /* array_elements() — a template in 8.0, not a macro */
#include <typelib.h>           /* TYPELIB, used by MYSQL_SYSVAR_ENUM */

#include <new>
#include <string>
#include <unordered_map>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/time.h>

#include "core/filter_engine.h"
#include "log_writer_file_mysql.h"
#include "log_writer_table_mysql.h"

using selective_trace::FilterRules;

/* ------------------------------------------------------------------------
   Filter state, shared by all connections
   ------------------------------------------------------------------------ */

static mysql_rwlock_t filter_lock;
static FilterRules *active_rules= NULL;
static std::string *schemas_storage= NULL;
static std::string *tables_storage= NULL;
static std::string *connections_storage= NULL;
static std::string *file_path_storage= NULL;
static int plugin_ready= 0;
/* exceptions swallowed at the C boundaries (SHOW STATUS, sysvar checks) */
static ulong status_callback_errors= 0;

#ifdef HAVE_PSI_INTERFACE
static PSI_rwlock_key key_rwlock_filter;
static PSI_rwlock_info rwlock_key_list[]=
{
  { &key_rwlock_filter, "selective_trace::filter_lock", 0, 0, PSI_DOCUMENT_ME }
};
#else
#define key_rwlock_filter 0
#endif

/* ------------------------------------------------------------------------
   Per-connection statement state

   *** HISTORY — read before touching this block ***
   The first version of this file used the same technique as the MariaDB
   plugin: a hidden MYSQL_THDVAR_STR whose default value is a NUL-free
   blob the server strdup()s once per connection (PLUGIN_VAR_MEMALLOC),
   giving a per-THD POD buffer freed automatically with the THD. That
   trick does **not** work the same way on MySQL 8.0/9.x: confirmed live
   against a real mysqld 8.0.40 (Docker, INSTALL PLUGIN, then
   SET GLOBAL selective_trace_enabled=ON) — THDVAR(thd, state) returned an
   invalid pointer and the very first dereference (st->magic) crashed the
   server with SIGSEGV inside get_state(), reproducibly, on the first
   audited event of the session. Root cause not fully chased into the
   server's own sysvar internals (plausible candidates: PLUGIN_VAR_NOSYSVAR
   THDVARs may not get the per-THD strdup() at all in this server series;
   or a ~4 KB default value is treated differently than MariaDB's shorter
   equivalents) — the fix does not depend on knowing which.

   Replacement design: per-connection state lives in a plain
   std::unordered_map<MYSQL_THD, StatementState> guarded by a dedicated
   rwlock (state_map_lock), fully owned and managed by this plugin instead
   of borrowed from a server mechanism whose semantics differ by version.
   std::unordered_map guarantees pointer/reference stability for existing
   elements across inserts/rehashes (only erasing the element itself
   invalidates its pointer — cppreference, unordered_map complexity
   guarantees), so get_state() can hand back a raw StatementState* that
   stays valid for the rest of that event's processing without holding
   the lock throughout. Entries are created lazily on first use and
   erased on MYSQL_AUDIT_CONNECTION_DISCONNECT (see notify_impl below).
   A connection that never raises a clean DISCONNECT event (e.g. killed
   mid-network-failure) leaks one ~4 KB entry — accepted as far better
   than the alternative of crashing the server on every session.
   ------------------------------------------------------------------------ */

#define STATE_TABLES_BUF 3968
#define STATE_CURRENT_DB_BUF 192        /* NAME_LEN in MySQL is 64*4+1;
                                            generous margin, not exact */

struct StatementState
{
  unsigned long long local_seq;         /* connection-local statement
                                            counter — NOT the server's
                                            internal query id (MySQL's
                                            mysql_event_general carries no
                                            query_id field, unlike
                                            MariaDB's; see DECISIONS.md) */
  unsigned long long start_ns;          /* CLOCK_MONOTONIC at dispatch     */
  int have_start;                       /* start_ns valid for this stmt    */
  int in_statement;                     /* between GENERAL_LOG and STATUS  */
  unsigned int cmd_mask;                /* allowed CommandBits from table
                                           matches accumulated so far      */
  int tables_truncated;                 /* tables[] overflowed             */
  unsigned int tables_len;
  char tables[STATE_TABLES_BUF];        /* "db.tbl,db.tbl,..."             */

  /* Best-effort current-schema tracker (see the DDL gap note at the top
     of this file). Persists across statements of the same connection —
     NOT reset by state_begin_statement(). */
  unsigned int current_db_len;
  char current_db[STATE_CURRENT_DB_BUF];

  /* std::unordered_map value-initializes this on first insertion
     ("T()" — zero-initializes every member since StatementState has no
     user-provided constructor), so a freshly created entry always starts
     all-zero without needing a separate "pristine" sentinel/magic check. */
};

static mysql_rwlock_t state_map_lock;
static std::unordered_map<MYSQL_THD, StatementState> *state_map= NULL;

#ifdef HAVE_PSI_INTERFACE
static PSI_rwlock_key key_rwlock_state_map;
static PSI_rwlock_info state_map_rwlock_list[]=
{
  { &key_rwlock_state_map, "selective_trace::state_map_lock", 0, 0,
    PSI_DOCUMENT_ME }
};
#else
#define key_rwlock_state_map 0
#endif

static StatementState *get_state(MYSQL_THD thd)
{
  mysql_rwlock_rdlock(&state_map_lock);
  auto it= state_map->find(thd);
  if (it != state_map->end())
  {
    StatementState *st= &it->second;
    mysql_rwlock_unlock(&state_map_lock);
    return st;
  }
  mysql_rwlock_unlock(&state_map_lock);

  /* First event ever seen for this THD: upgrade to the write lock and
     insert. A given THD's events are only ever produced by the one
     connection thread executing its statements (never concurrently from
     two threads for the same THD), so there is no lost-update race here
     — the write lock only protects the map's internal structure against
     other connections' concurrent inserts/finds. */
  mysql_rwlock_wrlock(&state_map_lock);
  StatementState *st= &(*state_map)[thd];
  mysql_rwlock_unlock(&state_map_lock);
  return st;
}

/* Runs for every CONNECTION_CLASS/DISCONNECT event, regardless of
   selective_trace_enabled — cheap, and avoids leaking an entry for every
   connection that happens to disconnect while tracing is toggled off. */
static void forget_state(MYSQL_THD thd)
{
  mysql_rwlock_wrlock(&state_map_lock);
  state_map->erase(thd);
  mysql_rwlock_unlock(&state_map_lock);
}

static unsigned long long now_ns()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned long long) ts.tv_sec * 1000000000ULL +
         (unsigned long long) ts.tv_nsec;
}

/* ------------------------------------------------------------------------
   System variables
   ------------------------------------------------------------------------ */

static bool opt_enabled= false;
static char *opt_schemas= NULL;
static char *opt_tables= NULL;
static char *opt_connections= NULL;
static ulong opt_output= 0;
static char *opt_file_path= NULL;
static uint opt_min_duration_ms= 0;
static bool opt_mask_passwords= true;

#define SELECTIVE_TRACE_OUTPUT_FILE  0
#define SELECTIVE_TRACE_OUTPUT_TABLE 1

/*
  nullptr, not the legacy NullS macro: NullS (from my_sys.h in MariaDB /
  MySQL 8.0) is not transitively pulled in by this file's includes on
  MySQL 9.x — confirmed by compiling against a real mysql-9.7.2 tree,
  where this line failed with "'NullS' was not declared in this scope".
  nullptr needs no extra include and works identically here.
*/
static const char *output_names[]= { "FILE", "TABLE", nullptr };
static TYPELIB output_typelib=
{
  array_elements(output_names) - 1, "", output_names, NULL
};

static int check_filter_list(MYSQL_THD thd __attribute__((unused)),
                             SYS_VAR *var,
                             void *save, struct st_mysql_value *value,
                             int is_table_list)
try
{
  int len= 0;
  const char *str= value->val_str(value, NULL, &len);
  FilterRules ignored;
  std::string bad_token;

  if (str != NULL)
  {
    bool ok= is_table_list
      ? selective_trace::parse_filter_lists(NULL, str, &ignored, &bad_token)
      : selective_trace::parse_filter_lists(str, NULL, &ignored, &bad_token);
    if (!ok)
    {
      my_printf_error(ER_WRONG_VALUE_FOR_VAR,
                      "selective_trace: invalid entry '%s' in %s",
                      MYF(0), bad_token.c_str(),
                      is_table_list ? "tables (expected schema.table"
                                      " or schema.*)"
                                    : "schemas");
      return 1;
    }
  }
  *(const char **) save= str;
  (void) var;
  return 0;
}
catch (...)
{
  /* C boundary: fail the SET instead of letting the exception escape */
  status_callback_errors++;
  return 1;
}

static int check_schemas(MYSQL_THD thd, SYS_VAR *var,
                                void *save, struct st_mysql_value *value)
{
  return check_filter_list(thd, var, save, value, 0);
}

static int check_tables(MYSQL_THD thd, SYS_VAR *var,
                               void *save, struct st_mysql_value *value)
{
  return check_filter_list(thd, var, save, value, 1);
}

static int check_connections(MYSQL_THD thd __attribute__((unused)),
                                    SYS_VAR *var
                                      __attribute__((unused)),
                                    void *save, struct st_mysql_value *value)
try
{
  int len= 0;
  const char *str= value->val_str(value, NULL, &len);
  FilterRules ignored;
  std::string bad_token;

  if (str != NULL &&
      !selective_trace::parse_connection_list(str, &ignored, &bad_token))
  {
    my_printf_error(ER_WRONG_VALUE_FOR_VAR,
                    "selective_trace: invalid entry '%s' in"
                    " connections (expected a decimal connection id)",
                    MYF(0), bad_token.c_str());
    return 1;
  }
  *(const char **) save= str;
  return 0;
}
catch (...)
{
  status_callback_errors++;
  return 1;
}

static int rebuild_rules_locked(const char *schemas_csv,
                                const char *tables_csv,
                                const char *conns_csv)
{
  FilterRules *fresh= new (std::nothrow) FilterRules();
  if (fresh == NULL)
    return 1;

  std::string bad_token;
  if (!selective_trace::parse_filter_lists(schemas_csv, tables_csv,
                                         fresh, &bad_token) ||
      !selective_trace::parse_connection_list(conns_csv, fresh, &bad_token))
  {
    /* Can't happen after check callbacks, but never swap in bad rules. */
    delete fresh;
    return 1;
  }

  FilterRules *old= active_rules;
  active_rules= fresh;
  delete old;
  return 0;
}

/* which list a sysvar update targets */
enum filter_kind { FK_SCHEMAS= 0, FK_TABLES= 1, FK_CONNECTIONS= 2 };

static void update_filter_list(void *var_ptr, const void *save,
                               filter_kind kind)
{
  const char *new_val= *(const char *const *) save;
  if (new_val == NULL)
    new_val= "";

  mysql_rwlock_wrlock(&filter_lock);

  /* the lock is C state: release it even if an allocation throws */
  try
  {
    std::string *storage= kind == FK_TABLES      ? tables_storage :
                          kind == FK_CONNECTIONS ? connections_storage :
                                                   schemas_storage;
    storage->assign(new_val);
    *(char **) var_ptr= const_cast<char *>(storage->c_str());

    (void) rebuild_rules_locked(schemas_storage->c_str(),
                                tables_storage->c_str(),
                                connections_storage->c_str());
  }
  catch (...)
  {
    status_callback_errors++;   /* old rules stay active */
  }

  mysql_rwlock_unlock(&filter_lock);
}

static void update_schemas(MYSQL_THD thd __attribute__((unused)),
                                  SYS_VAR *var
                                    __attribute__((unused)),
                                  void *var_ptr, const void *save)
{
  update_filter_list(var_ptr, save, FK_SCHEMAS);
}

static void update_tables(MYSQL_THD thd __attribute__((unused)),
                                 SYS_VAR *var
                                   __attribute__((unused)),
                                 void *var_ptr, const void *save)
{
  update_filter_list(var_ptr, save, FK_TABLES);
}

static void update_connections(MYSQL_THD thd __attribute__((unused)),
                                       SYS_VAR *var
                                         __attribute__((unused)),
                                       void *var_ptr, const void *save)
{
  update_filter_list(var_ptr, save, FK_CONNECTIONS);
}

static void update_file_path(MYSQL_THD thd __attribute__((unused)),
                                 SYS_VAR *var
                                   __attribute__((unused)),
                                 void *var_ptr, const void *save)
{
  const char *new_val= *(const char *const *) save;
  if (new_val == NULL)
    new_val= "";

  try
  {
    file_path_storage->assign(new_val);
    *(char **) var_ptr= const_cast<char *>(file_path_storage->c_str());
  }
  catch (...)
  {
    status_callback_errors++;
    return;                     /* keep the previous path */
  }

  /* Reopen only if the writer is in use; otherwise it opens lazily. */
  if (opt_enabled && opt_output == SELECTIVE_TRACE_OUTPUT_FILE)
    selective_trace::file_writer_reopen(file_path_storage->c_str());
  else
    selective_trace::file_writer_close();
}

static MYSQL_SYSVAR_BOOL(enabled, opt_enabled, PLUGIN_VAR_OPCMDARG,
  "Enable/disable selective query logging.",
  NULL, NULL, false);

static MYSQL_SYSVAR_STR(schemas, opt_schemas,
  PLUGIN_VAR_RQCMDARG,
  "Comma separated list of schemas whose queries are logged."
  " Empty means no schema filter.",
  check_schemas, update_schemas, "");

static MYSQL_SYSVAR_STR(tables, opt_tables,
  PLUGIN_VAR_RQCMDARG,
  "Comma separated list of schema.table entries logged regardless of the"
  " schema filter (schema.* matches the whole schema). Only DML"
  " (SELECT/INSERT/UPDATE/DELETE) is table-granular in this MySQL build —"
  " see the DDL gap note in selective_trace_mysql.cc / docs/DECISIONS.md."
  " Empty means no table filter.",
  check_tables, update_tables, "");

static MYSQL_SYSVAR_STR(connections, opt_connections,
  PLUGIN_VAR_RQCMDARG,
  "Comma separated list of connection ids (as in SHOW PROCESSLIST). Every"
  " statement of a listed connection is traced in full, regardless of the"
  " schema/table filters. Empty means no connection filter.",
  check_connections, update_connections, "");

static MYSQL_SYSVAR_ENUM(output, opt_output, PLUGIN_VAR_RQCMDARG,
  "Log destination. FILE writes one JSON object per line to"
  " selective_trace_log_file_path; TABLE inserts into the plugin log"
  " table.",
  NULL, NULL, SELECTIVE_TRACE_OUTPUT_FILE, &output_typelib);

static MYSQL_SYSVAR_STR(log_file_path, opt_file_path,
  PLUGIN_VAR_RQCMDARG,
  "Path of the log file used when selective_trace_output=FILE."
  " Relative paths are resolved from the current working directory of"
  " the server process.",
  NULL, update_file_path, "selective_trace.json");

static MYSQL_SYSVAR_UINT(min_duration_ms, opt_min_duration_ms,
  PLUGIN_VAR_RQCMDARG,
  "Only log queries slower than this many milliseconds. 0 logs all queries.",
  NULL, NULL, 0, 0, 0x7FFFFFFF, 1);

static MYSQL_SYSVAR_BOOL(mask_passwords, opt_mask_passwords,
  PLUGIN_VAR_OPCMDARG,
  "Replace credential literals (IDENTIFIED BY, PASSWORD(), SET PASSWORD)"
  " with *** before logging. On by default.",
  NULL, NULL, true);

static SYS_VAR *selective_trace_sysvars[]=
{
  MYSQL_SYSVAR(enabled),
  MYSQL_SYSVAR(schemas),
  MYSQL_SYSVAR(tables),
  MYSQL_SYSVAR(connections),
  MYSQL_SYSVAR(output),
  MYSQL_SYSVAR(log_file_path),
  MYSQL_SYSVAR(min_duration_ms),
  MYSQL_SYSVAR(mask_passwords),
  NULL
};

/* Status variables (SHOW STATUS LIKE 'selective_trace%') */
static ulong status_events_logged= 0;
static ulong status_write_failures= 0;
static ulong status_events_dropped= 0;
static ulong status_writer_reconnects= 0;

/*
  Confirmed against the real MySQL 8.0.40 headers (include/mysql/status_var.h):
  the plugin-facing type is SHOW_VAR (not st_mysql_show_var, which doesn't
  exist in MySQL — that name is MariaDB's), it has 4 fields
  (name, value, type, scope), there is no SHOW_ULONG (SHOW_LONG is already
  "shown as unsigned long"), and the SHOW_FUNC callback signature is
  int(MYSQL_THD, SHOW_VAR *, char *) — 3 parameters, not 5 like MariaDB's.
*/
static int show_write_failures(MYSQL_THD thd __attribute__((unused)),
                               SHOW_VAR *var,
                               char *buff __attribute__((unused)))
{
  status_write_failures= selective_trace::file_writer_failures() +
                         selective_trace::table_writer_failures();
  var->type= SHOW_LONG;
  var->value= (char *) &status_write_failures;
  return 0;
}

static int show_events_dropped(MYSQL_THD thd __attribute__((unused)),
                               SHOW_VAR *var,
                               char *buff __attribute__((unused)))
{
  status_events_dropped= selective_trace::table_writer_dropped();
  var->type= SHOW_LONG;
  var->value= (char *) &status_events_dropped;
  return 0;
}

/*
  Periodic recycles of the TABLE writer's internal connection. Expected to
  climb steadily (~events/20000) under sustained TABLE-mode tracing — it
  is the mechanism that keeps server RSS bounded, not an error counter.
  See src/writer_recycle_policy.h.
*/
static int show_writer_reconnects(MYSQL_THD thd __attribute__((unused)),
                                  SHOW_VAR *var,
                                  char *buff __attribute__((unused)))
{
  status_writer_reconnects= selective_trace::table_writer_reconnects();
  var->type= SHOW_LONG;
  var->value= (char *) &status_writer_reconnects;
  return 0;
}

static SHOW_VAR selective_trace_status[]=
{
  { "selective_trace_events_logged", (char *) &status_events_logged,
    SHOW_LONG, SHOW_SCOPE_GLOBAL },
  { "selective_trace_writer_reconnects", (char *) show_writer_reconnects,
    SHOW_FUNC, SHOW_SCOPE_GLOBAL },
  { "selective_trace_write_failures", (char *) show_write_failures,
    SHOW_FUNC, SHOW_SCOPE_GLOBAL },
  { "selective_trace_events_dropped", (char *) show_events_dropped,
    SHOW_FUNC, SHOW_SCOPE_GLOBAL },
  { "selective_trace_callback_errors", (char *) &status_callback_errors,
    SHOW_LONG, SHOW_SCOPE_GLOBAL },
  { 0, 0, SHOW_UNDEF, SHOW_SCOPE_UNDEF }
};

/* ------------------------------------------------------------------------
   Event capture
   ------------------------------------------------------------------------ */

/*
  The GENERAL_STATUS event fires for every command; we only want the ones
  that carry actual SQL text (a directly issued statement or a prepared-
  statement execution).
*/
static bool general_command_is(const struct mysql_event_general *event,
                               const char *label, size_t label_len)
{
  return event->general_command.length == label_len &&
         memcmp(event->general_command.str, label, label_len) == 0;
}

static bool is_query_command(const struct mysql_event_general *event)
{
  return general_command_is(event, "Query", 5) ||
         general_command_is(event, "Execute", 7);
}

static void set_current_db(StatementState *st, const char *db, size_t len)
{
  if (len >= sizeof(st->current_db))
    len= sizeof(st->current_db) - 1;
  memcpy(st->current_db, db, len);
  st->current_db[len]= 0;
  st->current_db_len= (unsigned int) len;
}

/*
  Best-effort current-schema tracker — see the DDL gap note at the top of
  this file. Two sources, both derived only from confirmed
  mysql_event_general fields (no server-internal API assumed):

    - COM_INIT_DB ("Init DB"): general_query carries the raw schema name
      verbatim (the wire protocol payload of USE-at-the-protocol-level,
      e.g. `mysql -D db`, or a client-side USE that the driver sends as
      COM_INIT_DB instead of a COM_QUERY "USE ..." statement).
    - A "USE <schema>" statement sent as an ordinary COM_QUERY: detected
      via extract_command() (reused from the filter engine), then the
      schema token is parsed out locally.

  This does not see the connection's initial default schema (given on
  the connection string, before any USE) unless the client sends it via
  COM_INIT_DB at connect time — most clients do, but this is not
  guaranteed. Documented as a known limitation in docs/DECISIONS.md.
*/
static void track_current_db(StatementState *st,
                             const struct mysql_event_general *event)
{
  if (general_command_is(event, "Init DB", 7))
  {
    if (event->general_query.length > 0)
      set_current_db(st, event->general_query.str,
                     event->general_query.length);
    return;
  }

  if (!general_command_is(event, "Query", 5) ||
      event->general_query.length == 0)
    return;

  char cmdbuf[8];
  selective_trace::extract_command(event->general_query.str,
                                   event->general_query.length,
                                   cmdbuf, sizeof(cmdbuf));
  if (strcmp(cmdbuf, "USE") != 0)
    return;

  /* Skip past "USE" (case-insensitive) and following whitespace; this is
     a light-weight re-scan, not a full re-run of extract_command's
     comment-skipping — a USE statement preceded by a block comment is
     not recognized as USE by this second pass, and current_db is left
     unchanged (safe default: stale but not wrong). */
  const char *p= event->general_query.str;
  const char *end= p + event->general_query.length;
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' ||
                     *p == '('))
    p++;
  if (end - p < 3)
    return;
  p+= 3;                                 /* "USE" */
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
    p++;
  if (p >= end)
    return;

  bool backtick= (*p == '`');
  if (backtick)
    p++;
  const char *name_begin= p;
  while (p < end)
  {
    char c= *p;
    if (backtick)
    {
      if (c == '`')
        break;
    }
    else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '$'))
      break;
    p++;
  }
  if (p > name_begin)
    set_current_db(st, name_begin, (size_t) (p - name_begin));
}

/*
  Reset per-statement fields. Runs at every GENERAL_LOG (dispatch start),
  so a statement accumulates ALL its table events until GENERAL_STATUS.
  current_db is intentionally NOT touched here — it survives across
  statements of the same connection.
*/
static void state_begin_statement(StatementState *st, int with_start)
{
  st->local_seq++;
  st->cmd_mask= 0;
  st->tables_len= 0;
  st->tables_truncated= 0;
  st->in_statement= 1;
  st->have_start= with_start;
  st->start_ns= with_start ? now_ns() : 0;
}

/*
  Persistent stats tables (InnoDB's engine-independent statistics) get
  locked as a side effect of ordinary DML and are not part of the user's
  query. Recorded/matched only when explicitly listed in
  selective_trace_tables.
*/
static bool is_internal_stats_table(const char *db, size_t db_len,
                                    const char *tbl, size_t tbl_len)
{
  static const struct { const char *name; size_t len; } stats_tables[]=
  {
    { "innodb_table_stats", 18 }, { "innodb_index_stats", 18 }
  };

  if (db_len != 5 || strncasecmp(db, "mysql", 5) != 0)
    return false;
  for (size_t i= 0; i < array_elements(stats_tables); i++)
    if (tbl_len == stats_tables[i].len &&
        strncasecmp(tbl, stats_tables[i].name, tbl_len) == 0)
      return true;
  return false;
}

static void state_add_table(StatementState *st,
                            const char *db, size_t db_len,
                            const char *tbl, size_t tbl_len)
{
  size_t need= db_len + 1 + tbl_len;
  if (need == 0 || need >= sizeof(st->tables))
  {
    st->tables_truncated= 1;
    return;
  }

  /* dedupe: same table touched more than once in a statement */
  if (st->tables_len > 0)
  {
    const char *hay= st->tables;
    size_t hay_len= st->tables_len;
    for (size_t pos= 0; pos + need <= hay_len; )
    {
      const char *entry_end= (const char *) memchr(hay + pos, ',',
                                                   hay_len - pos);
      size_t entry_len= entry_end ? (size_t)(entry_end - (hay + pos))
                                  : hay_len - pos;
      if (entry_len == need &&
          memcmp(hay + pos, db, db_len) == 0 &&
          hay[pos + db_len] == '.' &&
          memcmp(hay + pos + db_len + 1, tbl, tbl_len) == 0)
        return;
      if (!entry_end)
        break;
      pos+= entry_len + 1;
    }
  }

  if (st->tables_len + (st->tables_len ? 1 : 0) + need >= sizeof(st->tables))
  {
    st->tables_truncated= 1;                  /* full: drop extra tables */
    return;
  }

  if (st->tables_len)
    st->tables[st->tables_len++]= ',';
  memcpy(st->tables + st->tables_len, db, db_len);
  st->tables_len+= (unsigned int) db_len;
  st->tables[st->tables_len++]= '.';
  memcpy(st->tables + st->tables_len, tbl, tbl_len);
  st->tables_len+= (unsigned int) tbl_len;
}

static void handle_table_access_event(
    MYSQL_THD thd, const struct mysql_event_table_access *event)
{
  StatementState *st= get_state(thd);

  /* Plugin enabled mid-statement (no GENERAL_LOG seen): start ad-hoc,
     without a start clock. */
  if (!st->in_statement)
    state_begin_statement(st, 0);

  const char *db= event->table_database.str;
  size_t db_len= event->table_database.length;
  const char *tbl= event->table_name.str;
  size_t tbl_len= event->table_name.length;

  unsigned explicit_cmds= 0;
  unsigned schema_cmds= 0;
  mysql_rwlock_rdlock(&filter_lock);
  const FilterRules *rules= active_rules;
  if (rules)
  {
    explicit_cmds= selective_trace::match_table(*rules, db, db_len,
                                              tbl, tbl_len);
    schema_cmds= selective_trace::match_schema(*rules, db, db_len);
  }
  mysql_rwlock_unlock(&filter_lock);

  /* bookkeeping side-effect tables only count when explicitly filtered */
  if (explicit_cmds == 0 && is_internal_stats_table(db, db_len,
                                                    tbl, tbl_len))
    return;

  state_add_table(st, db, db_len, tbl, tbl_len);
  st->cmd_mask|= explicit_cmds | schema_cmds;
}

static void handle_status_event(MYSQL_THD thd,
                                const struct mysql_event_general *event)
{
  StatementState *st= get_state(thd);

  /* Runs for every GENERAL_STATUS, query or not — see track_current_db. */
  track_current_db(st, event);

  if (!is_query_command(event))
    return;

  /* Which command is this statement? Needed for the per-entry command
     qualifiers, and reused verbatim in the output. */
  char cmdbuf[24];
  selective_trace::extract_command(event->general_query.str,
                                 event->general_query.length,
                                 cmdbuf, sizeof(cmdbuf));
  const unsigned cmd_bit= selective_trace::command_bit(cmdbuf);

  unsigned allowed= st->in_statement ? st->cmd_mask : 0;

  if (!(allowed & cmd_bit))
  {
    /* fall back to the session-schema and connection filters */
    mysql_rwlock_rdlock(&filter_lock);
    const FilterRules *rules= active_rules;
    if (rules)
    {
      /* a listed connection is traced in full (all commands) */
      if (selective_trace::match_connection(*rules,
                                            event->general_thread_id))
        allowed= selective_trace::CMD_ALL;
      else if (st->current_db_len > 0)
        allowed|= selective_trace::match_schema(*rules, st->current_db,
                                              st->current_db_len);
    }
    mysql_rwlock_unlock(&filter_lock);
  }

  if (!(allowed & cmd_bit))
    goto reset;

  {
    double duration_ms= -1;
    if (st->in_statement && st->have_start)
      duration_ms= (double) (now_ns() - st->start_ns) / 1e6;

    if (opt_min_duration_ms > 0 &&
        (duration_ms < 0 || duration_ms < (double) opt_min_duration_ms))
      goto reset;

    /* timestamp with milliseconds (wall clock) */
    struct timeval tv;
    struct tm tm_time;
    gettimeofday(&tv, NULL);
    time_t secs= (time_t) tv.tv_sec;
    localtime_r(&secs, &tm_time);

    char ts[40];   /* GCC's -Wformat-truncation sizes %d against INT_MIN;
                      the real tm_* range fits in 24 bytes, but pad the
                      buffer instead of fighting the (harmless) warning */
    snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             tm_time.tm_year + 1900, tm_time.tm_mon + 1, tm_time.tm_mday,
             tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec,
             (int) (tv.tv_usec / 1000));

    const int have_tables= (st->in_statement && st->tables_len > 0);
    char numbuf[64];
    std::string user_host;
    user_host.reserve(event->general_user.length + 1 +
                      event->general_host.length);
    user_host.append(event->general_user.str, event->general_user.length);
    user_host.push_back('@');
    user_host.append(event->general_host.str, event->general_host.length);

    /* Credential masking: log the sanitized query text in both modes. */
    const char *qtext= event->general_query.str;
    size_t qlen= event->general_query.length;
    std::string masked;
    if (opt_mask_passwords &&
        selective_trace::mask_secrets(qtext, qlen, &masked))
    {
      qtext= masked.data();
      qlen= masked.size();
    }

    /* Best available "current schema" for the output db field: prefer
       the tracked current_db (works for any statement); TABLE_ACCESS
       events don't carry a single "the" schema when several tables from
       different schemas are touched, so current_db is the right choice
       here, not st->tables. */
    const char *db_out= st->current_db_len ? st->current_db : "";
    size_t db_out_len= st->current_db_len;

    if (opt_output == SELECTIVE_TRACE_OUTPUT_FILE)
    {
      std::string line;
      line.reserve(320 + event->general_query.length + st->tables_len + 32);

      line.append("{\"ts\":\"").append(ts);
      snprintf(numbuf, sizeof(numbuf),
               "\",\"conn_id\":%lu,\"query_seq\":%llu",
               event->general_thread_id, st->local_seq);
      line.append(numbuf);

      line.append(",\"user\":\"");
      selective_trace::json_escape_append(&line, user_host.data(),
                                        user_host.size());

      line.append("\",\"db\":\"");
      selective_trace::json_escape_append(&line, db_out, db_out_len);

      line.append("\",\"tables\":[");
      if (have_tables)
      {
        const char *p= st->tables;
        const char *tend= st->tables + st->tables_len;
        int first= 1;
        while (p < tend)
        {
          const char *comma= (const char *) memchr(p, ',',
                                                   (size_t)(tend - p));
          size_t elen= comma ? (size_t)(comma - p) : (size_t)(tend - p);
          if (!first)
            line.push_back(',');
          first= 0;
          line.push_back('"');
          selective_trace::json_escape_append(&line, p, elen);
          line.push_back('"');
          p+= elen + 1;
        }
      }

      line.push_back(']');
      if (have_tables && st->tables_truncated)
        line.append(",\"tables_truncated\":true");
      line.append(",\"command\":\"");
      line.append(cmdbuf);

      if (duration_ms >= 0)
        snprintf(numbuf, sizeof(numbuf), "\",\"duration_ms\":%.3f",
                 duration_ms);
      else
        snprintf(numbuf, sizeof(numbuf), "\",\"duration_ms\":null");
      line.append(numbuf);

      snprintf(numbuf, sizeof(numbuf), ",\"error_code\":%d,\"query\":\"",
               event->general_error_code);
      line.append(numbuf);
      selective_trace::json_escape_append(&line, qtext, qlen);
      line.append("\"}\n");

      if (selective_trace::file_writer_write(line.data(), line.size(),
                                           opt_file_path))
        status_events_logged++;
    }
    else                                /* SELECTIVE_TRACE_OUTPUT_TABLE */
    {
      std::string sql;
      sql.reserve(400 + event->general_query.length + st->tables_len + 32);

      sql.append("INSERT INTO mysql.selective_trace_events"
                 " (`ts`,`conn_id`,`query_id`,`user`,`db`,`tables_involved`,"
                 "`command`,`duration_ms`,`error_code`,`query`) VALUES ('");
      sql.append(ts);
      snprintf(numbuf, sizeof(numbuf), "',%lu,%llu,'",
               event->general_thread_id, st->local_seq);
      sql.append(numbuf);

      selective_trace::sql_escape_append(&sql, user_host.data(),
                                       user_host.size());

      sql.append("','");
      selective_trace::sql_escape_append(&sql, db_out, db_out_len);
      sql.append("','");
      if (have_tables)
      {
        selective_trace::sql_escape_append(&sql, st->tables, st->tables_len);
        if (st->tables_truncated)
          sql.append(",...");
      }
      sql.append("','");
      sql.append(cmdbuf);
      sql.append("',");

      if (duration_ms >= 0)
        snprintf(numbuf, sizeof(numbuf), "%.3f", duration_ms);
      else
        snprintf(numbuf, sizeof(numbuf), "NULL");
      sql.append(numbuf);

      snprintf(numbuf, sizeof(numbuf), ",%d,'", event->general_error_code);
      sql.append(numbuf);
      selective_trace::sql_escape_append(&sql, qtext, qlen);
      sql.append("')");

      if (selective_trace::table_writer_enqueue(&sql))
        status_events_logged++;
    }
  }

reset:
  /* STATUS closes the statement: never log it twice. current_db is left
     untouched — it belongs to the connection, not the statement. */
  st->cmd_mask= 0;
  st->tables_len= 0;
  st->tables_truncated= 0;
  st->have_start= 0;
  st->in_statement= 0;
}

static void notify_impl(MYSQL_THD thd, unsigned int event_class,
                        const void *event)
{
  if (event_class == MYSQL_AUDIT_GENERAL_CLASS)
  {
    const struct mysql_event_general *ev=
      (const struct mysql_event_general *) event;
    if (ev->event_subclass == MYSQL_AUDIT_GENERAL_LOG)
    {
      /* statement dispatch starts: stamp the state */
      StatementState *st= get_state(thd);
      state_begin_statement(st, 1);
    }
    else if (ev->event_subclass == MYSQL_AUDIT_GENERAL_STATUS)
      handle_status_event(thd, ev);
  }
  else if (event_class == MYSQL_AUDIT_TABLE_ACCESS_CLASS)
    handle_table_access_event(thd,
        (const struct mysql_event_table_access *) event);
}

/*
  Confirmed against the real (non-preprocessed) plugin_audit.h in this
  build: st_mysql_audit::event_notify is
  int (*)(MYSQL_THD, mysql_event_class_t, const void *) — MYSQL_THD, not
  void*. (An earlier check against plugin_audit.h.pp, a pre-generated
  ABI-snapshot file also present in the tree, suggested void* — that file
  does not reflect what this header actually declares; the compiler is
  the ground truth here, not the .pp file.)
*/
static int selective_trace_notify(MYSQL_THD thd,
                                  mysql_event_class_t event_class,
                                  const void *event)
{
  if (!plugin_ready || thd == NULL)
    return 0;

  /* Never log the internal writer's own INSERTs (self-log loop). */
  if (selective_trace::table_writer_is_self())
    return 0;

  /* C boundary: no C++ exception (e.g. bad_alloc while assembling the
     output under memory pressure) may reach the server — that would
     abort mysqld. Drop the event and count it instead. */
  try
  {
    /* Connection cleanup runs regardless of selective_trace_enabled, so
       toggling tracing off never leaks a state_map entry for a
       connection that disconnects while it's off. */
    if (event_class == MYSQL_AUDIT_CONNECTION_CLASS)
    {
      const struct mysql_event_connection *ev=
        (const struct mysql_event_connection *) event;
      if (ev->event_subclass == MYSQL_AUDIT_CONNECTION_DISCONNECT)
        forget_state(thd);
      return 0;
    }

    if (!opt_enabled)
      return 0;

    notify_impl(thd, (unsigned int) event_class, event);
  }
  catch (...)
  {
    status_callback_errors++;
  }
  return 0;
}

/* ------------------------------------------------------------------------
   init / deinit
   ------------------------------------------------------------------------ */

static void register_filter_psi()
{
#ifdef HAVE_PSI_INTERFACE
  mysql_rwlock_register("selective_trace", rwlock_key_list, 1);
  mysql_rwlock_register("selective_trace", state_map_rwlock_list, 1);
#endif
}

static int selective_trace_init(MYSQL_PLUGIN arg __attribute__((unused)))
{
  register_filter_psi();
  mysql_rwlock_init(key_rwlock_filter, &filter_lock);
  mysql_rwlock_init(key_rwlock_state_map, &state_map_lock);
  state_map= new (std::nothrow) std::unordered_map<MYSQL_THD, StatementState>();
  if (state_map == NULL)
  {
    mysql_rwlock_destroy(&state_map_lock);
    mysql_rwlock_destroy(&filter_lock);
    return 1;
  }
  selective_trace::file_writer_init();
  selective_trace::table_writer_init();

  schemas_storage= new (std::nothrow) std::string(
      opt_schemas ? opt_schemas : "");
  tables_storage= new (std::nothrow) std::string(
      opt_tables ? opt_tables : "");
  connections_storage= new (std::nothrow) std::string(
      opt_connections ? opt_connections : "");
  file_path_storage= new (std::nothrow) std::string(
      opt_file_path ? opt_file_path : "");
  if (schemas_storage == NULL || tables_storage == NULL ||
      connections_storage == NULL || file_path_storage == NULL)
    goto fail;

  /* Point the sysvars at our storage from the start, so the update
     callbacks and SHOW VARIABLES always deal with the same memory. */
  opt_schemas= const_cast<char *>(schemas_storage->c_str());
  opt_tables= const_cast<char *>(tables_storage->c_str());
  opt_connections= const_cast<char *>(connections_storage->c_str());
  opt_file_path= const_cast<char *>(file_path_storage->c_str());

  if (rebuild_rules_locked(schemas_storage->c_str(),
                           tables_storage->c_str(),
                           connections_storage->c_str()))
    goto fail;

  plugin_ready= 1;
  fprintf(stderr, "selective_trace: plugin %s started\n", PLUGIN_STR_VERSION);
  return 0;

fail:
  delete schemas_storage;
  delete tables_storage;
  delete connections_storage;
  delete file_path_storage;
  schemas_storage= tables_storage= connections_storage= file_path_storage= NULL;
  selective_trace::table_writer_shutdown();
  selective_trace::file_writer_deinit();
  delete state_map;
  state_map= NULL;
  mysql_rwlock_destroy(&state_map_lock);
  mysql_rwlock_destroy(&filter_lock);
  return 1;
}

static int selective_trace_deinit(MYSQL_PLUGIN arg __attribute__((unused)))
{
  if (!plugin_ready)
    return 0;
  plugin_ready= 0;

  selective_trace::table_writer_shutdown();
  selective_trace::file_writer_deinit();

  delete active_rules;
  active_rules= NULL;
  delete schemas_storage;
  delete tables_storage;
  delete connections_storage;
  delete file_path_storage;
  schemas_storage= tables_storage= connections_storage= file_path_storage= NULL;
  delete state_map;
  state_map= NULL;

  mysql_rwlock_destroy(&state_map_lock);
  mysql_rwlock_destroy(&filter_lock);
  fprintf(stderr, "selective_trace: plugin stopped\n");
  return 0;
}

/* ------------------------------------------------------------------------
   Plugin declaration

   class_mask is positional, one slot per audit class in the enum order
   confirmed in CLAUDE.md section 2.1 (GENERAL=0, CONNECTION=1, PARSE=2,
   AUTHORIZATION=3, TABLE_ACCESS=4, GLOBAL_VARIABLE=5, SERVER_STARTUP=6,
   SERVER_SHUTDOWN=7, COMMAND=8, QUERY=9, STORED_PROGRAM=10,
   AUTHENTICATION=11, MESSAGE=12) — 13 slots. The st_mysql_plugin struct
   layout (14 fields incl. check_uninstall between init and deinit, and a
   trailing flags field) and this whole descriptor were confirmed working
   by loading the compiled .so into a real mysqld 8.0.40 via
   INSTALL PLUGIN / SHOW PLUGINS (Etapa 5, Docker) — it comes up ACTIVE.
   ------------------------------------------------------------------------ */

static struct st_mysql_audit selective_trace_descriptor=
{
  MYSQL_AUDIT_INTERFACE_VERSION,
  NULL,                                  /* release_thd */
  selective_trace_notify,
  {
    (unsigned long) (MYSQL_AUDIT_GENERAL_LOG | MYSQL_AUDIT_GENERAL_STATUS),
                                                    /* GENERAL_CLASS      */
    (unsigned long) MYSQL_AUDIT_CONNECTION_DISCONNECT,
                                                    /* CONNECTION_CLASS —
                                                       state_map cleanup */
    0,                                              /* PARSE_CLASS        */
    0,                                              /* AUTHORIZATION_CLASS*/
    (unsigned long) (MYSQL_AUDIT_TABLE_ACCESS_READ |
                     MYSQL_AUDIT_TABLE_ACCESS_INSERT |
                     MYSQL_AUDIT_TABLE_ACCESS_UPDATE |
                     MYSQL_AUDIT_TABLE_ACCESS_DELETE),
                                                    /* TABLE_ACCESS_CLASS */
    0, 0, 0, 0, 0, 0, 0, 0                          /* remaining 8 classes*/
  }
};

mysql_declare_plugin(selective_trace)
{
  MYSQL_AUDIT_PLUGIN,
  &selective_trace_descriptor,
  "selective_trace",
  "selective_trace plugin authors",
  "Selective query tracing by schema/table/command",
  PLUGIN_LICENSE_GPL,
  selective_trace_init,
  NULL,                                  /* check_uninstall — see note above */
  selective_trace_deinit,
  PLUGIN_VERSION,
  selective_trace_status,
  selective_trace_sysvars,
  NULL,                                  /* __reserved1 */
  0                                      /* flags */
}
mysql_declare_plugin_end;
