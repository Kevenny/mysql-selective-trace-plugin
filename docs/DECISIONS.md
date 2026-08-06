# DECISIONS.md

Registro das decisões de design do porte `selective_trace` para MySQL 8.0+,
e por que cada uma foi tomada. Complementa `docs/RESEARCH_NOTES_MYSQL.md`
(o que foi pesquisado/confirmado) com o raciocínio de projeto.

## 1. Core compartilhado vendorizado em `core/`, submodule adiado

`core/filter_engine.{h,cc}` e `core/test_filter_logic.cc` são uma cópia
byte-a-byte do `filter_engine` do plugin MariaDB
(`t:\mariadb-selective-log-plugin\src\filter_engine.{h,cc}`), com duas
funções puras (`json_escape_append`, `sql_escape_append`) movidas para
dentro — elas viviam em `log_writer_file.cc`/`log_writer_table.cc` no lado
MariaDB, mas são formatação pura, sem dependência de servidor, e agora os
dois plugins as compartilham.

O plano original (`purrfect-riding-kazoo.md`, CLAUDE.md §7) previa um
repositório `selective-trace-core` à parte, consumido como **git
submodule** pelos dois plugins. Isso não foi feito nesta sessão porque:

- Criar um repositório novo no GitHub e configurar remotes é uma ação que
  o ambiente de desenvolvimento não consegue fazer sozinho (sem `gh` CLI —
  CLAUDE.md §10.3, risco já conhecido e aceito de antemão).
- Este diretório (`t:\mysql-selective-trace-plugin`) nem sequer é um
  repositório git ainda no início desta sessão.

**Ação pendente para o usuário**: quando o `selective-trace-core` for
criado como repo próprio, `core/` aqui deve virar um submodule apontando
para ele, e o MariaDB deve passar pela mesma refatoração (fora do escopo
deste repo). Até lá, qualquer correção ao `filter_engine` precisa ser
replicada manualmente nos dois repos — validado nesta sessão que os testes
batem 160/160 (157 originais + 3 novos para as funções de escape) contra
esta cópia.

## 2. Testes: 3 casos novos para as funções de escape movidas

`test_filter_logic.cc` ganhou `test_output_escaping()` cobrindo
`json_escape_append`/`sql_escape_append`, que não tinham teste dedicado no
lado MariaDB (eram exercitadas apenas indiretamente via o plugin rodando).
Como elas moraram para o `core` compartilhado e agora são parte da
superfície pública testável sem precisar de um servidor, ganhar cobertura
direta é barato e vale a pena. Total: **160 checks**, todos passando
(validado nesta sessão com `gcc:13` via Docker — ver abaixo).

## 3. Sem `RECOMPILE_FOR_EMBEDDED` no CMakeLists

