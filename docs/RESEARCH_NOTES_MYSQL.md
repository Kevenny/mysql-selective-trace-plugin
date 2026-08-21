# RESEARCH_NOTES_MYSQL.md — Etapa 0 / Etapa 1 / Etapa 5

Notas de pesquisa da Audit API do MySQL 8.0/9.x, para o porte do
`selective_trace`.

## Etapa 5 (2026-08-13) — primeira validação em runtime, achados críticos

Subimos um `mysqld` 8.0.40 oficial (container Docker isolado,
`selective-trace-mysql8-test`), instalamos o plugin de verdade e batemos
em dois problemas reais — nenhum dos dois era visível por inspeção nem
pelos 194 testes unitários (que só cobrem `core/`, sem tocar servidor):

### 5.1 — Crash de segurança: `INSTALL PLUGIN` + `SET GLOBAL
selective_trace_enabled=ON` derrubava o `mysqld` (SIGSEGV)

Reproduzido de forma 100% consistente: `INSTALL PLUGIN` sozinho não
derrubava nada (o plugin só fica `ACTIVE`, sem processar eventos
enquanto `enabled=OFF`); a primeira `SET GLOBAL
selective_trace_enabled=ON` — ou qualquer primeiro evento processado daí
em diante — sempre crashava. Stack trace do `mysqld`:

```
/usr/lib64/mysql/plugin/selective_trace.so(+0x5806)
/usr/lib64/mysql/plugin/selective_trace.so(+0x6dca)
mysql_audit_notify(...)
dispatch_command(...)
```

`addr2line` contra o `.so` (compilado com `-g`, símbolos completos)
apontou exatamente `get_state()`, `selective_trace_mysql.cc:180` —
`if (st->magic != STATE_MAGIC)`, ou seja, `THDVAR(thd, state)` retornou
um ponteiro inválido e a primeira desreferência crashou.

**Causa raiz**: a técnica emprestada do plugin MariaDB — um
`MYSQL_THDVAR_STR` com `PLUGIN_VAR_MEMALLOC` cujo valor-default é um blob
"pristine" sem NUL que o servidor copiaria (`strdup`-like) uma vez por
conexão, dando um buffer POD de `sizeof(StatementState)` (~4,2 KB) por
THD — **não funciona da mesma forma no MySQL 8.0/9.x**. Não foi possível
confirmar a causa exata dentro do `sql_plugin.cc` do servidor sem uma
sessão dedicada a isso (candidatos: `PLUGIN_VAR_NOSYSVAR` pode pular o
`strdup()` automático nessa série; ou um valor-default desse tamanho é
tratado diferente do que os exemplos mais curtos usados no MariaDB) — mas
o fix não depende de saber qual exatamente.

**Fix**: removida a dependência do blob THDVAR inteiramente.
`StatementState` agora vive num `std::unordered_map<MYSQL_THD,
StatementState>` global, protegido por um `mysql_rwlock_t` dedicado
(`state_map_lock`), criado/destruído em `init`/`deinit`. Entradas são
criadas sob demanda no primeiro evento de cada conexão e removidas no
evento `MYSQL_AUDIT_CONNECTION_DISCONNECT` (novo: o plugin agora também
assina `CONNECTION_CLASS`, só o subclass `DISCONNECT`). A limpeza roda
independente de `selective_trace_enabled` (só depende de `plugin_ready`),
pra não vazar uma entrada por conexão que desconecta com o tracing
desligado. `std::unordered_map` garante estabilidade de ponteiro/
referência dos elementos existentes através de inserts/rehashes (só
apagar o próprio elemento invalida o ponteiro) — por isso `get_state()`
pode devolver um `StatementState*` cru e seguro de usar pelo resto do
processamento daquele evento sem segurar o lock o tempo todo.

Validado depois do fix: `INSTALL PLUGIN` → `SET GLOBAL
selective_trace_enabled=ON` → tráfego real rastreado (FILE e TABLE,
ver 5.2) → nenhum crash, servidor seguiu no ar. Recompilado limpo contra
8.0.40 e 9.7.2.

### 5.2 — Modo TABLE: `mysql.session`@`localhost` precisa de GRANT explícito

Depois do fix do crash, `INSERT`/`SELECT` rastreados não geravam erro
nenhum, mas `mysql.selective_trace_events` nunca era criada. O log do
`mysqld` mostrava `selective_trace: could not create
mysql.selective_trace_events` (mensagem genérica — código original não
expunha o erro real do servidor).

Diagnóstico: `SHOW PROCESSLIST` revelou que a conexão interna do writer
autentica como **`mysql.session`@`localhost`** — a conta de sistema
interna que o MySQL usa por padrão para `mysql_command_services` quando
`MYSQL_COMMAND_USER_NAME` não é setado explicitamente. Mesmo já tendo
`SUPER` e outros privilégios administrativos por padrão, essa conta
**não tem `CREATE`/`INSERT` em tabelas arbitrárias** — não é o
equivalente do "skip_grants"/full-privilege interno que o
`mysql_real_connect_local()` do MariaDB dá de graça. Isso é uma diferença
de arquitetura real entre os dois mecanismos, não um bug.

**Fix de operação** (não de código): documentado em `docs/USAGE.md` §1.1
como pré-requisito obrigatório de instalação —

```sql
GRANT CREATE, INSERT, SELECT ON mysql.selective_trace_events
  TO 'mysql.session'@'localhost';
