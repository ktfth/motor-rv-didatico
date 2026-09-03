#pragma once
// O adaptador de negócios do estudo: um simulador que emite `ExecutionReport`s.
//
// Em produção, este é o lugar do drop copy FIX 4.4 mais os arquivos de posição da depositária
// (docs/arquitetura.md). Aqui ele gera um pregão inteiro e plausível — e o faz obedecendo às
// mesmas duas regras do motor, por uma razão prática:
//
//   1. **Determinístico.** Semente fixa, LCG próprio, zero relógio. Rodar o simulador duas vezes
//      dá o mesmo log; é o que torna qualquer bug reproduzível e qualquer medição comparável.
//   2. **O calendário é DADO.** A data de liquidação de cada negócio é resolvida AQUI, contra
//      `data/calendario-b3-2026.csv`, e vai gravada no evento. O motor nunca a recalcula — se
//      recalculasse, leria arquivo durante o replay e violaria I12.
//
// O simulador emite os eventos na ordem que o contrato de ordenação exige:
//   `DayOpened` → um `ClosingPriceSet` por instrumento → negócios → alocações → nets →
//   liquidações → eventos corporativos e proventos → reconciliação → `EodMarked`.

#include <cstdint>
#include <string>
#include <vector>

#include "codec/events.hpp"
#include "core/partition.hpp"
#include "ingress/partitioner.hpp"

namespace rv::ingress {

struct Instrumento {
  uint32_t id;
  std::string ticker;
  std::string isin;
  uint8_t tipo;
  uint32_t fator_cotacao;
  uint32_t lote;
};

struct DiaDePregao {
  uint32_t data;
  uint32_t liquidacao_d2;
};

// Um evento pronto para o ring, com a partição de destino já resolvida.
struct EventoRoteado {
  PartitionId particao;
  uint16_t tmpl;
  uint16_t len;
  std::byte bytes[core::kMaxIngressPayload];
};

struct ConfigSimulacao {
  uint64_t semente = 20260902;
  uint32_t investidores = 1000;
  uint32_t negocios_por_dia = 5000;
  uint32_t dias = 3;
  uint32_t particoes = 4;
  // Um em cada N negócios é uma venda a descoberto NÃO autorizada — será rejeitado. A mistura
  // com eventos rejeitados é deliberada: um log que só contém acerto não exercita o caminho que
  // mais quebra no replay.
  uint32_t um_em_n_invalidos = 50;
  // Um em cada N liquidações falha na entrega (cenário golden 06).
  uint32_t um_em_n_falhas = 200;
};

// Lê `data/instrumentos.csv` e `data/calendario-b3-2026.csv`. Devolve false com a razão em `erro`
// se o arquivo faltar — o simulador NÃO inventa calendário: um feriado errado produziria datas de
// liquidação erradas, e o motor as aceitaria sem reclamar porque para ele a data é dado.
[[nodiscard]] bool carrega_instrumentos(const std::string& caminho, std::vector<Instrumento>& out,
                                        std::string& erro);
[[nodiscard]] bool carrega_calendario(const std::string& caminho, uint32_t data_inicial,
                                      uint32_t dias, std::vector<DiaDePregao>& out,
                                      std::string& erro);

// Gera a sessão completa. `saida` recebe os eventos na ordem de emissão, já roteados.
void gera(const ConfigSimulacao& cfg, const std::vector<Instrumento>& instrumentos,
          const std::vector<DiaDePregao>& calendario, std::vector<EventoRoteado>& saida);

}  // namespace rv::ingress
