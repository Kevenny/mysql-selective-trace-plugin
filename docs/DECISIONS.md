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
batem 194/194 (157 originais + 37 novos — 3 de escape na sessão anterior, 34 de edge cases nesta sessão) contra
esta cópia.

## 2. Testes: 3 casos novos para as funções de escape movidas

`test_filter_logic.cc` ganhou `test_output_escaping()` cobrindo
`json_escape_append`/`sql_escape_append`, que não tinham teste dedicado no
lado MariaDB (eram exercitadas apenas indiretamente via o plugin rodando).
Como elas moraram para o `core` compartilhado e agora são parte da
superfície pública testável sem precisar de um servidor, ganhar cobertura
direta é barato e vale a pena. Total: **194 checks**, todos passando
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

Esse era o pedaço de **maior risco técnico** do porte (CLAUDE.md §10.1) —
os nomes exatos de serviço/função usados em `src/log_writer_table_mysql.cc`
foram confirmados em sessão posterior contra um `mysql-server` real
(8.0.40 e 9.7.2), compilando limpo nas duas séries. Ver
`docs/RESEARCH_NOTES_MYSQL.md`. **Falta ainda** o exercício em runtime
(uma query de verdade passando pelo writer contra um `mysqld` de pé) —
Etapa 5.

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
  container `gcc:13` (`g++ -std=c++17 -Wall -Wextra -Werror`): **194/194
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
- **Sessão seguinte (mesmo dia): validado também contra `mysql-9.7.2`**
  real (série mais nova, Innovation release), volumes Docker separados
  dos do 8.0.40. Achados (detalhes em `docs/RESEARCH_NOTES_MYSQL.md`
  "MySQL 9.x"): a Audit API em si não mudou nada que este plugin use —
  só a toolchain de build precisou de ajuste (`scripts/build.sh` agora
  passa `-DCMAKE_C_COMPILER`/`-DCMAKE_CXX_COMPILER` explícitos, porque o
  CMake do MySQL 9.x exige gcc-toolset-14 por padrão e aborta com
  `FATAL_ERROR` se não achar — a menos que o compilador já esteja
  setado, o que pula a checagem; `gcc-toolset-12` funcionou de boa; e
  `-DWITH_CURL=0`, porque a detecção de CURL do 9.x não aceita o
  `libcurl-devel` do EL8). Um bug de código real também apareceu:
  `NullS` (macro legada) não chega mais transitivamente pelos includes
  do plugin no 9.7.2 — trocado por `nullptr`, testado limpo nas duas
  séries depois. `scripts/build.sh --plugin` foi reexecutado nas duas
  séries após as mudanças, ambas limpas.
- **Testes unitários ampliados na mesma sessão**: 34 novos checks em
  `core/test_filter_logic.cc` cobrindo lacunas reais não exercitadas
  antes — `mask_secrets` com segredo não fechado e sem gatilho nenhum,
  `extract_command` com `buf_size` 0/1/pequeno, merge de máscara de
  comando entre `selective_trace_schemas` e `selective_trace_tables`
  quando o mesmo schema aparece nas duas listas, backtick não pareado,
  overflow de conexão além de 2^64-1. Total agora: **194/194 checks**
  (era 160/160). Dois dos novos testes falharam na primeira tentativa
  por um bug no próprio teste (tamanho de string hardcoded incluindo o
  `\0` como se fosse dado) — corrigido usando `strlen()`/o helper `mask()`
  já existente no arquivo.
- **Ainda não exercido**: `INSTALL PLUGIN` num `mysqld` real, e qualquer
  comportamento em runtime (FILE, TABLE, filtros, `mysql_command_services`
  executando uma query de verdade). Isso é Etapa 5 — precisa de um
  servidor de pé, não só compilar.

## 11. Injeção SQL corrigida — falha ao pinar `sql_mode` agora é fatal

Revisão de segurança nesta sessão encontrou uma vulnerabilidade real (não
teórica) em `src/log_writer_table_mysql.cc`, herdada do mesmo padrão do
plugin MariaDB (`log_writer_table.cc` tem o código idêntico).

