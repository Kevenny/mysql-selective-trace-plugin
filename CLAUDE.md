# CLAUDE.md — Briefing do Projeto `selective_trace` para **MySQL 8.0+**

> Este arquivo é o briefing principal para o **Claude Code** desenvolver o
> porte do plugin `selective_trace` (originalmente escrito para MariaDB) para
> o **MySQL 8.0+**. Leia-o por completo antes de escrever qualquer código.
>
> **Projeto irmão (origem):** `selective_trace` para MariaDB, v1.0.1,
> validado e publicado. Repo local: `t:\mariadb-selective-log-plugin`. O
> "cérebro" (`filter_engine`) é **compartilhado** com este projeto — ver
> seção 7.

---

## 0. Estado atual deste projeto

**Somente planejado — nenhum código escrito ainda.** Este diretório contém
por enquanto só este briefing e a pasta `docs/`. O plano aprovado que
originou este projeto está em
`C:\Users\lima\.claude\plans\purrfect-riding-kazoo.md`.

O próximo desenvolvedor (Claude ou humano) deve começar pela **Etapa 0**
(seção 6) e seguir na ordem.

---

## 1. Objetivo

Levar a funcionalidade do `selective_trace` para o **MySQL 8.0+**: um plugin
nativo de **trace seletivo de queries** — alternativa de baixo overhead ao
`general_log`, filtrando por **schema**, **tabela** (cross-schema),
**tipo de comando** e **conexão**, configurável em tempo de execução via
`SET GLOBAL`, com dois modos de saída (**FILE** e **TABLE**).

O `general_log` do MySQL também é tudo-ou-nada; o valor de diagnóstico do
trace seletivo é o mesmo lá.

### Decisões de escopo (já tomadas com o usuário)

- **Somente MySQL 8.0+.** MySQL 5.7 está **fora de escopo** (EOL desde
  out/2023).
- **FILE + TABLE desde o início** (não faseado).
- **Projeto à parte** do MariaDB, com o core compartilhado via **git
  submodule** (seção 7).
- Licença **GPLv2** (MySQL também é GPLv2 — sem impedimento).

---

## 2. Descoberta central — a Audit API do MySQL **é outra API**

> ⚠️ Não é um dialeto da API do MariaDB. As classes, a struct do descriptor
> e o mecanismo de SQL interno são diferentes. **Confirme sempre lendo os
> headers reais do MySQL 8.0**, não assuma pelo MariaDB.

Fatos já confirmados nos headers do **MySQL 8.0.40** (tag `mysql-8.0.40`,
`include/mysql/plugin_audit.h` e `include/mysql/plugin.h`):

| Peça | MariaDB (origem) | **MySQL 8.0 (confirmado)** |
|---|---|---|
| `MYSQL_AUDIT_INTERFACE_VERSION` | 0x0302 / 0x0303 | **0x0401** |
| Classes de audit | GENERAL/CONNECTION/TABLE (bitmask) | **enum de 13 classes** (ver abaixo) |
| Ponto de escrita | `GENERAL_STATUS` (user@host numa string) | `GENERAL_STATUS` — **user/host/ip/sql_command em campos separados** |
| Filtro por tabela | `TABLE_LOCK` (db/table/read_only) | `TABLE_ACCESS` — **operação READ/INSERT/UPDATE/DELETE + sql_command_id + query + db + table** |
| Tipo de plugin | `MYSQL_AUDIT_PLUGIN` (0x0100…) | `MYSQL_AUDIT_PLUGIN 5` |
| Sysvars | `MYSQL_SYSVAR_STR`/`THDVAR_STR` | **macros idênticas** (existem em `plugin.h`) |
| SQL interno (modo TABLE) | `mysql_real_connect_local` | **component `mysql_command_services`** (8.0.24+) |
| FILE output | logger service do servidor | **inexistente** — writer próprio |
| plugin-maturity (flag STABLE) | existe | **não existe** — instala sem flag |

### 2.1 Enum de classes (MySQL 8.0)

```
GENERAL_CLASS=0, CONNECTION_CLASS=1, PARSE_CLASS=2, AUTHORIZATION_CLASS=3,
TABLE_ACCESS_CLASS=4, GLOBAL_VARIABLE_CLASS=5, SERVER_STARTUP_CLASS=6,
SERVER_SHUTDOWN_CLASS=7, COMMAND_CLASS=8, QUERY_CLASS=9,
STORED_PROGRAM_CLASS=10, AUTHENTICATION_CLASS=11, MESSAGE_CLASS=12
```

