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

#include "filter_engine.h"

#include <cstring>
#include <algorithm>

namespace selective_trace {

static inline char ascii_lower(char c)
{
  return (c >= 'A' && c <= 'Z') ? (char) (c - 'A' + 'a') : c;
}

static inline bool is_space(char c)
{
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* entry (already lowercase) == s[0..len) folded to lowercase? */
static bool ci_eq(const std::string &entry, const char *s, size_t len)
{
  if (entry.size() != len)
    return false;
  for (size_t i= 0; i < len; i++)
    if (entry[i] != ascii_lower(s[i]))
      return false;
  return true;
}

/* entry (lowercase "schema.table") == db + '.' + table, folded? */
static bool ci_eq_qualified(const std::string &entry,
                            const char *db, size_t db_len,
                            const char *table, size_t table_len)
{
  if (entry.size() != db_len + 1 + table_len)
    return false;
  size_t i= 0;
  for (size_t j= 0; j < db_len; j++, i++)
    if (entry[i] != ascii_lower(db[j]))
      return false;
  if (entry[i++] != '.')
    return false;
  for (size_t j= 0; j < table_len; j++, i++)
    if (entry[i] != ascii_lower(table[j]))
      return false;
  return true;
}

/* find entry by name; NULL when absent */
static FilterEntry *find_entry(std::vector<FilterEntry> &list,
                               const char *s, size_t len)
{
  for (size_t i= 0; i < list.size(); i++)
    if (ci_eq(list[i].name, s, len))
      return &list[i];
  return NULL;
}

static void add_entry(std::vector<FilterEntry> &list,
                      const std::string &name, unsigned cmds)
{
  FilterEntry *e= find_entry(list, name.data(), name.size());
  if (e)
    e->cmds|= cmds;                    /* duplicate: merge masks */
  else
  {
    FilterEntry fresh;
    fresh.name= name;
    fresh.cmds= cmds;
    list.push_back(fresh);
  }
}

/*
  Parse the optional ":cmd1|cmd2" qualifier. Returns true and fills *cmds
  (CMD_ALL when there is no qualifier); false on unknown tokens.
*/
static bool parse_cmd_qualifier(const std::string &quals, unsigned *cmds)
{
  static const struct { const char *tok; unsigned mask; } cmd_tokens[]=
  {
    { "select",   CMD_SELECT },   { "insert", CMD_INSERT },
    { "update",   CMD_UPDATE },   { "delete", CMD_DELETE },
    { "replace",  CMD_REPLACE },  { "load",   CMD_LOAD },
    { "call",     CMD_CALL },     { "create", CMD_CREATE },
    { "alter",    CMD_ALTER },    { "drop",   CMD_DROP },
    { "truncate", CMD_TRUNCATE }, { "rename", CMD_RENAME },
    { "other",    CMD_OTHER },
    { "commit",   CMD_COMMIT },   { "rollback", CMD_ROLLBACK },
    { "begin",    CMD_BEGIN },    { "savepoint", CMD_SAVEPOINT },
    { "dml",      CMD_DML },      { "ddl",    CMD_DDL },
    { "tcl",      CMD_TCL },      { "all",    CMD_ALL }
  };

  *cmds= 0;
  size_t pos= 0;
  while (pos <= quals.size())
  {
    size_t bar= quals.find('|', pos);
    if (bar == std::string::npos)
      bar= quals.size();
    std::string tok= quals.substr(pos, bar - pos);
    /* trim */
    while (!tok.empty() && is_space(tok[0]))
      tok.erase(0, 1);
    while (!tok.empty() && is_space(tok[tok.size() - 1]))
      tok.erase(tok.size() - 1);

    bool known= false;
    for (size_t i= 0; i < sizeof(cmd_tokens) / sizeof(cmd_tokens[0]); i++)
      if (tok == cmd_tokens[i].tok)
      {
        *cmds|= cmd_tokens[i].mask;
        known= true;
        break;
      }
    if (!known)
      return false;

    if (bar == quals.size())
      break;
    pos= bar + 1;
  }
  return *cmds != 0;
}

/*
  Split "name[:quals]" and resolve the command mask. Returns false on a
  bad qualifier.
*/
static bool split_cmd_qualifier(const std::string &token,
                                std::string *name, unsigned *cmds)
{
  size_t colon= token.find(':');
  if (colon == std::string::npos)
  {
    *name= token;
    *cmds= CMD_ALL;
    return true;
  }
  *name= token.substr(0, colon);
  /* trim trailing spaces left between name and ':' */
  while (!name->empty() && is_space((*name)[name->size() - 1]))
    name->erase(name->size() - 1);
  return parse_cmd_qualifier(token.substr(colon + 1), cmds);
}

/* Trim whitespace and lowercase. Backticks are handled per identifier
   part (after splitting schema.table at the dot), not here. */
static std::string clean_token(const std::string &raw)
{
  size_t b= 0, e= raw.size();
  while (b < e && is_space(raw[b]))
    b++;
  while (e > b && is_space(raw[e - 1]))
    e--;
  std::string out;
  out.reserve(e - b);
  for (size_t i= b; i < e; i++)
    out.push_back(ascii_lower(raw[i]));
  return out;
}

/* Strip one optional pair of surrounding backticks from an identifier. */
static std::string strip_ticks(const std::string &part)
{
  if (part.size() >= 2 && part[0] == '`' && part[part.size() - 1] == '`')
    return part.substr(1, part.size() - 2);
  return part;
}

static void split_csv(const char *csv, std::vector<std::string> *tokens)
{
  if (csv == NULL)
    return;
  std::string cur;
  for (const char *p= csv; ; p++)
  {
    if (*p == ',' || *p == '\0')
    {
      /* trim before deciding the token is empty */
      std::string t= clean_token(cur);
      if (!t.empty())
        tokens->push_back(t);
      cur.clear();
      if (*p == '\0')
        break;
    }
    else
      cur.push_back(*p);
  }
}

bool parse_filter_lists(const char *schemas_csv, const char *tables_csv,
                        FilterRules *out, std::string *error)
{
  FilterRules rules;

  std::vector<std::string> raw_schemas;
  split_csv(schemas_csv, &raw_schemas);
  for (size_t i= 0; i < raw_schemas.size(); i++)
  {
    std::string name;
    unsigned cmds;
    if (!split_cmd_qualifier(raw_schemas[i], &name, &cmds))
    {
      if (error)
        *error= raw_schemas[i];
      return false;
    }
    std::string schema= strip_ticks(name);
    /* schema names cannot contain a dot; catch table entries put in the
       wrong variable early */
    if (schema.empty() || schema.find('.') != std::string::npos)
    {
      if (error)
        *error= raw_schemas[i];
      return false;
    }
    add_entry(rules.schemas, schema, cmds);
  }

  std::vector<std::string> raw_tables;
  split_csv(tables_csv, &raw_tables);
  for (size_t i= 0; i < raw_tables.size(); i++)
  {
    std::string tok;
    unsigned cmds;
    if (!split_cmd_qualifier(raw_tables[i], &tok, &cmds))
    {
      if (error)
        *error= raw_tables[i];
      return false;
    }
    size_t dot= tok.find('.');
    if (dot == std::string::npos ||           /* no dot at all        */
        dot == 0 ||                           /* empty schema part    */
        dot == tok.size() - 1 ||              /* empty table part     */
        tok.find('.', dot + 1) != std::string::npos) /* more than one dot */
    {
      if (error)
        *error= raw_tables[i];
      return false;
    }
    std::string schema= strip_ticks(tok.substr(0, dot));
    std::string table= strip_ticks(tok.substr(dot + 1));
    if (schema.empty() || table.empty())
    {
      if (error)
        *error= raw_tables[i];
      return false;
    }
    if (table == "*")
      add_entry(rules.wildcard_schemas, schema, cmds);
    else
      add_entry(rules.tables, schema + "." + table, cmds);
  }

  out->schemas.swap(rules.schemas);
  out->wildcard_schemas.swap(rules.wildcard_schemas);
  out->tables.swap(rules.tables);
  return true;
}

bool parse_connection_list(const char *conns_csv, FilterRules *out,
                           std::string *error)
{
  std::vector<std::string> raw;
  split_csv(conns_csv, &raw);

  std::vector<unsigned long long> ids;
  for (size_t i= 0; i < raw.size(); i++)
  {
    const std::string &tok= raw[i];
    /* decimal unsigned only — every char must be a digit */
    if (tok.empty())
      continue;
    unsigned long long v= 0;
    bool ok= true;
    for (size_t k= 0; k < tok.size(); k++)
    {
      char c= tok[k];
      if (c < '0' || c > '9')
      {
        ok= false;
        break;
      }
      v= v * 10ULL + (unsigned long long) (c - '0');
    }
    if (!ok)
    {
      if (error)
        *error= tok;
      return false;
    }
    ids.push_back(v);
  }

  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  out->connections.swap(ids);
  return true;
}

bool match_connection(const FilterRules &rules, unsigned long long conn_id)
{
  return std::binary_search(rules.connections.begin(),
                            rules.connections.end(), conn_id);
}

unsigned match_schema(const FilterRules &rules, const char *db, size_t db_len)
{
  unsigned cmds= 0;
  if (db == NULL || db_len == 0)
    return 0;
  for (size_t i= 0; i < rules.schemas.size(); i++)
    if (ci_eq(rules.schemas[i].name, db, db_len))
      cmds|= rules.schemas[i].cmds;
  for (size_t i= 0; i < rules.wildcard_schemas.size(); i++)
    if (ci_eq(rules.wildcard_schemas[i].name, db, db_len))
      cmds|= rules.wildcard_schemas[i].cmds;
  return cmds;
}

unsigned match_table(const FilterRules &rules, const char *db, size_t db_len,
                     const char *table, size_t table_len)
{
  unsigned cmds= 0;
  if (db == NULL || table == NULL || db_len == 0 || table_len == 0)
    return 0;
  for (size_t i= 0; i < rules.wildcard_schemas.size(); i++)
    if (ci_eq(rules.wildcard_schemas[i].name, db, db_len))
      cmds|= rules.wildcard_schemas[i].cmds;
  for (size_t i= 0; i < rules.tables.size(); i++)
    if (ci_eq_qualified(rules.tables[i].name, db, db_len, table, table_len))
      cmds|= rules.tables[i].cmds;
  return cmds;
}

unsigned command_bit(const char *cmd)
{
  static const struct { const char *kw; unsigned bit; } keywords[]=
  {
    { "SELECT",   CMD_SELECT },   { "WITH",    CMD_SELECT },
    { "INSERT",   CMD_INSERT },   { "UPDATE",  CMD_UPDATE },
    { "DELETE",   CMD_DELETE },   { "REPLACE", CMD_REPLACE },
    { "LOAD",     CMD_LOAD },     { "CALL",    CMD_CALL },
    { "CREATE",   CMD_CREATE },   { "ALTER",   CMD_ALTER },
    { "DROP",     CMD_DROP },     { "TRUNCATE", CMD_TRUNCATE },
    { "RENAME",   CMD_RENAME },
    /* transaction control. START alone is NOT here: only START TRANSACTION
       opens a transaction, and extract_command canonicalizes that to BEGIN;
       bare START (START SLAVE/REPLICA/...) must fall through to CMD_OTHER. */
    { "COMMIT",   CMD_COMMIT },   { "ROLLBACK", CMD_ROLLBACK },
    { "BEGIN",    CMD_BEGIN },    { "SAVEPOINT", CMD_SAVEPOINT }
  };
  for (size_t i= 0; i < sizeof(keywords) / sizeof(keywords[0]); i++)
    if (strcmp(cmd, keywords[i].kw) == 0)
      return keywords[i].bit;
  return CMD_OTHER;
}

void extract_command(const char *query, size_t query_len,
                     char *buf, size_t buf_size)
{
  const char *p= query;
  const char *end= query + query_len;

  if (buf_size == 0)
    return;

  /* skip whitespace, parens and every comment flavor, repeatedly */
  for (;;)
  {
    if (p < end && (is_space(*p) || *p == '('))
    {
      p++;
      continue;
    }
    if (p + 1 < end && p[0] == '/' && p[1] == '*')
    {
      /* executable comments — "!" (MySQL/MariaDB) and "M!" (MariaDB-only):
         skip the marker and version digits, the content is the statement */
      if (p + 2 < end && p[2] == '!')
      {
        p+= 3;
        while (p < end && *p >= '0' && *p <= '9')
          p++;
        continue;
      }
      if (p + 3 < end && p[2] == 'M' && p[3] == '!')
      {
        p+= 4;
        while (p < end && *p >= '0' && *p <= '9')
          p++;
        continue;
      }
      p+= 2;
      while (p + 1 < end && !(p[0] == '*' && p[1] == '/'))
        p++;
      p= (p + 1 < end) ? p + 2 : end;
      continue;
    }
    if (p + 1 < end && p[0] == '-' && p[1] == '-' &&
        (p + 2 == end || is_space(p[2])))
    {
      while (p < end && *p != '\n')
        p++;
      continue;
    }
    if (p < end && *p == '#')
    {
      while (p < end && *p != '\n')
        p++;
      continue;
    }
    break;
  }

  size_t n= 0;
  while (p < end && n + 1 < buf_size && n < 16)
  {
    char c= *p;
    if (c >= 'a' && c <= 'z')
      c= (char) (c - 'a' + 'A');
    else if (!(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') && c != '_')
      break;
    buf[n++]= c;
    p++;
  }
  buf[n]= 0;

  /* Disambiguate START: only "START TRANSACTION" opens a transaction —
     START SLAVE / START REPLICA / START ALL SLAVES are admin commands.
     Canonicalize "START TRANSACTION" to BEGIN (so it maps to CMD_BEGIN and
     is counted together with BEGIN); any other START stays "START", which
     command_bit() maps to CMD_OTHER. */
  if (n == 5 && memcmp(buf, "START", 5) == 0 && buf_size >= 6)
  {
    const char *q= p;
    while (q < end && is_space(*q))
      q++;
    char w[12];
    size_t m= 0;
    while (q < end && m + 1 < sizeof(w))
    {
      char c= *q;
      if (c >= 'a' && c <= 'z')
        c= (char) (c - 'a' + 'A');
      else if (!(c >= 'A' && c <= 'Z'))
        break;
      w[m++]= c;
      q++;
    }
    w[m]= 0;
    if (strcmp(w, "TRANSACTION") == 0)
      memcpy(buf, "BEGIN", 6);          /* 5 chars + NUL */
  }

  if (n == 0)
  {
    const char other[]= "OTHER";
    size_t m= sizeof(other) - 1 < buf_size - 1 ? sizeof(other) - 1
                                               : buf_size - 1;
    memcpy(buf, other, m);
    buf[m]= 0;
  }
}

/* ---- secret masking ------------------------------------------------- */

static char up(char c)
{
  return (c >= 'a' && c <= 'z') ? (char) (c - 'a' + 'A') : c;
}

static bool is_ident_char(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

/* Case-insensitive keyword match at q[i], honoring word boundaries. */
static bool kw_at(const char *q, size_t len, size_t i, const char *kw)
{
  size_t k= 0;
  while (kw[k])
  {
    if (i + k >= len || up(q[i + k]) != kw[k])
      return false;
    k++;
  }
  /* boundary before */
  if (i > 0 && is_ident_char(q[i - 1]))
    return false;
  /* boundary after */
  if (i + k < len && is_ident_char(q[i + k]))
    return false;
  return true;
}

/*
  From index i (just past a trigger keyword), skip separators (spaces, '(',
  '='), then replace the credential value with *** and return the index just
  past it. Handles two value forms:
    - a quoted string ('...' or "...", with backslash escapes), and
    - an unquoted hex literal (0x1234ABCD / X'1234' — password hashes are
      commonly given this way).
  Returns i unchanged (nothing appended) if neither form follows.
*/
static size_t mask_following_quote(const char *q, size_t len, size_t i,
                                   std::string *out, bool *masked)
{
  while (i < len && (q[i] == ' ' || q[i] == '\t' || q[i] == '\n' ||
                     q[i] == '\r' || q[i] == '(' || q[i] == '='))
  {
    out->push_back(q[i]);
    i++;
  }
  if (i >= len)
    return i;

  if (q[i] == '\'' || q[i] == '"')
  {
    char quote= q[i];
    out->push_back(quote);
    out->append("***");
    i++;                                /* opening quote consumed */
    while (i < len)
    {
      if (q[i] == '\\' && i + 1 < len)  /* skip escaped char */
      {
        i+= 2;
        continue;
      }
      if (q[i] == quote)
        break;
      i++;
    }
    if (i < len)                        /* closing quote */
    {
      out->push_back(quote);
      i++;
    }
    *masked= true;
    return i;
  }

  /* unquoted hex literal: 0x... */
  if (i + 1 < len && q[i] == '0' && (q[i + 1] == 'x' || q[i + 1] == 'X'))
  {
    i+= 2;
    while (i < len && ((q[i] >= '0' && q[i] <= '9') ||
                       (q[i] >= 'a' && q[i] <= 'f') ||
                       (q[i] >= 'A' && q[i] <= 'F')))
      i++;
    out->append("0x***");
    *masked= true;
    return i;
  }

  return i;
}

bool mask_secrets(const char *query, size_t query_len, std::string *out)
{
  bool masked= false;
  out->clear();
  out->reserve(query_len + 8);

  size_t i= 0;
  while (i < query_len)
  {
    /* IDENTIFIED [WITH x] {BY|AS|USING} [PASSWORD] '<secret>' */
    if (kw_at(query, query_len, i, "IDENTIFIED"))
    {
      out->append(query + i, 10);
      i+= 10;
      /* copy up to the connector keyword (BY/AS/USING/VIA), passing over an
         optional "WITH <plugin>" or "VIA <plugin>" */
      while (i < query_len)
      {
        if (kw_at(query, query_len, i, "BY"))       { out->append(query+i,2); i+=2; break; }
        if (kw_at(query, query_len, i, "AS"))       { out->append(query+i,2); i+=2; break; }
        if (kw_at(query, query_len, i, "USING"))    { out->append(query+i,5); i+=5; break; }
        if (query[i] == '\'' || query[i] == '"')    break; /* direct literal */
        /* 0x hash right after IDENTIFIED (no connector) */
        if (query[i] == '0' && i+1 < query_len &&
            (query[i+1]=='x' || query[i+1]=='X'))   break;
        out->push_back(query[i]);
        i++;
      }
      /* optional PASSWORD keyword before the literal */
      size_t j= i;
      while (j < query_len && (query[j]==' '||query[j]=='\t'||query[j]=='\n'||query[j]=='\r'))
        j++;
      if (kw_at(query, query_len, j, "PASSWORD"))
      {
        out->append(query + i, j - i);
        out->append(query + j, 8);
        i= j + 8;
      }
      i= mask_following_quote(query, query_len, i, out, &masked);
      continue;
    }
    /* OLD_PASSWORD ( ... )  — the MySQL 4.x hash function */
    if (kw_at(query, query_len, i, "OLD_PASSWORD"))
    {
      out->append(query + i, 12);
      i+= 12;
      i= mask_following_quote(query, query_len, i, out, &masked);
      continue;
    }
    /* PASSWORD ( ... )  or  PASSWORD '<secret>'  (SET PASSWORD, etc.) */
    if (kw_at(query, query_len, i, "PASSWORD"))
    {
      out->append(query + i, 8);
      i+= 8;
      i= mask_following_quote(query, query_len, i, out, &masked);
      continue;
    }
    out->push_back(query[i]);
    i++;
  }
  return masked;
}

/* ---- output escaping -------------------------------------------------
   Pure, format-only helpers used by both writers (FILE/JSON and
   TABLE/SQL). Kept here so the MariaDB and MySQL plugins share one
   implementation instead of two hand-maintained copies. */

void json_escape_append(std::string *out, const char *src, size_t len)
{
  static const char hexdig[]= "0123456789abcdef";
  for (size_t i= 0; i < len; i++)
  {
    unsigned char c= (unsigned char) src[i];
    switch (c)
    {
    case '"':  out->append("\\\"", 2); break;
    case '\\': out->append("\\\\", 2); break;
    case '\b': out->append("\\b", 2); break;
    case '\f': out->append("\\f", 2); break;
    case '\n': out->append("\\n", 2); break;
    case '\r': out->append("\\r", 2); break;
    case '\t': out->append("\\t", 2); break;
    default:
      if (c < 0x20)
      {
        char buf[7]= { '\\', 'u', '0', '0',
                       hexdig[(c >> 4) & 0xF], hexdig[c & 0xF], 0 };
        out->append(buf, 6);
      }
      else
        out->push_back((char) c);
      break;
    }
  }
}

void sql_escape_append(std::string *out, const char *src, size_t len)
{
  for (size_t i= 0; i < len; i++)
  {
    char c= src[i];
    switch (c)
    {
    case '\'': out->append("\\'", 2); break;
    case '\\': out->append("\\\\", 2); break;
    case '\0': out->append("\\0", 2); break;
    case '\n': out->append("\\n", 2); break;
    case '\r': out->append("\\r", 2); break;
    case '\032': out->append("\\Z", 2); break;
    default: out->push_back(c); break;
    }
  }
}

} /* namespace selective_trace */
