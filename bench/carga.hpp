#pragma once
// A carga de medição: a MESMA sessão determinística que o `motor-rv-sim` roda.
//
// Nada de "gerar eventos aleatórios rápido". O que se mede tem de ser o que o motor faz num
// pregão: a mistura de dez templates na ordem que o contrato de ordenação exige, com rejeições
// dentro (um em cada cinquenta negócios é venda descoberta não autorizada) e falhas de entrega
// (uma em duzentas liquidações). Um benchmark que só aplica `TradeExecuted` válido mede o ramo
// mais barato do `apply` e chama isso de vazão do motor.
//
// A carga é gerada com `particoes = 1`: a métrica obrigatória é eventos por segundo POR CORE, e
// dividir a sessão em quatro para depois medir um quarto dela mediria o particionador junto.

#include <cstdint>
#include <string>
#include <vector>

#include "bench/harness.hpp"
#include "ingress/simulator.hpp"

namespace rv::bench {

struct Carga {
  std::vector<ingress::EventoRoteado> eventos;
  ingress::ConfigSimulacao cfg;
  std::string erro;
  bool ok = false;

  [[nodiscard]] size_t tamanho() const noexcept { return eventos.size(); }

  // O que vai gravado no JSON, junto dos números. Ver DescricaoCarga em bench/harness.hpp.
  [[nodiscard]] DescricaoCarga descricao() const noexcept {
    DescricaoCarga d;
    d.dias = cfg.dias;
    d.negocios_por_dia = cfg.negocios_por_dia;
    d.investidores = cfg.investidores;
    d.particoes = cfg.particoes;
    d.semente = cfg.semente;
    d.eventos = eventos.size();
    return d;
  }
};

// `dados_dir` aponta para `data/` (instrumentos e calendário reais da B3). O simulador NÃO
// inventa calendário — ver src/ingress/simulator.hpp.
[[nodiscard]] Carga gera_carga(const std::string& dados_dir, uint32_t dias, uint32_t negocios,
                               uint32_t investidores, uint64_t semente, uint32_t data_inicial);

}  // namespace rv::bench