O writer TABLE monta o `INSERT` concatenando texto escapado via
`sql_escape_append()` (escape por barra invertida — `'` → `\'`). Esse
escape só é seguro se a sessão não tiver `NO_BACKSLASH_ESCAPES` no
`sql_mode` (parte do modo `ANSI`, usado em instalações "hardened" — não é
cenário exótico). O código já tentava neutralizar isso com
`SET SESSION sql_mode=''` antes de qualquer INSERT, mas **ignorava o
retorno** dessa query: se falhasse, só logava um aviso e seguia inserindo
mesmo assim. Se o `sql_mode` global tivesse `NO_BACKSLASH_ESCAPES` (ou o
`SET` falhasse por qualquer motivo transitório), qualquer usuário cuja
query fosse rastreada pelo filtro podia incluir um `'` desacompanhado no
texto da própria query, que sobrevivia ao escape sem neutralização e
quebrava a string literal do `INSERT` — injeção SQL rodando com o
privilégio da conexão interna do writer (não do usuário que originou a
query), potencialmente elevado já que a conexão não define
`MYSQL_COMMAND_USER_NAME` explicitamente.

**Fix**: `ensure_conn()` agora trata a falha do `SET SESSION sql_mode=''`
como fatal — fecha a conexão e retorna `false`, e o INSERT correspondente
é contado como falha/drop em vez de ser executado com o `sql_mode`
potencialmente inseguro. Recompilado e validado limpo contra MySQL
8.0.40 e 9.7.2 depois do fix. O mesmo furo continua presente no plugin
MariaDB irmão (fora do escopo deste repo corrigir).

## 12. Estado por conexão: mapa próprio em vez do blob THDVAR (Etapa 5)

Etapa 5 (2026-08-13, `docs/RESEARCH_NOTES_MYSQL.md` §5.1) achou que a
técnica do blob "pristine" via `MYSQL_THDVAR_STR` — copiada do plugin
MariaDB, onde funciona — **crasha o `mysqld` 8.0/9.x** logo no primeiro
evento processado depois de `selective_trace_enabled=ON`
(`THDVAR(thd, state)` volta um ponteiro inválido; `st->magic` estoura).
Confirmado ao vivo, reproduzido de forma consistente, corrigido antes de
qualquer uso real.

Substituição: `StatementState` deixou de morar num blob "emprestado" do
mecanismo de sysvar do servidor e passou a viver inteiramente sob
controle do plugin — um `std::unordered_map<MYSQL_THD, StatementState>`
global protegido por um `mysql_rwlock_t` próprio (`state_map_lock`).
Motivos da escolha:

- `std::unordered_map` garante estabilidade de ponteiro/referência para
  elementos existentes através de inserções e rehashes (só apagar o
  próprio elemento invalida o ponteiro — garantia da biblioteca padrão).
  Isso permite `get_state()` devolver um `StatementState*` cru, seguro de
  usar pelo resto do processamento do evento, sem segurar o lock durante
  todo esse tempo.
- Cada `THD` só é tocado por uma conexão/thread por vez (nunca duas
  threads concorrentes para o mesmo `THD`), então não existe corrida de
  "lost update" dentro de uma entrada — o rwlock protege só a estrutura
  interna do mapa contra inserções/buscas concorrentes de *outras*
  conexões.
- Limpeza via `MYSQL_AUDIT_CONNECTION_DISCONNECT` — o plugin passou a
  assinar também `CONNECTION_CLASS` (só esse subclass). A limpeza roda
  independente de `selective_trace_enabled` (só depende de
  `plugin_ready`), para não vazar uma entrada por conexão que desconecta
  com o tracing desligado. Uma conexão que nunca dispara um `DISCONNECT`
  limpo (kill abrupto, falha de rede) vaza uma entrada de ~4 KB —
  aceito como bem melhor que a alternativa de derrubar o servidor.
- O truque do "magic number" para detectar um blob "pristine" não é mais
  necessário: `std::unordered_map::operator[]` em uma chave nova
  value-inicializa o `StatementState` (zero-preenche todos os campos, já
  que a struct não tem construtor definido pelo usuário), então uma
  entrada nova já nasce zerada de graça.

Trade-off aceito: uma tabela hash global com lock é mensuravelmente mais
cara por evento do que um blob já alocado por THD teria sido *se
funcionasse* — mas como a técnica original não funciona nesta série do
servidor, a comparação é acadêmica. Não medido overhead real (Etapa 5
ainda não fez benchmark formal).