O descriptor usa `class_mask[MYSQL_AUDIT_CLASS_MASK_SIZE]`. Vamos assinar
**`GENERAL` + `TABLE_ACCESS`**.

### 2.2 Structs de evento relevantes (confirmadas)

```c
/* ponto de escrita — tem tudo que precisamos, campos limpos */
struct mysql_event_general {
  mysql_event_general_subclass_t event_subclass;   /* usar GENERAL_STATUS */
  int                general_error_code;
  unsigned long      general_thread_id;
  MYSQL_LEX_CSTRING  general_user;                 /* separado */
  MYSQL_LEX_CSTRING  general_command;
  MYSQL_LEX_CSTRING  general_query;                /* texto completo */
  const CHARSET_INFO *general_charset;
  unsigned long long general_time;
  unsigned long long general_rows;
  MYSQL_LEX_CSTRING  general_host;                 /* separado — sem parse! */
  MYSQL_LEX_CSTRING  general_sql_command;
  MYSQL_LEX_CSTRING  general_external_user;
  MYSQL_LEX_CSTRING  general_ip;                   /* separado */
};

/* filtro por tabela — mais rico que o TABLE_LOCK do MariaDB */
struct mysql_event_table_access {
  mysql_event_table_access_subclass_t event_subclass; /* READ/INSERT/UPDATE/DELETE */
  unsigned long      connection_id;
  enum_sql_command_t sql_command_id;
  MYSQL_LEX_CSTRING  query;
  const CHARSET_INFO *query_charset;
  MYSQL_LEX_CSTRING  table_database;
  MYSQL_LEX_CSTRING  table_name;
};

/* alternativa de ponto de escrita — sem user/host (menos completo) */
struct mysql_event_query {
  mysql_event_query_subclass_t event_subclass;     /* QUERY_START / QUERY_STATUS_END */
  int                status;
  unsigned long      connection_id;
  enum_sql_command_t sql_command_id;
  MYSQL_LEX_CSTRING  query;
  const CHARSET_INFO *query_charset;
};
```

**Decisão de ponto de escrita:** usar **`GENERAL_STATUS`** (tem user/host/ip
/erro/query — espelha o desenho do MariaDB) + **`TABLE_ACCESS`** para
acumular tabelas e a operação por statement. `QUERY_CLASS` fica como
referência (não carrega user/host).

---

## 3. Linguagem e build

- **C++17** (MySQL 8.0 exige toolchain mais novo que o MariaDB).
- Plugin **DYNAMIC** (`INSTALL PLUGIN ... SONAME '...'`), `.so`.
- Build via **`MYSQL_ADD_PLUGIN(...)`** na árvore de fontes do MySQL 8.0
  (**sem** `RECOMPILE_FOR_EMBEDDED`, que é coisa de MariaDB). Confirmar a
  sintaxe exata em `plugin.cmake` do MySQL e num plugin de referência
  (`plugin/audit_null/`, `components/`).
- Alvo de compatibilidade: EL8/EL9 (glibc 2.17), como no MariaDB.

---

## 4. Requisitos funcionais (paridade com o MariaDB)

As 8 variáveis de sistema (portam quase 1:1 — macros idênticas):

```sql
selective_trace_enabled            -- BOOL, dinâmica, GLOBAL
selective_trace_schemas            -- VARCHAR, dinâmica, GLOBAL
selective_trace_tables             -- VARCHAR (schema.tabela, wildcard *), GLOBAL
selective_trace_connections        -- VARCHAR (lista de connection_id), GLOBAL
selective_trace_output             -- ENUM('FILE','TABLE'), GLOBAL
selective_trace_log_file_path      -- VARCHAR (modo FILE), GLOBAL
selective_trace_min_duration_ms    -- INT (0 = todas), GLOBAL
selective_trace_mask_passwords     -- BOOL, GLOBAL
```

Comportamento (idêntico ao MariaDB — ver `docs/USAGE.md` do projeto origem):
- Filtros vazios ⇒ **nada é logado** (fail-safe).
- JOIN casa se **qualquer** tabela envolvida casar.
- Wildcard `*` no nome da tabela dentro de um schema.
- Filtro por **tipo de comando** via sufixo `:select`, `:dml`, `:ddl`,
  `:tcl`, `:commit`, etc. (lógica no `filter_engine`).