O MariaDB usa essa flag do `MYSQL_ADD_PLUGIN` porque o servidor embarcado
(embedded server) do MariaDB faz link estático, então símbolos do
servidor referenciados pelo plugin (PSI, `thd_*`) precisam do tratamento
especial de `-Wl,--no-undefined`. **MySQL 8.0 não tem mais servidor
embarcado** (removido antes da série 8.0), então essa flag não existe e
não é necessária — confirmado em CLAUDE.md §3 ("sem `RECOMPILE_FOR_EMBEDDED`,
que é coisa de MariaDB").

## 4. Writer FILE: `fopen`/`fwrite` ao invés de um serviço do servidor

O MariaDB usa `service_logger.h` (serviço de logging do próprio servidor).
CLAUDE.md §2 já apontou que o MySQL **não tem** esse serviço para plugins.
A alternativa mais simples e portátil é stdio puro (`fopen`, `fwrite`,
`fflush`) protegido por `mysql_rwlock_t` — mesmo padrão de lock do
MariaDB (rwlock só protege o ciclo de vida do handle, não a escrita byte a
byte), então múltiplas threads de statement não serializam entre si no
caminho comum (handle já aberto → `rdlock`).

`fflush()` acontece a cada linha, para o caso de uso de observar o log com
`tail -f`. Isso é uma troca deliberada de throughput por visibilidade
imediata — se o benchmark da Etapa 5 mostrar que o `fflush` é o gargalo,
vale revisitar (ex.: `setvbuf` em modo bloco + flush periódico por uma
thread separada).

## 5. Writer TABLE: `mysql_command_services` em vez de conexão interna direta

O MariaDB usa `mysql_real_connect_local()` — uma função que qualquer
plugin pode chamar para abrir uma conexão SQL interna sem credenciais,
sem passar pela rede. O MySQL 8.0 não expõe esse atalho a plugins comuns;
o mecanismo suportado desde a 8.0.24 é o serviço de componente
`mysql_command_services`, adquirido via `mysql_plugin_registry_acquire()`.

Esse é o pedaço de **maior risco técnico** do porte (CLAUDE.md §10.1) — os
nomes exatos de serviço/função usados em
`src/log_writer_table_mysql.cc` seguem a arquitetura certa (todo serviço
de componente MySQL 8.0 é resolvido por nome, sem exceção) mas não foram
confirmados contra um header real nesta sessão (sem árvore de fontes do
MySQL disponível). Ver `docs/RESEARCH_NOTES_MYSQL.md` item 3 para o plano
de validação da Etapa 4.

## 6. Sem `query_id` do servidor — contador local por conexão

`struct mysql_event_general` do MySQL 8.0 não tem um campo `query_id`
(diferente do MariaDB). A tabela/JSON de saída usa
`StatementState::local_seq`, um contador incrementado a cada
`GENERAL_LOG` da conexão — identifica statements dentro de uma sessão
(útil para correlacionar linhas de log da mesma conexão) mas **não** é o
id de query interno do servidor. Documentado também em `docs/USAGE.md`.

## 7. Rastreamento de schema corrente via heurística, não via API do servidor

Sem o campo `database` no evento GENERAL (que o MariaDB tem) e sem
confirmação de uma função de serviço tipo `thd_get_current_db()` para
plugins MySQL, o schema "corrente" da conexão — necessário para o
fallback de filtro por schema em comandos que não passam por
`TABLE_ACCESS` (DDL, TCL, etc.) — é inferido localmente:

- Do comando `Init DB` (`general_command == "Init DB"`): `general_query`
  carrega o nome do schema cru.
- De um `USE <schema>` enviado como `COM_QUERY`: reconhecido via
  `extract_command()` (reaproveitado do filter_engine) e parseado
  localmente.

Isso **não invade** território de suposições sobre a API do servidor — só
usa campos já confirmados do struct `mysql_event_general`. O trade-off
documentado: não vê o schema padrão inicial da conexão a menos que o
cliente mande via `Init DB` no connect (a maioria manda, mas não é
garantido), e um `USE` prefixado por comentário de bloco
(`/* c */ USE db`) não é reconhecido por este parser leve (o `db` fica
"stale" até o próximo `USE` reconhecido — falha segura: schema errado
*antigo*, nunca schema errado *inventado*).

## 8. "DDL gap" — limitação aceita, não um bug

`MYSQL_AUDIT_TABLE_ACCESS_CLASS` só cobre
READ/INSERT/UPDATE/DELETE (confirmado em CLAUDE.md §2.2). `CREATE TABLE`,
`ALTER TABLE`, `DROP TABLE`, `TRUNCATE`, `RENAME TABLE` não emitem esse
evento — então este plugin **não consegue** ver o nome da tabela de uma
DDL, diferente do MariaDB (cujo `TABLE_LOCK` cobre abertura de tabela de
forma genérica, incluindo DDL). Um filtro como `vendas.pedidos:ddl` nunca
vai casar via evento de tabela nesta versão MySQL — só o filtro de
conexão, ou o filtro de schema via a heurística do item 7, podem
selecionar uma DDL.

Isso é uma limitação real da Audit API do MySQL 8.0, não um defeito de
implementação. Documentado com destaque no topo de
`src/selective_trace_mysql.cc`, em `docs/RESEARCH_NOTES_MYSQL.md`, e em
`docs/USAGE.md` para o operador que for configurar filtros.

## 9. Sysvars sem sufixo `_to_log`

`selective_trace_schemas`, `selective_trace_tables`,
`selective_trace_connections` (não `_schemas_to_log` etc.) — alinhado com
o plugin MariaDB, cujos sysvars reais (`src/selective_trace.cc`) já usam
esses nomes; o `_to_log` só existia no rascunho de `CLAUDE.md` deste
repo antes desta sessão (inconsistência corrigida a pedido do usuário).

## 10. Verificação feita nesta sessão

- `core/test_filter_logic.cc` compilado e executado dentro de um
  container `gcc:13` (`g++ -std=c++17 -Wall -Wextra -Werror`): **160/160
  checks OK**.
- **Etapa 1 fechada na mesma sessão, em seguida**: `docker/Dockerfile` foi
  buildado, um `mysql-server` tag `mysql-8.0.40` real foi clonado
  (`scripts/build.sh full`), e `src/` foi compilado e linkado contra os
  headers reais via `ninja selective_trace` — zero warnings no fim,
  `selective_trace.so` gerado e com os símbolos esperados de plugin
  dinâmico (`_mysql_plugin_declarations_`, verificado com `nm -D`). O
  fluxo documentado completo (`scripts/build.sh full|--plugin|--package`)
  foi exercitado de ponta a ponta com sucesso.
- Isso só saiu limpo depois de **corrigir vários erros reais** que só um
  compilador real contra os headers reais podia pegar — nenhum deles era
  visível por inspeção (todos pareciam corretos por analogia com o
  MariaDB antes da compilação real):
  - `SYS_VAR`/`SHOW_VAR`, não `st_mysql_sys_var`/`st_mysql_show_var`
    (esses nomes não existem no MySQL — são do MariaDB).
  - `SHOW_VAR` tem 4 campos (`name,value,type,scope`), não 3; o callback
    `SHOW_FUNC` tem 3 parâmetros, não 5; não existe `SHOW_ULONG`.
  - `event_notify` usa `MYSQL_THD`, não `void*` — um arquivo `.pp`
    (snapshot pré-processado, também presente na árvore) sugeria o
    contrário e estava desatualizado/enganoso.
  - `mysql_rwlock_t` e companhia vêm de `<mysql/psi/mysql_rwlock.h>`,
    que `<mysql/psi/mysql_thread.h>` não inclui sozinho; `TYPELIB` vem de
    `<typelib.h>`; `array_elements` é um template em
    `<template_utils.h>`, não uma macro.
  - `mysql_command_error_info::sql_errno()` escreve por ponteiro de
    saída, não retorna o valor.
  - Um symlink `../core/filter_engine.cc` no `CMakeLists.txt` não
    resolvia dentro da árvore do MySQL porque o CMake resolve caminhos
    relativos contra o diretório *lógico* do symlink do plugin, não seu
    alvo real — corrigido com um segundo symlink `src/core -> ../core`
    (fica dentro da árvore, sem precisar de `..`).
  - Todos os detalhes e o texto exato dos headers reais estão em
    `docs/RESEARCH_NOTES_MYSQL.md`.
- **Ainda não exercido**: `INSTALL PLUGIN` num `mysqld` real, e qualquer
  comportamento em runtime (FILE, TABLE, filtros, `mysql_command_services`
  executando uma query de verdade). Isso é Etapa 5 — precisa de um
  servidor de pé, não só compilar.
