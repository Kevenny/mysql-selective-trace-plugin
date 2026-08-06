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
  Standalone unit tests for core/filter_engine.{h,cc}. Shared verbatim by
  the MariaDB and MySQL selective_trace plugins.

  No test framework and no server headers on purpose — build and run with:

      g++ -std=c++17 -Wall -Wextra -Werror \
          -I core core/test_filter_logic.cc core/filter_engine.cc \
          -o test_filter_logic && ./test_filter_logic
*/

#include <cstdio>
#include <cstring>
#include "filter_engine.h"

using selective_trace::FilterRules;
using selective_trace::parse_filter_lists;
using selective_trace::match_schema;
using selective_trace::match_table;

static int failures= 0;
static int checks= 0;

#define CHECK(cond)                                                     \
  do                                                                    \
  {                                                                     \
    checks++;                                                           \
    if (!(cond))                                                        \
    {                                                                   \
      failures++;                                                       \
      std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                                   \
  } while (0)

/* strlen-based convenience wrappers for the (ptr, len) API */
static bool m_schema(const FilterRules &r, const char *db)
{
  return match_schema(r, db, std::strlen(db));
}
static bool m_table(const FilterRules &r, const char *db, const char *tbl)
{
  return match_table(r, db, std::strlen(db), tbl, std::strlen(tbl));
}

static void test_empty_lists()
{
  FilterRules r;
  std::string err;
  CHECK(parse_filter_lists("", "", &r, &err));
  CHECK(r.empty());
  CHECK(parse_filter_lists(NULL, NULL, &r, &err));
  CHECK(r.empty());

  /* fail-safe: empty rules never match anything */
  CHECK(!m_schema(r, "testdb"));
  CHECK(!m_table(r, "testdb", "t1"));
}

static void test_schema_filter()
{
  FilterRules r;
  std::string err;
  CHECK(parse_filter_lists("testdb, other_db", "", &r, &err));
  CHECK(r.schemas.size() == 2);

  CHECK(m_schema(r, "testdb"));
  CHECK(m_schema(r, "TESTDB"));          /* case-insensitive */
  CHECK(m_schema(r, "Other_Db"));
  CHECK(!m_schema(r, "testdb2"));
  CHECK(!m_schema(r, "testd"));
  CHECK(!m_schema(r, ""));

  /* schema filter must not make table filter match */
  CHECK(!m_table(r, "testdb", "t1"));
}

static void test_token_cleanup()
{
  FilterRules r;
  std::string err;
  CHECK(parse_filter_lists("  a  ,, b ,", " `S1`.`T1` , c.d ", &r, &err));
  CHECK(r.schemas.size() == 2);
  CHECK(r.tables.size() == 2);
  CHECK(m_schema(r, "a"));
  CHECK(m_schema(r, "b"));
  CHECK(m_table(r, "s1", "t1"));         /* backticks stripped, lowered */
  CHECK(m_table(r, "S1", "T1"));
  CHECK(m_table(r, "c", "d"));

  /* duplicates are collapsed */
  CHECK(parse_filter_lists("x,X, x ", "y.z,`Y`.`Z`", &r, &err));
  CHECK(r.schemas.size() == 1);
  CHECK(r.tables.size() == 1);
}

static void test_table_filter_cross_schema()
{
  FilterRules r;
  std::string err;
  CHECK(parse_filter_lists("", "appdb.orders,other.users", &r, &err));

  CHECK(m_table(r, "appdb", "orders"));
  CHECK(m_table(r, "APPDB", "ORDERS"));
  CHECK(m_table(r, "other", "users"));
  CHECK(!m_table(r, "appdb", "users"));  /* pair must match together   */
  CHECK(!m_table(r, "other", "orders"));
  CHECK(!m_table(r, "appdb", "orders2"));
  CHECK(!m_table(r, "app", "dborders")); /* no substring confusion     */
  CHECK(!m_table(r, "appdb.or", "ders")); /* dot boundary is exact     */

  /* table filter must not make schema filter match */
  CHECK(!m_schema(r, "appdb"));
}

static void test_wildcard()
{
  FilterRules r;
  std::string err;
  CHECK(parse_filter_lists("", "logs.*,appdb.orders", &r, &err));
  CHECK(r.wildcard_schemas.size() == 1);
  CHECK(r.tables.size() == 1);

  /* schema.* logs every table of the schema... */
  CHECK(m_table(r, "logs", "anything"));
  CHECK(m_table(r, "LOGS", "Other"));
  /* ...and behaves like a schema filter entry */
  CHECK(m_schema(r, "logs"));
  CHECK(!m_schema(r, "appdb"));
  CHECK(!m_table(r, "logsx", "t"));
}

static void test_invalid_tokens()
{
  FilterRules r;
  std::string err;

  /* table entries must be schema.table */
  CHECK(!parse_filter_lists("", "noDot", &r, &err));
  CHECK(err == "nodot");                 /* offending token reported */
  CHECK(!parse_filter_lists("", ".x", &r, &err));
  CHECK(!parse_filter_lists("", "x.", &r, &err));
  CHECK(!parse_filter_lists("", "a.b.c", &r, &err));

  /* schema entries cannot contain a dot */
  CHECK(!parse_filter_lists("app.orders", "", &r, &err));
  CHECK(err == "app.orders");

  /* on failure previously parsed state is not leaked into *out */
  FilterRules untouched;
  std::string e2;
  CHECK(parse_filter_lists("keepme", "", &untouched, &e2));
  CHECK(!parse_filter_lists("", "bad", &untouched, &e2));
  CHECK(untouched.schemas.size() == 1);  /* still the old contents */
}

static void test_command_qualifiers()
{
  using namespace selective_trace;
  FilterRules r;
  std::string err;

  CHECK(parse_filter_lists("vendas:insert|update, rh",
                           "app.pedidos:delete, logs.*:dml", &r, &err));
  CHECK(match_schema(r, "vendas", 6) == (CMD_INSERT | CMD_UPDATE));
  CHECK(match_schema(r, "VENDAS", 6) == (CMD_INSERT | CMD_UPDATE));
  CHECK(match_schema(r, "rh", 2) == CMD_ALL);      /* sem qualificador */
  CHECK(match_table(r, "app", 3, "pedidos", 7) == CMD_DELETE);
  CHECK(match_table(r, "app", 3, "outra", 5) == 0);
  CHECK(match_table(r, "logs", 4, "qualquer", 8) == CMD_DML);
  CHECK(match_schema(r, "logs", 4) == CMD_DML);    /* schema.* conta */

  /* entradas duplicadas mesclam as máscaras */
  CHECK(parse_filter_lists("a:insert, A:update", "", &r, &err));
  CHECK(r.schemas.size() == 1);
  CHECK(match_schema(r, "a", 1) == (CMD_INSERT | CMD_UPDATE));

  /* grupos ddl/all e espaços em volta do qualificador */
  CHECK(parse_filter_lists("s:ddl, v : insert | delete", "t.x:all", &r, &err));
  CHECK(match_schema(r, "s", 1) == CMD_DDL);
  CHECK(match_schema(r, "v", 1) == (CMD_INSERT | CMD_DELETE));
  CHECK(match_table(r, "t", 1, "x", 1) == CMD_ALL);

  /* qualificadores inválidos rejeitados com o token ofensor */
  CHECK(!parse_filter_lists("s:banana", "", &r, &err));
  CHECK(err == "s:banana");
  CHECK(!parse_filter_lists("", "a.b:", &r, &err));
  CHECK(!parse_filter_lists("s:", "", &r, &err));

  /* mapeamento keyword -> bit */
  CHECK(command_bit("INSERT") == CMD_INSERT);
  CHECK(command_bit("TRUNCATE") == CMD_TRUNCATE);
  CHECK(command_bit("WITH") == CMD_SELECT);        /* CTE ~ SELECT */
  CHECK(command_bit("GRANT") == CMD_OTHER);
  CHECK(command_bit("OTHER") == CMD_OTHER);

  /* transaction-control commands */
  CHECK(command_bit("COMMIT") == CMD_COMMIT);
  CHECK(command_bit("ROLLBACK") == CMD_ROLLBACK);
  CHECK(command_bit("BEGIN") == CMD_BEGIN);
  CHECK(command_bit("SAVEPOINT") == CMD_SAVEPOINT);
  /* bare START must NOT be a transaction begin (START SLAVE/REPLICA/...);
     only START TRANSACTION opens one, and extract_command turns it into
     BEGIN before command_bit ever sees it */
  CHECK(command_bit("START") == CMD_OTHER);

  /* filter only commits of a schema */
  CHECK(parse_filter_lists("app:commit", "", &r, &err));
  CHECK(match_schema(r, "app", 3) == CMD_COMMIT);
  /* tcl group covers all transaction control */
  CHECK(parse_filter_lists("app:tcl", "", &r, &err));
  CHECK(match_schema(r, "app", 3) ==
        (CMD_COMMIT | CMD_ROLLBACK | CMD_BEGIN | CMD_SAVEPOINT));
  /* commit + rollback together */
  CHECK(parse_filter_lists("app:commit|rollback", "", &r, &err));
  CHECK(match_schema(r, "app", 3) == (CMD_COMMIT | CMD_ROLLBACK));
}

static std::string cmd(const char *q)
{
  char buf[24];
  selective_trace::extract_command(q, std::strlen(q), buf, sizeof(buf));
  return std::string(buf);
}

static void test_extract_command()
{
  CHECK(cmd("SELECT 1") == "SELECT");
  CHECK(cmd("  insert into t values (1)") == "INSERT");
  CHECK(cmd("(SELECT 1) UNION (SELECT 2)") == "SELECT");
  CHECK(cmd("WITH cte AS (SELECT 1) SELECT * FROM cte") == "WITH");

  /* START TRANSACTION canonicalizes to BEGIN; other START commands do not */
  CHECK(cmd("START TRANSACTION") == "BEGIN");
  CHECK(cmd("start transaction read only") == "BEGIN");
  CHECK(cmd("BEGIN") == "BEGIN");
  CHECK(cmd("START SLAVE") == "START");
  CHECK(cmd("START REPLICA") == "START");
  CHECK(cmd("START ALL SLAVES") == "START");
  CHECK(cmd("COMMIT") == "COMMIT");
  CHECK(cmd("ROLLBACK TO SAVEPOINT sp1") == "ROLLBACK");

  /* comentários de linha "--" (caso DBeaver: comentário anexado ao stmt) */
  CHECK(cmd("-- 3. Gerar eventos\nINSERT INTO testdb.t1 VALUES (1,'x')")
        == "INSERT");
  CHECK(cmd("--\nUPDATE t SET v=1") == "UPDATE");
  CHECK(cmd("-- so comentario") == "OTHER");
  /* "--x" sem espaço não é comentário em MariaDB/MySQL: vira token */
  CHECK(cmd("--x") == "OTHER");

  /* comentários "#" */
  CHECK(cmd("# cabecalho\n# outro\ndelete from t") == "DELETE");

  /* comentários de bloco e executáveis */
  CHECK(cmd("/* c1 */ /* c2 */ REPLACE INTO t VALUES (1)") == "REPLACE");
  CHECK(cmd("/*!40000 ALTER TABLE t DISABLE KEYS */") == "ALTER");
  CHECK(cmd("/*M!100400 CREATE TABLE x (a int) */") == "CREATE");

  /* misturas */
  CHECK(cmd(" -- a\n /* b */ # c\n\tCALL proc(1)") == "CALL");
  CHECK(cmd("") == "OTHER");
  CHECK(cmd("   \n\t ") == "OTHER");
}

static std::string mask(const char *q)
{
  std::string out;
  selective_trace::mask_secrets(q, std::strlen(q), &out);
  return out;
}

static void test_mask_secrets()
{
  /* nada a mascarar: query devolvida idêntica */
  CHECK(mask("SELECT * FROM t WHERE v='hello'") ==
        "SELECT * FROM t WHERE v='hello'");
  CHECK(mask("INSERT INTO t VALUES ('data')") ==
        "INSERT INTO t VALUES ('data')");

  /* DCL: senha vira *** preservando aspas */
  CHECK(mask("CREATE USER x@localhost IDENTIFIED BY 'SuperSecret123'") ==
        "CREATE USER x@localhost IDENTIFIED BY '***'");
  CHECK(mask("SET PASSWORD FOR x = PASSWORD('AnotherSecret456')") ==
        "SET PASSWORD FOR x = PASSWORD('***')");
  CHECK(mask("SET PASSWORD = 'plain'") == "SET PASSWORD = '***'");
  CHECK(mask("ALTER USER x IDENTIFIED BY \"double\"") ==
        "ALTER USER x IDENTIFIED BY \"***\"");
  CHECK(mask("GRANT ALL ON *.* TO x IDENTIFIED BY 'p'") ==
        "GRANT ALL ON *.* TO x IDENTIFIED BY '***'");

  /* IDENTIFIED WITH plugin AS/BY '<hash>' */
  CHECK(mask("CREATE USER x IDENTIFIED WITH ed25519 AS 'HASHVAL'") ==
        "CREATE USER x IDENTIFIED WITH ed25519 AS '***'");
  CHECK(mask("CREATE USER x IDENTIFIED BY PASSWORD 'HASHVAL'") ==
        "CREATE USER x IDENTIFIED BY PASSWORD '***'");

  /* case-insensitive e aspas escapadas dentro do segredo */
  CHECK(mask("create user x identified by 'a\\'b'") ==
        "create user x identified by '***'");

  /* não confundir substrings: coluna chamada password_hash não dispara
     mascaramento indevido do valor de outra coluna */
  CHECK(mask("UPDATE t SET note='my password is safe' WHERE id=1") ==
        "UPDATE t SET note='my password is safe' WHERE id=1");

  /* hashes hexadecimais sem aspas (0x...) também são mascarados */
  CHECK(mask("CREATE USER x IDENTIFIED BY PASSWORD 0x1234ABCD") ==
        "CREATE USER x IDENTIFIED BY PASSWORD 0x***");
  CHECK(mask("GRANT ALL TO x IDENTIFIED BY 0xDEADBEEF") ==
        "GRANT ALL TO x IDENTIFIED BY 0x***");
  /* conector VIA (sintaxe MariaDB) + hash hex */
  CHECK(mask("ALTER USER x IDENTIFIED VIA plug USING 0xAAAA") ==
        "ALTER USER x IDENTIFIED VIA plug USING 0x***");
  /* OLD_PASSWORD() function */
  CHECK(mask("SET PASSWORD = OLD_PASSWORD('secret')") ==
        "SET PASSWORD = OLD_PASSWORD('***')");
  /* dois usuários numa CREATE USER: ambos mascarados */
  CHECK(mask("CREATE USER a IDENTIFIED BY 'p1', b IDENTIFIED BY 'p2'") ==
        "CREATE USER a IDENTIFIED BY '***', b IDENTIFIED BY '***'");
  /* 0x fora de contexto de senha NÃO é mascarado (não é falso positivo) */
  CHECK(mask("SELECT * FROM t WHERE id = 0xFF") ==
        "SELECT * FROM t WHERE id = 0xFF");
  CHECK(mask("INSERT INTO t VALUES (0xCAFE, 'x')") ==
        "INSERT INTO t VALUES (0xCAFE, 'x')");
}

static void test_connection_filter()
{
  using namespace selective_trace;
  FilterRules r;
  std::string err;

  /* empty = no connections */
  CHECK(parse_connection_list("", &r, &err));
  CHECK(r.connections.empty());
  CHECK(parse_connection_list(NULL, &r, &err));
  CHECK(r.connections.empty());

  /* parse, sort, dedupe */
  CHECK(parse_connection_list("42, 7, 42, 100 , 7", &r, &err));
  CHECK(r.connections.size() == 3);       /* 7, 42, 100 */
  CHECK(match_connection(r, 7));
  CHECK(match_connection(r, 42));
  CHECK(match_connection(r, 100));
  CHECK(!match_connection(r, 8));
  CHECK(!match_connection(r, 0));

  /* large id (64-bit) */
  CHECK(parse_connection_list("18446744073709551615", &r, &err));
  CHECK(match_connection(r, 18446744073709551615ULL));

  /* empty tokens between commas are ignored */
  CHECK(parse_connection_list("1,,2,", &r, &err));
  CHECK(r.connections.size() == 2);

  /* invalid tokens rejected with the offending token */
  CHECK(!parse_connection_list("42,abc", &r, &err));
  CHECK(err == "abc");
  CHECK(!parse_connection_list("-1", &r, &err));   /* '-' is not a digit */
  CHECK(!parse_connection_list("4 2", &r, &err));  /* embedded space */

  /* connections make FilterRules non-empty (fail-safe interplay) */
  FilterRules r2;
  CHECK(parse_filter_lists("", "", &r2, &err));
  CHECK(r2.empty());
  CHECK(parse_connection_list("5", &r2, &err));
  CHECK(!r2.empty());
}

static void test_match_null_safety()
{
  FilterRules r;
  std::string err;
  CHECK(parse_filter_lists("a", "b.c", &r, &err));
  CHECK(!match_schema(r, NULL, 0));
  CHECK(!match_table(r, NULL, 0, "c", 1));
  CHECK(!match_table(r, "b", 1, NULL, 0));
}

static void test_output_escaping()
{
  using selective_trace::json_escape_append;
  using selective_trace::sql_escape_append;

  std::string j;
  json_escape_append(&j, "a\"b\\c\n\td", std::strlen("a\"b\\c\n\td"));
  CHECK(j == "a\\\"b\\\\c\\n\\td");

  std::string j2;
  char ctrl[]= { 0x01, 0 };
  json_escape_append(&j2, ctrl, 1);
  CHECK(j2 == "\\u0001");

  std::string s;
  sql_escape_append(&s, "a'b\\c\nd", std::strlen("a'b\\c\nd"));
  CHECK(s == "a\\'b\\\\c\\nd");
}

/* ------------------------------------------------------------------------
   Additional edge-case coverage (added after the Etapa 1 real-header
   build session — filling gaps that the original 157+3 checks did not
   exercise, not padding: every case below documents a behavior contract
   that a future change to filter_engine.cc could silently break).
   ------------------------------------------------------------------------ */

static void test_output_escaping_edge_cases()
{
  using selective_trace::json_escape_append;
  using selective_trace::sql_escape_append;

  /* empty input: no crash, empty output */
  std::string j_empty;
  json_escape_append(&j_empty, "", 0);
  CHECK(j_empty.empty());
  std::string s_empty;
  sql_escape_append(&s_empty, "", 0);
  CHECK(s_empty.empty());

  /* clean input passes through byte-for-byte */
  std::string j_clean;
  json_escape_append(&j_clean, "hello world", 11);
  CHECK(j_clean == "hello world");

  /* DEL (0x7F) is outside JSON's mandatory-escape range (<0x20) and is
     passed through unchanged, same as any other printable-ish byte */
  std::string j_del;
  char del[]= { (char) 0x7F, 0 };
  json_escape_append(&j_del, del, 1);
  CHECK(j_del.size() == 1 && (unsigned char) j_del[0] == 0x7F);

  /* every JSON-significant control char in one pass, in order */
  std::string j_all;
  json_escape_append(&j_all, "\"\\\b\f\n\r\t", 7);
  CHECK(j_all == "\\\"\\\\\\b\\f\\n\\r\\t");

  /* SUB (0x1A / Ctrl-Z) is MySQL/SQL-significant (\\Z), not JSON-significant
     (falls into the generic <0x20 \\u00XX branch there) */
  std::string j_sub;
  char sub[]= { (char) 0x1A, 0 };
  json_escape_append(&j_sub, sub, 1);
  CHECK(j_sub == "\\u001a");
  std::string s_sub;
  sql_escape_append(&s_sub, sub, 1);
  CHECK(s_sub == "\\Z");
}

static void test_extract_command_edge_cases()
{
  using selective_trace::extract_command;

  /* buf_size == 0: must not write anything (no room even for the NUL) */
  char sentinel[4]= { 'X', 'X', 'X', 'X' };
  extract_command("SELECT 1", 8, sentinel, 0);
  CHECK(sentinel[0] == 'X');   /* untouched */

  /* buf_size == 1: only room for the terminating NUL */
  char one[1]= { 'X' };
  extract_command("SELECT 1", 8, one, 1);
  CHECK(one[0] == 0);

  /* buf smaller than the keyword: truncates, still NUL-terminated */
  char small[4];
  extract_command("SELECT 1", 8, small, sizeof(small));
  CHECK(std::string(small) == "SEL");

  /* query that is only comments/whitespace, no keyword at all */
  CHECK(cmd("/* just a comment */") == "OTHER");
  CHECK(cmd("((( )))") == "OTHER");

  /* keyword immediately followed by end-of-string (no trailing space) */
  CHECK(cmd("COMMIT") == "COMMIT");
}

static void test_mask_secrets_edge_cases()
{
  using selective_trace::mask_secrets;

  /* IDENTIFIED with no connector and no literal at all: nothing to mask,
     query passed through unchanged, mask_secrets() reports false */
  const char *q1= "CREATE USER x IDENTIFIED";
  std::string out1;
  bool masked1= mask_secrets(q1, std::strlen(q1), &out1);
  CHECK(!masked1);
  CHECK(out1 == q1);

  /* unterminated quoted secret: the value is masked (truncated, not
     reproduced) rather than left unescaped or crashing */
  CHECK(mask("IDENTIFIED BY 'never closes") ==
        "IDENTIFIED BY '***");   /* no closing quote emitted */

  /* PASSWORD keyword with nothing following it at all */
  const char *q3= "SET PASSWORD";
  std::string out3;
  bool masked3= mask_secrets(q3, std::strlen(q3), &out3);
  CHECK(!masked3);
  CHECK(out3 == q3);

  /* two independent triggers in one statement, both masked */
  CHECK(mask("SET PASSWORD = 'a', PASSWORD = 'b'") ==
        "SET PASSWORD = '***', PASSWORD = '***'");
}

static void test_filter_cross_list_merge()
{
  /* the same schema listed both explicitly (selective_trace_schemas) and
     via a schema.* wildcard (selective_trace_tables) must have its
     command masks unioned across both lists when matching */
  FilterRules r;
  std::string err;
  CHECK(selective_trace::parse_filter_lists("app:select", "app.*:insert",
                                            &r, &err));
  CHECK(selective_trace::match_schema(r, "app", 3) ==
        (selective_trace::CMD_SELECT | selective_trace::CMD_INSERT));

  /* a table covered by both a wildcard entry and its own specific entry
     gets the union of both masks; a sibling table only gets the
     wildcard's mask */
  FilterRules r2;
  CHECK(selective_trace::parse_filter_lists(
      "", "logs.*:select, logs.important:insert", &r2, &err));
  CHECK(selective_trace::match_table(r2, "logs", 4, "important", 9) ==
        (selective_trace::CMD_SELECT | selective_trace::CMD_INSERT));
  CHECK(selective_trace::match_table(r2, "logs", 4, "other", 5) ==
        selective_trace::CMD_SELECT);

  /* "*" as a literal schema NAME (no dot at all) is not a wildcard —
     wildcard semantics only apply to "schema.*" table entries */
  FilterRules r3;
  CHECK(selective_trace::parse_filter_lists("*", "", &r3, &err));
  CHECK(selective_trace::match_schema(r3, "*", 1));
  CHECK(!selective_trace::match_schema(r3, "anything", 8));
}

static void test_backtick_edge_cases()
{
  FilterRules r;
  std::string err;

  /* a single, unmatched backtick is not stripped (strip_ticks only acts
     on a pair: first AND last char both '`') — it stays part of the
     literal (lowercased) identifier and must be matched verbatim */
  CHECK(selective_trace::parse_filter_lists("`odd", "", &r, &err));
  CHECK(selective_trace::match_schema(r, "`odd", 4));
  CHECK(!selective_trace::match_schema(r, "odd", 3));
}

static void test_connection_list_edge_cases()
{
  using selective_trace::parse_connection_list;
  using selective_trace::match_connection;

  FilterRules r;
  std::string err;

  /* a token that is pure whitespace between commas is trimmed to empty
     and ignored, same as a genuinely empty token */
  CHECK(parse_connection_list("1,   ,2", &r, &err));
  CHECK(r.connections.size() == 2);

  /* decimal literal beyond 2^64-1: every character is still a valid
     digit, so parsing does not fail — the accumulator wraps per normal
     unsigned overflow semantics. Documented contract: parse_connection_list
     validates *character set*, not numeric range. */
  FilterRules r2;
  bool ok= parse_connection_list("99999999999999999999", &r2, &err);
  CHECK(ok);
  CHECK(r2.connections.size() == 1);
}

int main()
{
  test_empty_lists();
  test_schema_filter();
  test_token_cleanup();
  test_table_filter_cross_schema();
  test_wildcard();
  test_invalid_tokens();
  test_command_qualifiers();
  test_extract_command();
  test_mask_secrets();
  test_connection_filter();
  test_match_null_safety();
  test_output_escaping();
  test_output_escaping_edge_cases();
  test_extract_command_edge_cases();
  test_mask_secrets_edge_cases();
  test_filter_cross_list_merge();
  test_backtick_edge_cases();
  test_connection_list_edge_cases();

  if (failures)
  {
    std::fprintf(stderr, "%d/%d checks FAILED\n", failures, checks);
    return 1;
  }
  std::printf("OK — %d checks passed\n", checks);
  return 0;
}
