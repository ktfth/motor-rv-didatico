# motor-rv

Motor de pós-negociação de renda variável (B3) em C++23: núcleo *thread-per-core* com ledgers de
custódia e financeiro, WAL com io_uring e *group commit*, snapshot EOD e exposição Open Finance
(API Renda Variável v1.3.0) sob o perfil FAPI-BR v2.2.1.

O projeto é, ao mesmo tempo, um motor e um material de estudo. Cada decisão de engenharia está
escrita em algum lugar antes de estar no código: as fechadas em `docs/adr/`, as regras do caminho
quente em `CODING_RULES.md`, as propriedades que o motor promete em `docs/invariantes.md`, e os
números que provam cada uma em `tests/domain/golden/` — todos fechados **à mão**, antes de existir
implementação.

## Comece por aqui

| Se você quer… | Leia |
|---|---|
| entender o sistema em vinte minutos | `docs/leitura-guiada.md` |
| o contrato mínimo que todo agente/pessoa segue | `CLAUDE.md` |
| as decisões e por que cada uma foi tomada | `docs/adr/README.md` |
| o que o motor promete e como isso é verificado | `docs/invariantes.md` |
| a aritmética do domínio, com números conferidos | `tests/domain/golden/` |
| como as camadas se encaixam | `docs/contratos-internos.md` |
| o estado da máquina onde isto foi medido | `docs/ambiente.md` |

## Construir

O projeto traz o próprio CMake e Ninja (a máquina de referência não tem nenhum dos dois e não há
`sudo`):

```sh
./scripts/bootstrap-toolchain.sh
export PATH="$PWD/.toolchain/bin:$PATH"

cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```

Presets disponíveis: `debug` (asserts de invariante ligados), `release` (o baseline oficial,
`x86-64-v2` + LTO), `nativo`, `asan`, `tsan`, `fuzz` e `clang-release`. O `release` é o único que
produz números válidos para `bench/baseline.json`.

Antes de confiar em qualquer medição numa máquina nova:

```sh
./scripts/probe-ambiente.sh
```

Ele responde as três perguntas que decidem o desenho do WAL — o kernel aceita
`SINGLE_ISSUER|DEFER_TASKRUN`? o filesystem aceita `O_DIRECT|O_DSYNC`? o kernel reporta o
alinhamento de I/O direto? — e imprime o nível de ISA suportado.

## O que este repositório é e o que não é

**É** o núcleo de pós-negociação e o *resource server* de Open Finance: ingestão, ledgers,
liquidação D+2, eventos corporativos, log de eventos durável, snapshot D-1 imutável e os seis
endpoints da API Renda Variável com o pipeline FAPI-BR.

**Não é** o servidor de autorização — ele é de prateleira e certificado (ADR-0002). Também não faz
apuração de IR (módulo separado, ADR-0011) nem cobre opções, termo ou aluguel (ADR-0010).

## Idioma

Documentos, ADRs, comentários e relatórios em português; identificadores de código em inglês.
Um motor que se explica em português para quem o mantém e fala inglês para o compilador.
