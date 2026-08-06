# Copyright (C) 2026 selective_trace plugin authors
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

# This file is symlinked into <mysql-src>/plugin/selective_trace by
# scripts/build.sh, so paths are relative to this directory (src/), with
# ../core/ reaching the shared filter_engine sources.

INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR}/../core)

SET(SELECTIVE_TRACE_SOURCES
    selective_trace_mysql.cc
    log_writer_file_mysql.cc
    log_writer_file_mysql.h
    log_writer_table_mysql.cc
    log_writer_table_mysql.h
    ../core/filter_engine.cc
    ../core/filter_engine.h)

# MYSQL_ADD_PLUGIN is the MySQL-source-tree equivalent of MariaDB's macro
# of the same name; unlike the MariaDB build, MODULE_ONLY needs no
# RECOMPILE_FOR_EMBEDDED companion flag — MySQL 8.0 dropped the embedded
# server, so there is no embedded variant to special-case.
MYSQL_ADD_PLUGIN(selective_trace ${SELECTIVE_TRACE_SOURCES} MODULE_ONLY)
