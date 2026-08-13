# selective_trace — MySQL 8.0+

Native audit plugin for **MySQL 8.0.24+** that traces (logs) only queries
touching a configurable set of schemas/tables/connections, filtered by
command type — a low-overhead, partial alternative to `general_log`
(which is all-or-nothing).

This is the MySQL port of the [`selective_trace` MariaDB
plugin](../mariadb-selective-log-plugin), sharing its filtering logic
(`core/filter_engine.{h,cc}`, 194 unit-tested checks) but with a from-
scratch server-integration layer, because the MySQL 8.0 Audit API is a
different API from MariaDB's — not a dialect of it. See
[`CLAUDE.md`](CLAUDE.md) for the full API comparison and
[`docs/DECISIONS.md`](docs/DECISIONS.md) for why each MySQL-specific
choice was made.

## Status

**Etapa 5 in progress — `INSTALL PLUGIN` and both output modes have run
successfully against a real, live `mysqld` 8.0.40.** The filter engine
(`core/`) is ported, unit-tested and passing (194/194 checks). Both
MySQL 8.0.40 and 9.7.2 source builds compile clean and export the
expected dynamic-plugin symbols. Beyond that, this build has now been
loaded into a real Docker `mysqld` 8.0.40 and exercised end to end:
`INSTALL PLUGIN`, `SET GLOBAL selective_trace_enabled=ON`, and both FILE
and TABLE output modes correctly captured a real traced query. Getting
here surfaced and fixed two real issues a compiler alone couldn't catch
— a **crash** (the MariaDB-borrowed per-connection state storage trick
doesn't work on MySQL 8.0/9.x; redesigned around a plugin-owned
`std::unordered_map`, see `docs/DECISIONS.md` §12) and a **required
one-time `GRANT`** for TABLE mode (the writer's internal connection
authenticates as the low-privilege `mysql.session` system account, not a
superuser — see `docs/USAGE.md` §1.1). Filtering variations,
`min_duration_ms`, `mask_passwords`, graceful `UNINSTALL` under load,
Valgrind, the adversarial security suite, and any MySQL 9.7.2 runtime
exercise are still open — see
[`docs/RESEARCH_NOTES_MYSQL.md`](docs/RESEARCH_NOTES_MYSQL.md) "Etapa 5"
for the exact remaining scope. Still not recommended for production.

## Quick start

```sql
INSTALL PLUGIN selective_trace SONAME 'selective_trace.so';
SET GLOBAL selective_trace_schemas = 'vendas';
SET GLOBAL selective_trace_output  = 'TABLE';
SET GLOBAL selective_trace_enabled = ON;

-- ... run traced queries ...

SELECT * FROM mysql.selective_trace_events ORDER BY ts DESC LIMIT 20;
```

Full syntax, all system variables, and known limitations (most notably:
DDL statements cannot be table-filtered in MySQL — see why in
`docs/DECISIONS.md` §8) are in [`docs/USAGE.md`](docs/USAGE.md).

## Building

```bash
docker build -t selective-trace-mysql-dev -f docker/Dockerfile docker/
docker run --rm -it -v "$PWD:/workspace" selective-trace-mysql-dev
# inside the container (MYSQL_VERSION picks the series — defaults to 8.0.40):
MYSQL_VERSION=9.7.2 ./scripts/build.sh full   # clones MySQL source + full build
./scripts/build.sh --plugin                   # incremental: plugin only
./scripts/build.sh --package                  # copy the .so to build/plugin_output/
```

This has been run successfully in this repo against both `mysql-8.0.40`
and `mysql-9.7.2` (source clone + configure + build + incremental plugin
rebuild + package, ~30-45 min the first time per series, seconds after
with the ccache/build volumes warm). See
[`docs/RESEARCH_NOTES_MYSQL.md`](docs/RESEARCH_NOTES_MYSQL.md) for the
exact reproduction steps, volume setup, and what differs building against
9.x.

## Repository layout

```
mysql-selective-trace-plugin/
├── CLAUDE.md                       # porting briefing / API comparison
├── core/                           # shared filter engine (vendored, see DECISIONS.md #1)
│   ├── filter_engine.h / .cc
│   └── test_filter_logic.cc        # 194 checks, g++ -std=c++17, no server headers
├── src/
│   ├── CMakeLists.txt
│   ├── selective_trace_mysql.cc    # entrypoint: descriptor, sysvars, event capture
│   ├── log_writer_file_mysql.h/.cc
│   └── log_writer_table_mysql.h/.cc
├── docker/Dockerfile               # MySQL 8.0 source + C++17 toolchain (OL8)
├── scripts/build.sh
└── docs/
    ├── RESEARCH_NOTES_MYSQL.md     # Etapa 0 — confirmed facts + open items
    ├── DECISIONS.md                # design rationale
    └── USAGE.md                    # operator guide
```

## Running the core unit tests

No server headers needed — pure C++17:

```bash
g++ -std=c++17 -Wall -Wextra -Werror -I core \
    core/test_filter_logic.cc core/filter_engine.cc \
    -o test_filter_logic && ./test_filter_logic
```

## License

GPLv2, same as MySQL and the sibling MariaDB plugin.
