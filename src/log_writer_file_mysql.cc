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

#include <mysql/psi/mysql_thread.h>

#include <cstdio>
#include <cerrno>
#include <cstring>

#include "log_writer_file_mysql.h"

namespace selective_trace {

static mysql_rwlock_t log_lock;
static FILE *log_fp= NULL;
static unsigned long write_failures= 0;
static int open_failed_logged= 0;   /* avoid flooding the error log */

#ifdef HAVE_PSI_INTERFACE
static PSI_rwlock_key key_rwlock_logfile;
static PSI_rwlock_info log_rwlock_list[]=
{
  { &key_rwlock_logfile, "selective_trace::log_file_lock", 0, 0, PSI_DOCUMENT_ME }
};
#else
#define key_rwlock_logfile 0
#endif

static void register_psi_rwlock()
{
#ifdef HAVE_PSI_INTERFACE
  mysql_rwlock_register("selective_trace", log_rwlock_list, 1);
#endif
}

void file_writer_init()
{
  register_psi_rwlock();
  mysql_rwlock_init(key_rwlock_logfile, &log_lock);
}

void file_writer_deinit()
{
  file_writer_close();
  mysql_rwlock_destroy(&log_lock);
}

/* caller holds the write lock */
static bool open_locked(const char *path)
{
  if (path == NULL || path[0] == '\0')
    return false;
  /* line-buffered-ish append; each write() below is one JSON line, and we
     fflush() it explicitly so tail -f users see events promptly. */
  log_fp= fopen(path, "a");
  if (log_fp == NULL)
  {
    if (!open_failed_logged)
    {
      open_failed_logged= 1;
      fprintf(stderr, "selective_trace: could not open log file '%s': %s\n",
              path, strerror(errno));
    }
    write_failures++;
    return false;
  }
  open_failed_logged= 0;
  return true;
}

bool file_writer_reopen(const char *path)
{
  bool ok;
  mysql_rwlock_wrlock(&log_lock);
  if (log_fp != NULL)
  {
    fclose(log_fp);
    log_fp= NULL;
  }
  ok= open_locked(path);
  mysql_rwlock_unlock(&log_lock);
  return ok;
}

void file_writer_close()
{
  mysql_rwlock_wrlock(&log_lock);
  if (log_fp != NULL)
  {
    fclose(log_fp);
    log_fp= NULL;
  }
  mysql_rwlock_unlock(&log_lock);
}

bool file_writer_write(const char *line, size_t len, const char *path)
{
  bool ok= false;

  mysql_rwlock_rdlock(&log_lock);
  if (log_fp == NULL)
  {
    /* lazy open: upgrade to the write lock */
    mysql_rwlock_unlock(&log_lock);
    mysql_rwlock_wrlock(&log_lock);
    if (log_fp == NULL && !open_locked(path))
    {
      mysql_rwlock_unlock(&log_lock);
      return false;
    }
  }

  if (fwrite(line, 1, len, log_fp) == len)
  {
    fflush(log_fp);
    ok= true;
  }
  else
    write_failures++;
  mysql_rwlock_unlock(&log_lock);
  return ok;
}

unsigned long file_writer_failures()
{
  return write_failures;
}

} /* namespace selective_trace */