- Campos logados por evento: timestamp (ms), connection_id, user@host,
  schema, tabela(s), tipo de comando, texto da query, duração, status/erro.

### Modos de saída
- **FILE**: uma linha **JSON** por evento.
- **TABLE**: insere em `mysql.selective_trace_events` (criada na
  inicialização se não existir). **Guard de reentrância** obrigatório para a
  própria escrita não gerar novo evento (loop infinito).

---

## 5. O que **porta de graça** vs. o que **é reescrito**

### Porta verbatim (via submodule `core/` — seção 7)
- **`filter_engine.{h,cc}`** — parsing das listas, matching por
  schema/tabela/comando/conexão, `command_bit`, `extract_command`,
  `mask_secrets`. C++11 puro, sem headers de servidor.
- **`test_filter_logic.cc`** — os **157 testes** unitários. Devem passar
  idênticos com `g++ -std=c++17`.
- **Refatoração recomendada:** mover `json_escape_append` /
  `sql_escape_append` (hoje nos writers do MariaDB) para dentro do core, pois
  são puros e ambos os plugins usam.

### Reescrito (camada que fala com o servidor MySQL)
Novo entrypoint `src/selective_trace_mysql.cc`:
1. Descriptor `st_mysql_audit` com `interface_version = 0x0401`,
   `class_mask` = `GENERAL | TABLE_ACCESS`, callback `event_notify`.
2. `TABLE_ACCESS` → acumula tabelas + máscara de operação por statement
   (operação já vem pronta). Reusa `match_table/match_schema/match_connection`.
3. `GENERAL_STATUS` → ponto de escrita; cruza com `command_bit`.
   Classificação de comando: preferir `sql_command_id` (enum robusto) com
   `extract_command` como fallback/consistência.
4. `src/log_writer_file_mysql.cc` — writer próprio (mysys `my_open/my_write`
   ou `fopen` + mutex). **MySQL não tem logger service.**
5. `src/log_writer_table_mysql.cc` — fila + thread dedicada (conceito
   portado), INSERT via **component `mysql_command_services`** adquirido pelo
   *plugin registry* (`mysql_plugin_registry_acquire`). **Maior risco
   técnico — validar isolado cedo.** Guard de auto-log por thread.
6. Sysvars + init/deinit. Estado por conexão via `MYSQL_THDVAR_STR`-blob
   (validar semântica de init do THDVAR no MySQL — pode diferir do MariaDB).
7. **Sem** `plugin-maturity` — instalação simples, sem flag.

---

## 6. Passo a passo esperado (na ordem)

### Etapa 0 — Pesquisa (não pule)
1. Ler por completo, dos fontes do MySQL 8.0:
   - `include/mysql/plugin_audit.h` (confirmar structs desta seção).
   - `include/mysql/plugin.h` (sysvars/thdvars).
   - `include/mysql/components/services/mysql_command_services.h` e
     `include/mysql/service_command.h` (SQL interno do modo TABLE).
   - `plugin/audit_null/audit_null.cc` (exemplo mínimo no MySQL).
2. Confirmar semântica exata de `MYSQL_THDVAR_STR` no MySQL e de
   `mysql_plugin_registry_acquire`.
3. Documentar tudo em **`docs/RESEARCH_NOTES_MYSQL.md`** — colar os trechos
   reais dos headers desta versão (versões diferentes mudam os campos).

### Etapa 1 — Ambiente + esqueleto
1. `docker/Dockerfile` com fonte do MySQL 8.0 + toolchain C++17.
2. `scripts/build.sh` (`MYSQL_ADD_PLUGIN`).
3. Esqueleto: descriptor `MYSQL_AUDIT_PLUGIN` 0x0401, init/deinit vazios,
   as 8 sysvars registradas (sem lógica).
4. Validar: `INSTALL PLUGIN` / `SHOW PLUGINS` num MySQL 8.0 oficial.

### Etapa 2 — Captura
Callbacks `GENERAL_STATUS` + `TABLE_ACCESS`, reusando o `filter_engine`.

### Etapa 3 — FILE
Writer próprio + JSON por linha. Teste: filtrar 1 schema, confirmar que só
ele aparece.

### Etapa 4 — TABLE
Criação automática da tabela + INSERT via component command services.
**Validar o mecanismo isolado antes** (é o pedaço de maior risco). Guard de
auto-log.

