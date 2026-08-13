# USAGE.md — `selective_trace` plugin usage guide (MySQL 8.0+)

Selective query trace plugin for **MySQL 8.0.24+** (needs the
`mysql_command_services` component, added in 8.0.24 — see the TABLE mode
note below) that logs only queries touching the configured
schemas/tables — a low-overhead, partial alternative to `general_log`.

> **Status**: `INSTALL PLUGIN`, FILE mode and TABLE mode have all been
> exercised end to end against a real `mysqld` 8.0.40 in Docker (Etapa 5,
> in progress). Filtering by schema, `min_duration_ms`, `mask_passwords`
> and connection identity have **not** been individually re-verified yet
> in that same session — treat those as documented-but-not-yet-drilled.
> See `docs/RESEARCH_NOTES_MYSQL.md` "Etapa 5" for exactly what was run
> and what's still open. §1 below has a **mandatory** extra `GRANT` step
> for TABLE mode that earlier versions of this doc did not have — do not
> skip it.

---

## 0. Known limitations (read this first)

- **DDL is not table-filterable.** MySQL 8.0's audit API only fires a
  table-level event (`TABLE_ACCESS`) for `SELECT`/`INSERT`/`UPDATE`/
  `DELETE`. `CREATE TABLE`, `ALTER TABLE`, `DROP TABLE`, `TRUNCATE`,
  `RENAME TABLE` never raise it. A filter entry like
  `vendas.pedidos:ddl` will **never** match — there is no table name
  available for DDL. To catch DDL, filter by **schema** (works via a
  best-effort current-schema tracker, see below) or by **connection**.
- **`query_seq` is not the server's query id.** MySQL's audit event
  carries no `query_id` field (unlike MariaDB). The `query_seq` column/
  field is a per-connection counter local to this plugin — useful to
  order/correlate rows of the same connection, not to cross-reference
  with `performance_schema` or `SHOW PROCESSLIST`.
- **The "current schema" used for DDL/TCL fallback is inferred, not
  read from the server.** It is derived from `Init DB` commands and
  `USE <schema>` statements seen on the connection. It can be stale if
  the client never sends an explicit schema (rare) or if a `USE`
  statement is prefixed by a comment (`/* c */ USE db` — not
  recognized). See `docs/DECISIONS.md` §7.

## 1. Installation

Copy `selective_trace.so` to the server's `plugin_dir` (check with
`SHOW GLOBAL VARIABLES LIKE 'plugin_dir'`) and:

```sql
INSTALL PLUGIN selective_trace SONAME 'selective_trace.so';
```

MySQL has no `plugin-maturity` concept (unlike MariaDB) — there is no
extra flag to pass.

Or via configuration (loads at startup):

```ini
[mysqld]
plugin_load_add=selective_trace.so
```

Uninstall with `UNINSTALL PLUGIN selective_trace;`.

### 1.1 Required grant for TABLE mode (`selective_trace_output = 'TABLE'`)