```

**Fix de código complementar**: `log_writer_table_mysql.cc` ganhou
`last_errmsg()` (usa `mysql_command_error_info::sql_error()`) — as
mensagens de log agora incluem o texto real do erro do servidor, não só
o número. Isso teria cortado o tempo de diagnóstico pela metade; sem
isso, a única pista era o `errno` genérico.

Validado depois do GRANT: reinstalar o plugin (conexão nova do writer,
já com o privilégio em vigor) → `mysql.selective_trace_events` criada e
populada corretamente com uma linha real
(`testdb.pessoas`, `INSERT`, `5.003ms`). Modo FILE também validado no
mesmo teste (JSON válido, uma linha por evento).

### 5.3 — Vazamento de RSS do MariaDB: padrão presente, sintoma ausente

O plugin irmão MariaDB reportou (v1.2.2) RSS crescendo sem limite no modo
TABLE — ~11-12 KB por evento, até OOM — causado pela conexão interna do
writer (e sua THD) viver para sempre e acumular fragmentação de heap.
Este porte tinha **o mesmo padrão estrutural** no código
(`ensure_conn()` sai cedo se `command_h != NULL`; a conexão só fechava em
erro ou no shutdown).

Medido ao vivo aqui (mysqld 8.0.40, modo TABLE, container reiniciado
entre braços, com um braço de controle para isolar o ruído do InnoDB):

Teste 1 — 100k statements serial:

| Braço | Δ RSS | KB/evento | reconnects |
|---|---:|---:|---:|
| Controle (tracing OFF) | 13 MB | 0,13 | 0 |
| Sem reciclagem | 36 MB | 0,37 | 0 |
| Com reciclagem (fix) | 35 MB | 0,36 | 5 |

Teste 2 — 600k eventos gerados, 8 conexões (fila satura; o writer
processou ~97k inserts por braço):

| Braço | Δ RSS | reconnects | gravados | dropados |
|---|---:|---:|---:|---:|
| Sem reciclagem | 73 MB | 0 | 96.838 | 503.163 |
| Com reciclagem | 68 MB | 4 | 98.247 | 501.753 |

**O sintoma não se reproduz nesta série.** Com 11-12 KB/evento, 100k
eventos custariam ~1,1 GB; custaram 36 MB, dos quais 13 MB são o baseline
do InnoDB (as próprias linhas inseridas). No teste concorrente, as séries
temporais dos dois braços amostradas a cada 30s ficam praticamente
sobrepostas (416→466 MB sem o fix, 418→464 MB com), crescendo linear e
suavemente **sem divergir** — e divergência é justamente o que apareceria
se a conexão de vida longa estivesse acumulando. Ou seja: o
`mysql_command_services` do MySQL não acumula como o
`mysql_real_connect_local()` do MariaDB — mecanismos diferentes de
conexão interna, comportamento de memória diferente. Os reconnects
observados batem com o esperado (98.247/20.000 = 4), então a política
funciona mecanicamente; simplesmente não há, aqui, o vazamento que ela
mitiga lá.

O fix (reciclagem a cada 20k inserts) foi mantido mesmo assim — o padrão
de risco existe no código, o custo medido é nulo (416s com vs 415s sem,
0 dropped, 0 failures nos dois), e mantém paridade com o plugin irmão.
Racional completo em `docs/DECISIONS.md` §13. Método de reprodução: os
binários A/B saem do mesmo build recompilando só a TU do writer com
`-DSELECTIVE_TRACE_RECONNECT_EVERY=0` (comportamento antigo).

### O que ainda falta da Etapa 5

- Filtro por schema/tabela/conexão com múltiplas variações (só testado
  schema simples).
- `min_duration_ms`, `mask_passwords` em runtime.
- `track_current_db()` (heurística de schema corrente) com `USE`
  explícito — o teste rodado usava `db.tabela` totalmente qualificado
  sem `USE`, então `db` saiu vazio no output (esperado, documentado, mas
  não é a mesma coisa que confirmar a heurística funcionando).
- Comportamento no `UNINSTALL PLUGIN` com o writer thread ainda
  processando fila (shutdown gracioso) sob carga.
- Valgrind e a bateria de segurança adversarial completa.
- Mesmos testes funcionais contra MySQL 9.7.2 (só 8.0.40 foi exercitado
  em runtime até agora).

**Status: Etapa 1 fechada em 2026-08-06 — validada contra DUAS séries do
MySQL.** `src/` compila e linka limpo (zero warnings) contra checkouts
reais de:

- `mysql-server` tag **`mysql-8.0.40`**
- `mysql-server` tag **`mysql-9.7.2`**

via `docker/Dockerfile` + `scripts/build.sh` (parametrizado por
`MYSQL_VERSION`) — os comandos documentados (`full`/`--plugin`/`--package`)
foram executados de ponta a ponta com sucesso nas duas séries, e
`selective_trace.so` exporta os símbolos esperados
(`_mysql_plugin_declarations_`, `_mysql_plugin_interface_version_`) para
um plugin `MYSQL_DYNAMIC_PLUGIN` em ambas. **Não validado ainda**:
`INSTALL PLUGIN` num `mysqld` rodando de verdade, nem qualquer exercício
funcional (FILE, TABLE, filtros) — isso é Etapa 5, precisa de um servidor
de pé, em qualquer das duas séries.

## MySQL 9.x — o que muda em relação ao 8.0 (achados desta sessão)

MySQL 9.x (testado com 9.7.2) compartilha a Audit API do 8.0 no que este
plugin usa (mesma `MYSQL_AUDIT_INTERFACE_VERSION 0x0401`, mesmos structs
de evento, mesmas macros de sysvar) — nenhuma mudança de código foi
necessária nesse nível. As diferenças encontradas foram todas na
**toolchain de build**, não na Audit API:

- **Exige C++23** (`-std=gnu++23` no log de build), não C++17/20 como o
  8.0. Não afetou o código deste plugin (escrito em C++17 puro, um
  subconjunto válido), mas é bom saber caso alguma mudança futura use algo
  que o C++23 trate diferente (ex.: `char8_t`, comparações de ponteiro).
- **Exige gcc-toolset-14** em EL8/EL9 por padrão — o CMake de nível
  superior do MySQL 9.x tem uma checagem (`CMakeLists.txt` linha ~311)
  que aborta com `FATAL_ERROR` se não achar um devtoolset compatível,
  **a menos que `CMAKE_C_COMPILER`/`CMAKE_CXX_COMPILER` já estejam
  definidos explicitamente** — nesse caso a checagem é pulada
  inteiramente. `scripts/build.sh` agora sempre passa esses dois
  explicitamente (apontando para o gcc-toolset-12 deste Dockerfile),
  contornando a exigência do 14 — compilar com um toolset mais antigo
  que o "oficialmente recomendado" funcionou sem problema aqui.
- **Detecção de CURL mais estrita** — o `WITH_CURL=system` (padrão) do
  MySQL 9.x falha no CMake (`ADD_LIBRARY cannot create ALIAS target
  "ext::curl"`) contra o `libcurl-devel` do EL8, que é antigo demais.
  Como o plugin não usa nada de HTTP, `-DWITH_CURL=0` resolve e é inócuo
  também no 8.0.x.
- **`NullS` não é mais alcançado transitivamente** pelos includes deste
  plugin (`<mysql/plugin.h>` e cia.) — no 8.0 chegava de algum header
  puxado indiretamente; no 9.7.2 não chega, e o build falhou com `'NullS'
  was not declared in this scope`. Trocado por `nullptr` em
  `selective_trace_mysql.cc` (mais portável, não depende de nenhum
  include extra) — funciona igual nas duas séries.

Essas descobertas foram incorporadas em `scripts/build.sh`
(`configure_cmake()`), que agora passa `-DCMAKE_C_COMPILER`,
`-DCMAKE_CXX_COMPILER` e `-DWITH_CURL=0` incondicionalmente — confirmado
inócuo no 8.0.40 (reexecutado depois da mudança, `--plugin` continua
limpo).

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

Funciona igual para as duas séries — só troca `MYSQL_VERSION` e o nome
dos volumes (para não misturar cache de fontes/build entre 8.0 e 9.x):

```bash
docker build -t selective-trace-mysql-dev -f docker/Dockerfile docker/