### Etapa 5 — Validação
Bateria completa num **MySQL 8.0 oficial**: funcional (schema/tabela/comando/
conexão, FILE e TABLE), **Valgrind** (zero leaks do plugin), bateria de
segurança adversarial (injeção SQL/JSON, mascaramento de senhas). MTR se
aplicável no MySQL.

### Etapa 6 — Documentação
`README.md`, `docs/USAGE.md`, `docs/DECISIONS.md`, `docs/BENCHMARKS.md` —
espelhando o projeto MariaDB, adaptado ao MySQL.

---

## 7. Estrutura de arquivos e **código compartilhado (submodule)**

O `filter_engine` (~40% do código + os 157 testes) é **idêntico** nos dois
plugins; um bug no cérebro (ex.: o `START SLAVE` da v1.0.1 do MariaDB)
precisa valer nos dois. Estratégia: **git submodule com fonte única**.

- Repo novo **`selective-trace-core`** = `filter_engine.{h,cc}` +
  `test_filter_logic.cc` (única fonte da verdade).
- **Este repo (MySQL)** consome o core em `core/` (submodule) e adiciona só a
  camada MySQL.
- O **repo MariaDB** será refatorado (uma vez, em branch, com rebuild + 157
  testes + MTR) para consumir o mesmo core.
- Correções do filtro entram **só no core**; ambos os plugins recebem via
  `git submodule update`.

```
mysql-selective-trace-plugin/
├── CLAUDE.md                       # este briefing
├── core/                           # submodule → selective-trace-core
│   ├── filter_engine.h / .cc
│   └── test_filter_logic.cc
├── src/
│   ├── CMakeLists.txt              # MYSQL_ADD_PLUGIN
│   ├── selective_trace_mysql.cc    # entrypoint MySQL, descriptor 0x0401, sysvars
│   ├── log_writer_file_mysql.h/.cc
│   └── log_writer_table_mysql.h/.cc
├── docker/Dockerfile               # fonte MySQL 8.0 + toolchain C++17
├── scripts/build.sh
└── docs/
    ├── RESEARCH_NOTES_MYSQL.md     # Etapa 0 — headers reais colados
    ├── DECISIONS.md
    ├── BENCHMARKS.md
    └── USAGE.md
```

---

## 8. Critérios de aceite (Definition of Done)

- [ ] Compila DYNAMIC, sem warnings tratados como erro.
- [ ] `INSTALL`/`UNINSTALL PLUGIN` sem crash num MySQL 8.0 oficial.
- [ ] Filtro por schema, por tabela (cross-schema), por comando e por
      conexão funcionam com queries reais.
- [ ] Ambos os filtros vazios ⇒ nada logado.
- [ ] FILE gera JSON válido, 1 linha/evento.
- [ ] TABLE cria e popula `mysql.selective_trace_events`, sem loop de
      auto-log.
- [ ] `min_duration_ms` filtra queries rápidas; `mask_passwords` mascara.
- [ ] Zero leaks relevantes no Valgrind.
- [ ] Overhead sensivelmente menor que `general_log=ON` (documentar %).
- [ ] Os 157 testes do core passam idênticos.
- [ ] Decisões de API/design documentadas em `docs/`.

---

## 9. Como trabalhar neste projeto

- **Leia o fonte real do MySQL 8.0** antes de assumir qualquer API — não
  confie no conhecimento do MariaDB; assinaturas mudam entre os dois.
- Use o projeto MariaDB (`t:\mariadb-selective-log-plugin`) como referência
  de comportamento e como origem do `filter_engine`.
- Rode o build ao fim de cada etapa e reporte erros reais antes de seguir.
- Commits pequenos e frequentes, um por etapa.
- Pergunte ao usuário só em decisão de produto ambígua; decisão técnica de
  implementação você resolve lendo o fonte do MySQL.

---

## 10. Riscos conhecidos

1. **Modo TABLE via component command services** — mecanismo novo (8.0.24+),
   diferente do MariaDB. Maior risco. Validar isolado na Etapa 4 antes de
   integrar.
2. **Semântica de `MYSQL_THDVAR_STR`** no MySQL pode diferir do MariaDB
   (init/alloc por conexão). Validar na Etapa 0/1.
3. **Ambiente `gh`/GitHub** — a criação dos repositórios (`selective-trace-core`
   e este) e o push são passos manuais do usuário; o ambiente de dev não tem
   `gh` CLI. Preparar tudo localmente e o usuário conecta o remote.
