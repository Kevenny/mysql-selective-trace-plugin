# RESEARCH_NOTES_MYSQL.md — Etapa 0 / Etapa 1

Notas de pesquisa da Audit API do MySQL 8.0, para o porte do
`selective_trace`.

**Status: Etapa 1 fechada em 2026-08-06.** `src/` compila e linka limpo
(zero warnings) contra um checkout real de `mysql-server` tag
`mysql-8.0.40`, via `docker/Dockerfile` + `scripts/build.sh` — os três
comandos documentados (`full`/`--plugin`/`--package`) foram executados de
ponta a ponta com sucesso, e `selective_trace.so` exporta os símbolos
esperados (`_mysql_plugin_declarations_`, `_mysql_plugin_interface_version_`)
para um plugin `MYSQL_DYNAMIC_PLUGIN`. **Não validado ainda**: `INSTALL
PLUGIN` num `mysqld` rodando de verdade, nem qualquer exercício funcional
(FILE, TABLE, filtros) — isso é Etapa 5, precisa de um servidor de pé.

## Confirmado contra os headers reais (dois momentos de pesquisa)

Sessão anterior (pesquisa por leitura, sem compilar):

- `MYSQL_AUDIT_INTERFACE_VERSION` = `0x0401`.
- Enum de 13 classes de auditoria (`GENERAL_CLASS=0` … `MESSAGE_CLASS=12`),
  reproduzido em `CLAUDE.md` §2.1.