# --- MySQL 8.0.40 ---
docker volume create mysql-plugin-src   # cache do clone (~vários GB)
docker volume create mysql-plugin-build
docker volume create mysql-plugin-boost
docker volume create mysql-plugin-ccache
docker run --rm -v "$PWD:/workspace" \
  -v mysql-plugin-src:/opt/mysql-src -v mysql-plugin-build:/opt/mysql-build \
  -v mysql-plugin-boost:/opt/boost -v mysql-plugin-ccache:/opt/ccache \
  -e MYSQL_VERSION=8.0.40 \
  -e MYSQL_SRC_DIR=/opt/mysql-src -e BUILD_DIR=/opt/mysql-build \
  -e BOOST_DIR=/opt/boost -e CCACHE_DIR=/opt/ccache \
  -w /workspace --user root selective-trace-mysql-dev bash -lc '
    export PATH="/opt/rh/gcc-toolset-12/root/usr/bin:${PATH}"
    git config --global --add safe.directory /opt/mysql-src
    ./scripts/build.sh full      # primeira vez: clona + configura + build completo
    ./scripts/build.sh --plugin  # depois: só recompila o plugin
    ./scripts/build.sh --package'

# --- MySQL 9.7.2 (volumes separados, mesmo procedimento) ---
docker volume create mysql9-plugin-src
docker volume create mysql9-plugin-build
docker volume create mysql9-plugin-boost
docker volume create mysql9-plugin-ccache
docker run --rm -v "$PWD:/workspace" \
  -v mysql9-plugin-src:/opt/mysql9-src -v mysql9-plugin-build:/opt/mysql9-build \
  -v mysql9-plugin-boost:/opt/mysql9-boost -v mysql9-plugin-ccache:/opt/ccache \
  -e MYSQL_VERSION=9.7.2 \
  -e MYSQL_SRC_DIR=/opt/mysql9-src -e BUILD_DIR=/opt/mysql9-build \
  -e BOOST_DIR=/opt/mysql9-boost -e CCACHE_DIR=/opt/ccache \
  -w /workspace --user root selective-trace-mysql-dev bash -lc '
    export PATH="/opt/rh/gcc-toolset-12/root/usr/bin:${PATH}"
    git config --global --add safe.directory /opt/mysql9-src
    ./scripts/build.sh full && ./scripts/build.sh --package'
```

## Próximos passos (Etapa 5)

1. Subir um `mysqld` 8.0.40 (e depois 9.7.2) oficial em container
   separado, copiar o `.so` de `build/plugin_output/` para o
   `plugin_dir`, `INSTALL PLUGIN`.
2. Bateria funcional: `selective_trace_schemas`/`_tables`/`_connections`,
   FILE (JSON válido), TABLE (`mysql.selective_trace_events` criada e
   populada, sem loop de auto-log), `min_duration_ms`, `mask_passwords`.
3. Confirmar `track_current_db()` com `USE` e conexão com schema default.
4. Valgrind (zero leaks do plugin) e a bateria de segurança adversarial,
   espelhando o que o MariaDB já tem em `scripts/security-test.sh` e
   `scripts/valgrind-test.sh`.
