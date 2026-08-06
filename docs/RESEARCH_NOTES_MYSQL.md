# RESEARCH_NOTES_MYSQL.md — Etapa 0

Notas de pesquisa da Audit API do MySQL 8.0, para o porte do
`selective_trace`. Ponto de partida: as tabelas/structs já confirmadas
contra os headers reais do MySQL **8.0.40** (`include/mysql/plugin_audit.h`
e `include/mysql/plugin.h`) em sessão anterior — reproduzidas em
[`CLAUDE.md`](../CLAUDE.md) seção 2. Este arquivo detalha o que essa
pesquisa cobriu, o que ficou pendente, e o que foi assumido nesta sessão de
implementação sem uma árvore de fontes do MySQL 8.0 disponível localmente
para conferência direta.

## Confirmado (headers reais, sessão anterior)

- `MYSQL_AUDIT_INTERFACE_VERSION` = `0x0401`.
- Enum de 13 classes de auditoria (`GENERAL_CLASS=0` … `MESSAGE_CLASS=12`),
  reproduzido em `CLAUDE.md` §2.1.
- `struct mysql_event_general` — 13 campos, **sem** `query_id` e **sem**
  `database`/`db` (diferença relevante vs. MariaDB — ver "O que isso
  quebrou" abaixo). Campos usados neste porte: `event_subclass`,
  `general_error_code`, `general_thread_id`, `general_user`,
  `general_command`, `general_query`, `general_host`,
  `general_sql_command`.
- `struct mysql_event_table_access` — `event_subclass`, `connection_id`,
  `sql_command_id`, `query`, `table_database`, `table_name`. Subclasses:
  **apenas** READ/INSERT/UPDATE/DELETE (sem RENAME, sem cobertura de DDL —
  ver a nota "DDL gap" no topo de `src/selective_trace_mysql.cc`).
- `MYSQL_AUDIT_PLUGIN` = `5` (tipo de plugin).
- Macros de sysvar (`MYSQL_SYSVAR_STR`, `MYSQL_SYSVAR_BOOL`,
  `MYSQL_SYSVAR_ENUM`, `MYSQL_SYSVAR_UINT`, `MYSQL_THDVAR_STR`) existem em
  `plugin.h` com a mesma forma que no MariaDB.
- Sem flag de `plugin-maturity` — MySQL não tem o conceito.

## Pendente / assumido nesta sessão (SEM árvore de fontes do MySQL local)

Esta sessão de implementação rodou sem acesso a um checkout real do MySQL
8.0 (ambiente Windows, sem `mysql-server` clonado). O código em `src/` foi
escrito usando (a) os fatos confirmados acima, (b) conhecimento geral de
plugins de auditoria MySQL 8.0 publicados (ex.: `plugin/audit_null/`), e
(c) o mesmo desenho já validado no plugin MariaDB. Os pontos abaixo
**precisam** ser confirmados contra o `include/` real antes de considerar a
Etapa 0/1 encerrada — cada um tem um comentário `***` no arquivo fonte
correspondente apontando para esta seção.

1. **Layout completo de `struct st_mysql_plugin`** — usado na macro
   `mysql_declare_plugin(...)` no fim de `selective_trace_mysql.cc`.
   Assumido: `type, info, name, author, descr, license, init,
   check_uninstall, deinit, version, status_vars, system_vars,
   __reserved1, flags` (14 campos). Se `check_uninstall` não existir nessa
   posição (ou não existir de todo), o descriptor compila mas com os
   ponteiros de função deslocados — **falha silenciosa**, não erro de
   build. É por isso que a Etapa 1 (`INSTALL PLUGIN` / `SHOW PLUGINS` num
   MySQL 8.0 oficial) é obrigatória antes de qualquer outra validação.

2. **`class_mask[MYSQL_AUDIT_CLASS_MASK_SIZE]`** — assumido como array
   posicional de 13 `unsigned long`, um por classe, cada um sendo uma
   máscara de subclasses daquela classe (o padrão usado em
   `audit_null.cc`). Coerente com CLAUDE.md §2.1, mas o valor exato de
   `MYSQL_AUDIT_CLASS_MASK_SIZE` (deveria ser 13) não foi lido do header
   nesta sessão.

3. **`mysql_command_services`** (modo TABLE) — o maior risco do projeto
   (CLAUDE.md §10.1), isolado em `src/log_writer_table_mysql.cc`. Nomes de
   serviço (`mysql_command_factory`, `mysql_command_options`,
   `mysql_command_query`, `mysql_command_query_result`,
   `mysql_command_field_info`, `mysql_command_error_info`,
   `mysql_command_thread`) e suas funções (`init`, `connect`, `close`,
   `query`, `sql_errno`) seguem a arquitetura certa (todo serviço de
   componente MySQL 8.0 é resolvido por nome via
   `mysql_plugin_registry_acquire()` e convertido para
   `SERVICE_TYPE(...)`), mas os nomes/assinaturas exatos **não foram
   confirmados** contra
   `include/mysql/components/services/mysql_command_services.h`. Etapa 4
   deve validar isso isoladamente (um plugin de teste rodando só
   `SELECT 1`) antes de confiar no modo TABLE em produção.

4. **`thd_get_current_db`-like API** — não assumida (deliberadamente). Ao
   invés de inventar uma função de serviço não confirmada para obter o
   schema corrente da conexão, o rastreamento de schema para
   DDL/TCL/outros comandos (que não passam por `TABLE_ACCESS`) é feito por
   heurística local: capturar `general_query` do comando `Init DB`
   (schema cru) e reconhecer `USE <schema>` via `extract_command()`. Ver
   `track_current_db()` em `selective_trace_mysql.cc`. Isso é uma escolha
   de design deliberada, não uma pendência — mas se uma investigação
   futura confirmar uma função de serviço de plugin para ler o schema
   corrente da THD diretamente, ela é estritamente melhor que a heurística
   e deveria substituí-la.

5. **`MYSQL_THDVAR_STR` + truque do blob "pristine"** — a técnica de
   inicializar o buffer POD por conexão via um valor-default sem NUL (que
   o servidor faz `strdup()` uma vez por THD, com `PLUGIN_VAR_MEMALLOC`)
   funciona no MariaDB e é razoável assumir que funciona no MySQL, já que
   a macro existe idêntica — mas o *timing* exato (a cópia acontece antes
   do primeiro uso da sysvar? relacionado ao registro de plugin vs.
   conexão?) não foi verificado em runtime.

6. **`st_mysql_show_var`** — assumido com 3 campos (`name, value, type`),
   igual ao MariaDB. Pode ter ganhado um campo de `scope`
   (`SHOW_SCOPE_GLOBAL`/...) em alguma versão 8.0 — o código está escrito
   de forma que a inicialização por agregado continua válida mesmo que um
   4º campo exista (fica zero-preenchido), então não deveria quebrar o
   build de qualquer forma; só precisa de confirmação se algum
   comportamento de `SHOW STATUS` parecer errado.

## O que isso quebrou vs. o desenho do MariaDB

- **Sem `query_id` no evento GENERAL** → o plugin usa um contador
  local por conexão (`StatementState::local_seq`) no lugar. Não é o
  `query_id` interno do servidor; documentado em `docs/DECISIONS.md`.
- **Sem `database`/`db` no evento GENERAL** → não há fallback de schema
  "de graça" como no MariaDB (`event->database`). Resolvido pela
  heurística `track_current_db()` (item 4 acima).
- **`TABLE_ACCESS` cobre só DML** → filtros `schema.table:ddl` nunca
  vão casar via evento de tabela; só o filtro de schema (heurístico) ou de
  conexão pode selecionar DDL. Ver a nota "DDL gap" em
  `selective_trace_mysql.cc`.

## Próximos passos (antes de fechar Etapa 0)

1. Clonar `mysql-server` tag `mysql-8.0.40` (ou a versão alvo) e ler, na
   íntegra: `include/mysql/plugin_audit.h`, `include/mysql/plugin.h`,
   `include/mysql/components/services/mysql_command_services.h`,
   `include/mysql/service_command.h`, `plugin/audit_null/audit_null.cc`.
2. Confirmar os 6 pontos pendentes acima linha por linha contra o código
   real; ajustar `selective_trace_mysql.cc` / `log_writer_table_mysql.cc`
   onde divergir.
3. Só então seguir para a Etapa 1 (build num MySQL 8.0 oficial via
   `docker/Dockerfile` + `scripts/build.sh`) com `INSTALL PLUGIN` /
   `SHOW PLUGINS` como critério de aceite mínimo.