- `struct mysql_event_general` — 13 campos, **sem** `query_id` e **sem**
  `database`/`db` (diferença relevante vs. MariaDB — ver "O que isso
  quebrou" abaixo).
- `struct mysql_event_table_access` — `event_subclass`, `connection_id`,
  `sql_command_id`, `query`, `table_database`, `table_name`. Subclasses:
  **apenas** READ/INSERT/UPDATE/DELETE (sem cobertura de DDL — ver a nota
  "DDL gap" no topo de `src/selective_trace_mysql.cc`).
- `MYSQL_AUDIT_PLUGIN` = `5`.

Esta sessão (compilando de verdade contra `mysql-server` tag
`mysql-8.0.40`, clonado em `docker/Dockerfile` + `scripts/build.sh`, e
grepado diretamente dentro do container):

- **`SYS_VAR`, não `st_mysql_sys_var`** — o tipo público para ponteiros de
  sysvar no MySQL 8.0 é `SYS_VAR` (forward-declarado em `plugin.h`, `struct
  SYS_VAR;`). `st_mysql_sys_var` **não existe** no MySQL — é nome do
  MariaDB. `MYSQL_SYSVAR(name)` expande para `(SYS_VAR *)&(...)`; um array
  `struct st_mysql_sys_var *[]` não compila (conversão de ponteiro
  incompatível — o compilador nem sabe que são o "mesmo" tipo).
- **`SHOW_VAR`, não `st_mysql_show_var`** — mesma história. Definido em
  `include/mysql/status_var.h`, **4 campos**: `name, value, type, scope`
  (não 3). Não existe `SHOW_ULONG` — `SHOW_LONG` já é documentado como
  "shown as _unsigned_ long". O callback `SHOW_FUNC` é
  `int(MYSQL_THD, SHOW_VAR *, char *)` — **3 parâmetros**, não os 5 do
  MariaDB (sem `system_status_var*` nem `enum_var_type` — esse enum nem é
  público, mora em `sql/set_var.h`, header interno do servidor).
- **`struct st_mysql_plugin`** — confirmado campo a campo em
  `include/mysql/plugin.h`: `type, info, name, author, descr, license,
  init, check_uninstall, deinit, version, SHOW_VAR *status_vars,
  SYS_VAR **system_vars, void *__reserved1, unsigned long flags` — exatos
  14 campos que este porte já assumia (confirmado também pela expansão de
  `mysql_declare_plugin_end`, que é literalmente `{0,0,0,0,0,0,0,0,0,0,0,0,0,0}`
  — 14 zeros). Só os *nomes dos tipos* dos dois ponteiros de array
  precisavam de correção (`SHOW_VAR*`/`SYS_VAR**`, não os nomes MariaDB).
- **`event_notify` usa `MYSQL_THD`, não `void*`.** Um arquivo
  `plugin_audit.h.pp` (uma cópia "achatada"/pré-processada, também presente
  na árvore, aparentemente um artefato de geração de docs/ABI-check) sugeria
  `void*` — **mas o header realmente incluído na build**
  (`include/mysql/plugin_audit.h`) declara
  `int (*event_notify)(MYSQL_THD, mysql_event_class_t, const void *);`.
  Lição: quando os dois divergem, o compilador é o juiz — não o `.pp`.
- **`mysql_rwlock_t`/`PSI_rwlock_key`/`PSI_rwlock_info`/`mysql_rwlock_register`
  vêm de `<mysql/psi/mysql_rwlock.h>`, que `<mysql/psi/mysql_thread.h>`
  NÃO inclui automaticamente.** Precisa dos dois includes.
- **`TYPELIB` vem de `<typelib.h>`**, não é puxado transitivamente por
  `plugin.h` no MySQL (era no MariaDB).
- **`array_elements` é um template `constexpr`** em `<template_utils.h>`
  no MySQL 8.0, não uma macro.
- **`mysql_command_services`** (modo TABLE) — confirmado nome por nome
  contra `include/mysql/components/services/mysql_command_services.h` E
  contra um plugin de referência real que usa exatamente este serviço a
  partir de um `st_mysql_plugin` comum (não um componente):
  `plugin/test_services/test_services_command_services.cc`. Achados:
  - Os 7 serviços (`mysql_command_factory`, `_options`, `_query`,
    `_query_result`, `_field_info`, `_error_info`, `_thread`) existem com
    os nomes já usados neste porte.
  - `mysql_command_thread` **existe de verdade** e é exatamente para o
    nosso caso (thread de plugin criada via `pthread_create`, nunca
    passou pela máquina de threads do servidor).
  - `mysql_command_error_info::sql_errno()` **escreve o errno por
    ponteiro de saída** e retorna `bool` (falha) — não retorna o errno
    diretamente. O código original assumia retorno direto; corrigido.
  - Variáveis de serviço devem ser tipadas `SERVICE_TYPE_NO_CONST(name)`
    (mutável), não `SERVICE_TYPE(name)` (const) — é o que o plugin de
    referência usa para estáticas do próprio plugin.
  - `MYSQL_COMMAND_LOCAL_THD_HANDLE` só é necessário se você quiser rodar
    a query numa THD *existente*; deixado de fora deliberadamente (o
    padrão — "se nulo, uma THD interna nova é criada" — é exatamente o
    que o writer TABLE quer).
- **Macros de sysvar** (`MYSQL_SYSVAR_STR`, `MYSQL_SYSVAR_BOOL`,
  `MYSQL_SYSVAR_ENUM`, `MYSQL_SYSVAR_UINT`, `MYSQL_THDVAR_STR`,
  `THDVAR`) — confirmadas com a mesma forma/campos que o MariaDB, exceto
  os nomes de tipo já cobertos acima.
- `PSI_rwlock_info` tem **5 campos** (`m_key, m_name, m_flags,
  m_volatility, m_documentation`), como já estava assumido no código.

## Ainda não exercido em runtime (Etapa 5)

1. **`MYSQL_THDVAR_STR` + truque do blob "pristine"** — compila, mas o
   comportamento em runtime (o `strdup()` do valor-default acontecendo
   antes do primeiro uso, uma vez por conexão) não foi observado com um
   servidor de verdade.
2. **`mysql_command_services` em runtime** — compila limpo contra os
   headers reais, mas nenhuma query foi de fato executada através dele
   (precisa de um `mysqld` rodando e o plugin instalado). Continua sendo
   o maior risco funcional do porte (CLAUDE.md §10.1) até isso acontecer.
3. **`INSTALL PLUGIN` / `SHOW PLUGINS`** — não executado. O `.so` exporta
   os símbolos certos para um plugin dinâmico (`_mysql_plugin_declarations_`,
   `_mysql_plugin_interface_version_`, verificado com `nm -D`), o que é um
   bom sinal, mas não substitui carregar de verdade.
4. **`track_current_db()`** (heurística de schema corrente) e o "DDL gap"
   — comportamento correto do ponto de vista do compilador, mas o
   resultado funcional (schema certo capturado nos casos de uso reais)
   só se confirma com tráfego de verdade.

## O que isso quebrou vs. o desenho do MariaDB

- **Sem `query_id` no evento GENERAL** → o plugin usa um contador
  local por conexão (`StatementState::local_seq`) no lugar. Não é o
  `query_id` interno do servidor; documentado em `docs/DECISIONS.md`.
- **Sem `database`/`db` no evento GENERAL** → não há fallback de schema
  "de graça" como no MariaDB (`event->database`). Resolvido pela
  heurística `track_current_db()`.
- **`TABLE_ACCESS` cobre só DML** → filtros `schema.table:ddl` nunca
  vão casar via evento de tabela; só o filtro de schema (heurístico) ou de
  conexão pode selecionar DDL. Ver a nota "DDL gap" em
  `selective_trace_mysql.cc`.

## Como reproduzir a validação de build desta sessão

```bash
docker build -t selective-trace-mysql-dev -f docker/Dockerfile docker/
docker volume create mysql-plugin-src   # cache do clone (~vários GB)
docker volume create mysql-plugin-build
docker volume create mysql-plugin-boost
docker volume create mysql-plugin-ccache
docker run --rm -v "$PWD:/workspace" \
  -v mysql-plugin-src:/opt/mysql-src -v mysql-plugin-build:/opt/mysql-build \
  -v mysql-plugin-boost:/opt/boost -v mysql-plugin-ccache:/opt/ccache \
  -e MYSQL_SRC_DIR=/opt/mysql-src -e BUILD_DIR=/opt/mysql-build \
  -e BOOST_DIR=/opt/boost -e CCACHE_DIR=/opt/ccache \
  -w /workspace --user root selective-trace-mysql-dev bash -lc '
    export PATH="/opt/rh/gcc-toolset-12/root/usr/bin:${PATH}"
    git config --global --add safe.directory /opt/mysql-src
    ./scripts/build.sh full      # primeira vez: clona + configura + build completo
    ./scripts/build.sh --plugin  # depois: só recompila o plugin
    ./scripts/build.sh --package'
```

## Próximos passos (Etapa 5)

1. Subir um `mysqld` 8.0.40 oficial (container separado), copiar o `.so`
   de `build/plugin_output/` para o `plugin_dir`, `INSTALL PLUGIN`.
2. Bateria funcional: `selective_trace_schemas`/`_tables`/`_connections`,
   FILE (JSON válido), TABLE (`mysql.selective_trace_events` criada e
   populada, sem loop de auto-log), `min_duration_ms`, `mask_passwords`.
3. Confirmar `track_current_db()` com `USE` e conexão com schema default.
4. Valgrind (zero leaks do plugin) e a bateria de segurança adversarial,
   espelhando o que o MariaDB já tem em `scripts/security-test.sh` e
   `scripts/valgrind-test.sh`.
