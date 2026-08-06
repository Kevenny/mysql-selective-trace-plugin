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
  log_writer_file_mysql — FILE output mode for the MySQL 8.0+ build of
  selective_trace.

  Unlike MariaDB, the MySQL server exposes no logger service to plugins
  (see docs/RESEARCH_NOTES_MYSQL.md) — this is a small writer of our own
  built on stdio (fopen/fwrite), guarded by an mysql_rwlock_t exactly like
  the MariaDB writer: the rwlock only protects the FILE* handle lifecycle
  (open/reopen on path change), never the byte-level write, so concurrent
  statement threads never serialize on each other for the common case
  (handle already open, read lock only).
*/

#ifndef SELECTIVE_TRACE_LOG_WRITER_FILE_MYSQL_H
#define SELECTIVE_TRACE_LOG_WRITER_FILE_MYSQL_H

#include <cstddef>

namespace selective_trace {

/* Called once from plugin init/deinit. */
void file_writer_init();
void file_writer_deinit();

/*
  (Re)open the log file at path. Safe to call while other threads write.
  Returns true on success; on failure the writer stays closed and
  file_writer_write() becomes a no-op returning false.
*/
bool file_writer_reopen(const char *path);

void file_writer_close();

/*
  Append one line (caller includes the trailing '\n'). Lazily opens the
  file on first use with the path given. Returns true if the line hit the
  file.
*/
bool file_writer_write(const char *line, size_t len, const char *path);

/* Total failed writes/opens since load (for diagnostics/status). */
unsigned long file_writer_failures();

} /* namespace selective_trace */

#endif /* SELECTIVE_TRACE_LOG_WRITER_FILE_MYSQL_H */
