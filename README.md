# selective_trace — MySQL 8.0+

Native audit plugin for **MySQL 8.0.24+** that traces (logs) only queries
touching a configurable set of schemas/tables/connections, filtered by
command type — a low-overhead, partial alternative to `general_log`
(which is all-or-nothing).

This is the MySQL port of the [`selective_trace` MariaDB
plugin](../mariadb-selective-log-plugin), sharing its filtering logic
(`core/filter_engine.{h,cc}`, 160 unit-tested checks) but with a from-
scratch server-integration layer, because the MySQL 8.0 Audit API is a
different API from MariaDB's — not a dialect of it. See
[`CLAUDE.md`](CLAUDE.md) for the full API comparison and
[`docs/DECISIONS.md`](docs/DECISIONS.md) for why each MySQL-specific
choice was made.

## Status

**Etapa 1 done — compiles and links against a real MySQL 8.0.40 source
tree; not yet run against a live server (Etapa 5).** The filter engine
(`core/`) is ported, unit-tested and passing (160/160 checks). The
MySQL-specific layer (`src/`) was built end-to-end via
`docker/Dockerfile` + `scripts/build.sh` against a real `mysql-server`
tag `mysql-8.0.40` checkout: `selective_trace.so` compiles with zero
warnings and exports the symbols expected of a dynamic plugin
(`_mysql_plugin_declarations_`, verified with `nm -D`). What's *not* done
yet is loading it into a running `mysqld` (`INSTALL PLUGIN`) and any
functional exercise (FILE, TABLE, filters) — see
[`docs/RESEARCH_NOTES_MYSQL.md`](docs/RESEARCH_NOTES_MYSQL.md) for the
exact remaining scope (Etapa 5) and for the real compiler-verified facts
that corrected several MariaDB-shaped assumptions along the way (e.g.
`SYS_VAR`/`SHOW_VAR`, not `st_mysql_sys_var`/`st_mysql_show_var`, which
don't exist in MySQL). Do not deploy this to production yet.

## Quick start (once Etapa 5 validation is done)

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
# inside the container:
./scripts/build.sh full        # clones MySQL 8.0 source + full build
./scripts/build.sh --plugin    # incremental: plugin only
./scripts/build.sh --package   # copy the .so to build/plugin_output/
```

This has been run successfully in this repo (source clone + configure +
build + incremental plugin rebuild + package, ~30-45 min the first time,
seconds after with the ccache/build volumes warm). See
[`docs/RESEARCH_NOTES_MYSQL.md`](docs/RESEARCH_NOTES_MYSQL.md) for the
exact reproduction steps and volume setup.

## Repository layout

```
mysql-selective-trace-plugin/
├── CLAUDE.md                       # porting briefing / API comparison
├── core/                           # shared filter engine (vendored, see DECISIONS.md #1)
│   ├── filter_engine.h / .cc
│   └── test_filter_logic.cc        # 160 checks, g++ -std=c++17, no server headers
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
