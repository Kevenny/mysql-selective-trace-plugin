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
  filter_engine — pure filtering logic shared by the selective_trace
  plugins (MariaDB and MySQL).

  This translation unit must stay free of server headers so it can be
  unit-tested standalone (test_filter_logic.cc builds it with plain g++
  and its own main()) and reused verbatim by both plugins.

  Matching is ASCII case-insensitive: schema/table identifiers are folded
  to lowercase at parse time and compared with a lowercase fold at match
  time, so filters behave the same regardless of lower_case_table_names.

  Source of truth: this file lives in the selective-trace-core project.
  It is vendored here (not yet wired as a git submodule — see
  docs/DECISIONS.md) and must stay byte-identical to the MariaDB plugin's
  copy until the submodule split happens.
*/

#ifndef SELECTIVE_TRACE_FILTER_ENGINE_H
#define SELECTIVE_TRACE_FILTER_ENGINE_H

#include <string>
#include <vector>
#include <cstddef>

namespace selective_trace {

/*
  Command-class bits for the per-entry command qualifiers
  ("entry:cmd1|cmd2"). An entry without qualifier gets CMD_ALL.
*/
enum CommandBits : unsigned
{
  CMD_SELECT    = 1u << 0,
  CMD_INSERT    = 1u << 1,
  CMD_UPDATE    = 1u << 2,
  CMD_DELETE    = 1u << 3,
  CMD_REPLACE   = 1u << 4,
  CMD_LOAD      = 1u << 5,
  CMD_CALL      = 1u << 6,
  CMD_CREATE    = 1u << 7,
  CMD_ALTER     = 1u << 8,
  CMD_DROP      = 1u << 9,
  CMD_TRUNCATE  = 1u << 10,
  CMD_RENAME    = 1u << 11,
  CMD_OTHER     = 1u << 12,
  /* transaction control (only explicit statements are visible to the
     plugin — autocommit does not emit a COMMIT command; see USAGE) */
  CMD_COMMIT    = 1u << 13,
  CMD_ROLLBACK  = 1u << 14,
  CMD_BEGIN     = 1u << 15,
  CMD_SAVEPOINT = 1u << 16,
  CMD_DML       = CMD_INSERT | CMD_UPDATE | CMD_DELETE | CMD_REPLACE |
                  CMD_LOAD,
  CMD_DDL       = CMD_CREATE | CMD_ALTER | CMD_DROP | CMD_TRUNCATE |
                  CMD_RENAME,
  CMD_TCL       = CMD_COMMIT | CMD_ROLLBACK | CMD_BEGIN | CMD_SAVEPOINT,
  CMD_ALL       = 0xFFFFFFFFu
};

struct FilterEntry
{
  std::string name;                    /* lowercase identifier(s)          */
  unsigned cmds;                       /* union of CommandBits             */
};

struct FilterRules
{
  /* From selective_trace_schemas: lowercase schema names. */
  std::vector<FilterEntry> schemas;
  /* From "schema.*" entries of selective_trace_tables. */
  std::vector<FilterEntry> wildcard_schemas;
  /* From selective_trace_tables: lowercase "schema.table". */
  std::vector<FilterEntry> tables;
  /* From selective_trace_connections: connection ids, sorted. A
     listed connection is traced in full (all its statements), regardless
     of the schema/table filters. */
  std::vector<unsigned long long> connections;

  bool empty() const
  {
    return schemas.empty() && wildcard_schemas.empty() &&
           tables.empty() && connections.empty();
  }
};

/*
  Parse the two comma separated lists into *out.

  - Tokens are trimmed of ASCII whitespace and lowercased; empty tokens are
    ignored (so "a,,b" and trailing commas are fine).
  - Optional backticks around each identifier part are stripped.
  - Table tokens must be schema.table; "schema.*" is accepted as a
    whole-schema wildcard (equivalent to listing the schema in the schema
    list). Anything else (missing dot, empty part, extra dot) is invalid.
  - Every entry accepts an optional command qualifier after ':' —
    "vendas:insert|update", "app.pedidos:delete", "logs.*:dml". Valid
    tokens: select, insert, update, delete, replace, load, call, create,
    alter, drop, truncate, rename, other, and the groups dml, ddl, all.
    No qualifier means all commands. Unknown tokens make the parse fail.
  - Duplicated entries have their command masks merged (OR).
  - NULL pointers are treated as empty lists.

  Returns true on success. On failure returns false and stores the
  offending token in *error (out is left untouched).
*/
bool parse_filter_lists(const char *schemas_csv, const char *tables_csv,
                        FilterRules *out, std::string *error);

/*
  Parse a comma-separated list of connection ids (decimal, unsigned) into
  out->connections (sorted, de-duplicated). Empty/NULL means no connection
  filter. On an invalid token returns false and stores it in *error
  (out->connections is left untouched).
*/
bool parse_connection_list(const char *conns_csv, FilterRules *out,
                           std::string *error);

/* True if conn_id is in the connection filter. */
bool match_connection(const FilterRules &rules, unsigned long long conn_id);

/*
  Union of the command masks of every schema-filter entry matching db
  (schemas list and schema.* wildcards). 0 = no match. Empty rules never
  match — callers get the "both lists empty means log nothing" fail-safe
  for free.
*/
unsigned match_schema(const FilterRules &rules,
                      const char *db, size_t db_len);

/*
  Union of the command masks of every table-filter entry matching
  db.table (exact entries and schema.* wildcards). 0 = no match.
*/
unsigned match_table(const FilterRules &rules, const char *db, size_t db_len,
                     const char *table, size_t table_len);

/*
  Map an uppercased keyword produced by extract_command() to its
  CommandBits bit. WITH maps to CMD_SELECT (CTEs are overwhelmingly
  selects); anything unknown maps to CMD_OTHER.
*/
unsigned command_bit(const char *cmd);

/*
  Extract the first SQL keyword of query, uppercased ("SELECT", "INSERT",
  "WITH", ...), skipping leading whitespace, parentheses and comments of
  all three SQL flavors: block comments, executable comments
  ("slash-star-bang NNNNN" — the content IS the statement), "-- " line
  comments and "#" line comments. Writes "OTHER" when no keyword is found.
  buf receives at most buf_size bytes including the terminating NUL.
*/
void extract_command(const char *query, size_t query_len,
                     char *buf, size_t buf_size);

/*
  Copy query into *out, replacing credential literals with ***. Covers the
  authentication clauses of DCL: IDENTIFIED BY '...', IDENTIFIED BY
  PASSWORD '...', IDENTIFIED [WITH plugin] {BY|AS|USING} '...', PASSWORD(...)
  / PASSWORD '...', and SET PASSWORD ... = '...' / = PASSWORD('...').
  Quoted strings ('...' and "...") after those keywords have their body
  replaced. Case-insensitive; keyword matching respects word boundaries.
  Returns true if anything was masked (so callers can note it).
*/
bool mask_secrets(const char *query, size_t query_len, std::string *out);

/* Append src as a JSON string body (no quotes added) into *out. */
void json_escape_append(std::string *out, const char *src, size_t len);

/* Append src as a SQL single-quoted string body (no quotes added). Valid
   only against a connection running with sql_mode NOT containing
   NO_BACKSLASH_ESCAPES — callers that own the connection must pin that. */
void sql_escape_append(std::string *out, const char *src, size_t len);

} /* namespace selective_trace */

#endif /* SELECTIVE_TRACE_FILTER_ENGINE_H */