The TABLE writer runs its `INSERT`s (and the initial `CREATE TABLE IF NOT
EXISTS`) through MySQL's `mysql_command_services` component. That internal
connection authenticates as the built-in **`mysql.session`@`localhost`**
system account — confirmed live (Etapa 5): even though that account
already carries `SUPER`, it still does **not** have `CREATE`/`INSERT` on
an arbitrary table by default, and the writer's first `CREATE TABLE`
silently fails (logged to the server's error log as `selective_trace:
could not create mysql.selective_trace_events (errno ...)`) until this
grant is in place:

```sql
GRANT CREATE, INSERT, SELECT ON mysql.selective_trace_events
  TO 'mysql.session'@'localhost';
FLUSH PRIVILEGES;
```

Run this **once**, as an account with `GRANT OPTION`, before the first
`INSTALL PLUGIN` (or before the first `SET GLOBAL selective_trace_output
= 'TABLE'` if the plugin is already loaded) — the writer's internal
connection is established once and reused, so a grant added *after* the
writer already gave up on creating the table won't take effect until the
plugin is reloaded (`UNINSTALL`/`INSTALL PLUGIN` again, or restart the
server). Table-level privileges on a not-yet-existing table are valid in
MySQL (stored pending in `mysql.tables_priv`), so granting before the
table exists works fine. FILE mode needs no such grant — it never talks
to the server as a client.

## 2. Enabling and configuring

Everything is a `GLOBAL` dynamic system variable — no restart needed.

```sql
SET GLOBAL selective_trace_schemas = 'vendas,rh';
SET GLOBAL selective_trace_output = 'TABLE';
SET GLOBAL selective_trace_enabled = ON;
```

| Variable | Type | Default | Meaning |
|---|---|---|---|
| `selective_trace_enabled` | BOOL | `OFF` | Master on/off switch. |
| `selective_trace_schemas` | VARCHAR | `''` | Comma-separated schema names to trace. |
| `selective_trace_tables` | VARCHAR | `''` | Comma-separated `schema.table` entries (or `schema.*`), matched cross-schema regardless of `selective_trace_schemas`. DML only — see §0. |
| `selective_trace_connections` | VARCHAR | `''` | Comma-separated connection ids (`SHOW PROCESSLIST`). Every statement of a listed connection is traced in full, any command. |
| `selective_trace_output` | ENUM | `FILE` | `FILE` (JSON lines) or `TABLE` (`mysql.selective_trace_events`). |
| `selective_trace_log_file_path` | VARCHAR | `selective_trace.json` | Path used when `output=FILE`. |
| `selective_trace_min_duration_ms` | INT | `0` | Only log statements slower than this. `0` = log everything that matches the filters. |
| `selective_trace_mask_passwords` | BOOL | `ON` | Replace credential literals (`IDENTIFIED BY`, `PASSWORD()`, `SET PASSWORD`) with `***` before logging. |

**Fail-safe**: if `selective_trace_schemas`, `selective_trace_tables` and
`selective_trace_connections` are all empty, **nothing is logged**, even
with `enabled=ON`.

## 3. Filter syntax

### Schemas

```sql
SET GLOBAL selective_trace_schemas = 'vendas, rh';
```

Case-insensitive, comma-separated, optional backticks
(`` `vendas` ``) and optional whitespace around entries.

### Tables (cross-schema, DML only — see §0)

```sql
SET GLOBAL selective_trace_tables = 'appdb.orders, other.users, logs.*';
```

`schema.*` matches every table of that schema (equivalent to adding it to
`selective_trace_schemas`).

### Connections

```sql
SET GLOBAL selective_trace_connections = '42, 108';
```

Decimal connection ids as shown in `SHOW PROCESSLIST` / `PROCESSLIST_ID`.
A listed connection is traced in full — every statement, any command,
overriding the schema/table filters (but still subject to
`min_duration_ms`).

### Command qualifiers

Any schema or table entry accepts an optional `:qualifier` suffix to
narrow which commands are logged for that entry:

```sql
SET GLOBAL selective_trace_schemas = 'vendas:insert|update, rh';
SET GLOBAL selective_trace_tables  = 'app.pedidos:delete, logs.*:dml';
```

Valid tokens: `select`, `insert`, `update`, `delete`, `replace`, `load`,
`call`, `create`, `alter`, `drop`, `truncate`, `rename`, `other`,
`commit`, `rollback`, `begin`, `savepoint`, and the groups `dml`, `ddl`,
`tcl`, `all` (default when no qualifier is given). Multiple tokens are
`|`-separated. An entry with an invalid qualifier is rejected by the
`SET` itself (`ER_WRONG_VALUE_FOR_VAR`), the old value stays active.

Remember §0: a `:ddl`/`:create`/... qualifier on a **table** entry will
never match anything in this MySQL build, because DDL never raises a
`TABLE_ACCESS` event. Put DDL-oriented qualifiers on **schema** entries
instead (relies on the current-schema heuristic) or use the connection
filter.

## 4. Output modes

### FILE

One JSON object per line, appended to `selective_trace_log_file_path`:

```json
{"ts":"2026-08-06 14:03:11.482","conn_id":42,"query_seq":7,"user":"app@10.0.0.5","db":"vendas","tables":["vendas.pedidos"],"command":"UPDATE","duration_ms":3.214,"error_code":0,"query":"UPDATE pedidos SET status='paid' WHERE id=123"}
```

### TABLE

Inserted into `mysql.selective_trace_events`, created automatically on
first use:

```sql
SELECT ts, conn_id, query_seq, db, tables_involved, command, duration_ms
FROM mysql.selective_trace_events
WHERE db = 'vendas'
ORDER BY ts DESC
LIMIT 20;
```

The writer thread runs its own `INSERT`s through a dedicated internal
connection (see `docs/DECISIONS.md` §5) and is excluded from tracing
itself — no self-log loop.

## 5. Status variables

```sql
SHOW GLOBAL STATUS LIKE 'selective_trace%';
```

| Variable | Meaning |
|---|---|
| `selective_trace_events_logged` | Total events written since plugin load. |
| `selective_trace_write_failures` | Failed writes (FILE) or INSERTs (TABLE). |
| `selective_trace_events_dropped` | TABLE mode: events dropped because the internal queue was full (10 000 pending events). |
| `selective_trace_callback_errors` | Exceptions caught at the audit-callback C boundary (should stay at 0). |

## 6. Disabling / removing

```sql
SET GLOBAL selective_trace_enabled = OFF;   -- stop tracing, keep config
UNINSTALL PLUGIN selective_trace;            -- unload entirely
```
