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
  log_writer_table_mysql — TABLE output mode for the MySQL 8.0+ build of
  selective_trace.

  Same architecture as the MariaDB plugin: events are enqueued as
  ready-to-run INSERT statements and executed by a dedicated background
  thread, so the audit callback never blocks a user statement on I/O.

  What differs is *how* the writer thread runs SQL. MariaDB exposes
  mysql_real_connect_local() (an internal, grants-free connection). MySQL
  8.0 has no equivalent entry point for plugins; the supported mechanism
  since 8.0.24 is the **mysql_command_services** component service,
  acquired through the plugin registry (mysql_plugin_registry_acquire()).
  See docs/RESEARCH_NOTES_MYSQL.md and docs/DECISIONS.md — this is the
  single highest-risk piece of the port (CLAUDE.md section 10.1) and the
  exact service/function names below MUST be re-checked against the real
  include/mysql/components/services/mysql_command_services.h of the
  target MySQL 8.0.x before Etapa 4 is considered done.
*/

#ifndef SELECTIVE_TRACE_LOG_WRITER_TABLE_MYSQL_H
#define SELECTIVE_TRACE_LOG_WRITER_TABLE_MYSQL_H

#include <string>

namespace selective_trace {

/* Called once from plugin init/deinit. shutdown() joins the thread. */
void table_writer_init();
void table_writer_shutdown();

/*
  Queue one INSERT statement; starts the writer thread on first use.
  Returns false if the queue is full (event dropped) or the thread could
  not be started.
*/
bool table_writer_enqueue(std::string *sql);

/* True when the calling thread is the writer thread (reentrancy guard). */
bool table_writer_is_self();

unsigned long table_writer_failures();
unsigned long table_writer_dropped();

/*
  Periodic internal-connection recycles since the plugin was loaded.
  The writer deliberately tears down and reopens its connection every N
  inserts to bound server-side memory growth — see
  writer_recycle_policy.h. A steadily climbing value here is normal
  under sustained TABLE-mode tracing (roughly events/20000), not a sign
  of connection trouble.
*/
unsigned long table_writer_reconnects();

} /* namespace selective_trace */

#endif /* SELECTIVE_TRACE_LOG_WRITER_TABLE_MYSQL_H */
