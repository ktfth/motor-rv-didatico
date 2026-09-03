#include "ingress/simulator.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "base/rounding.hpp"

namespace rv::ingress {
namespace {

// O mesmo LCG do andaime de teste. Semente fixa, sem relógio: rodar duas vezes dá o mesmo log.
class Lcg {
 public:
  explicit constexpr Lcg(uint64_t s) noexcept : s_(s) {}
  constexpr uint64_t next() noexcept {
    s_ = s_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return s_ >> 16;
  }
  constexpr uint32_t below(uint32_t n) noexcept { return static_cast<uint32_t>(next() % n); }

 private:
  uint64_t s_;
};

// Copia um texto para um campo SBE de largura fixa: zera e copia o que couber.
//
// Não é `strncpy`. O campo do schema é preenchido com NUL até o fim e NÃO é terminado em NUL — um
// ticker de doze caracteres ocupa os doze bytes. `strncpy` faz exatamente isso, e é por isso que o
// GCC avisa (`stringop-truncation`): o aviso é para quem LÊ, que pode tratar o campo como string.
// Escrever a cópia explicitamente deixa a intenção no código em vez de num aviso silenciado.
template <std::size_t N>
void copia_campo_fixo(char (&destino)[N], const std::string& origem) noexcept {
  const std::size_t n = origem.size() < N ? origem.size() : N;
  std::memset(destino, 0, N);
  std::memcpy(destino, origem.data(), n);
}

template <class T>
void empurra(std::vector<EventoRoteado>& v, PartitionId p, const T& m) {
  EventoRoteado e{};
  e.particao = p;
  e.tmpl = T::kTemplateId;
  e.len = T::kBlockLength;
  std::memcpy(e.bytes, &m, T::kBlockLength);
  v.push_back(e);
}

// Um CPF plausível a partir de um índice. Não é validado por dígito verificador — o motor não
// valida documento, ele o usa como chave; validar é trabalho do cadastro, não do ledger.
[[nodiscard]] uint64_t cpf_de(uint32_t i) noexcept {
  return 10000000000ULL + static_cast<uint64_t>(i) * 137 % 89999999999ULL;
}

}  // namespace

bool carrega_instrumentos(const std::string& caminho, std::vector<Instrumento>& out,
                          std::string& erro) {
  std::ifstream f(caminho);
  if (!f) {
    erro = "não abriu " + caminho;
    return false;
  }
  std::string linha;
  while (std::getline(f, linha)) {
    if (linha.empty() || linha[0] == '#') continue;
    if (linha.rfind("instrument_id", 0) == 0) continue;
    std::istringstream ss(linha);
    std::string campo[7];
    for (auto& c : campo) std::getline(ss, c, ',');
    Instrumento i{};
    i.id = static_cast<uint32_t>(std::stoul(campo[0]));
    i.ticker = campo[1];
    i.isin = campo[2];
    i.tipo = campo[3] == "ACAO"  ? 1
             : campo[3] == "ETF" ? 2
             : campo[3] == "FII" ? 3
                                 : 4;
    i.fator_cotacao = static_cast<uint32_t>(std::stoul(campo[4]));
    i.lote = static_cast<uint32_t>(std::stoul(campo[5]));
    out.push_back(i);
  }
  if (out.empty()) {
    erro = caminho + " não tem instrumento nenhum";
    return false;
  }
  return true;
}

bool carrega_calendario(const std::string& caminho, uint32_t data_inicial, uint32_t dias,
                        std::vector<DiaDePregao>& out, std::string& erro) {
  std::ifstream f(caminho);
  if (!f) {
    erro = "não abriu " + caminho + " (rode scripts/gera-calendario.py)";
    return false;
  }
  std::string linha;
  bool comecou = false;
  while (std::getline(f, linha) && out.size() < dias) {
    if (linha.rfind("data,", 0) == 0) continue;
    std::istringstream ss(linha);
    std::string campo[7];
    for (auto& c : campo) std::getline(ss, c, ',');
    if (campo[2] != "1") continue;  // não é pregão
    const auto d = static_cast<uint32_t>(std::stoul(campo[0]));
    if (!comecou && d < data_inicial) continue;
    comecou = true;
    out.push_back(DiaDePregao{d, static_cast<uint32_t>(std::stoul(campo[3]))});
  }
  if (out.size() < dias) {
    erro = "o calendário não tem " + std::to_string(dias) + " pregões a partir de " +
           std::to_string(data_inicial);
    return false;
  }
  return true;
}

void gera(const ConfigSimulacao& cfg, const std::vector<Instrumento>& instrumentos,
          const std::vector<DiaDePregao>& calendario, std::vector<EventoRoteado>& saida) {
  Lcg r{cfg.semente};
  const Partitioner p{cfg.particoes};
  const auto n_instr = static_cast<uint32_t>(instrumentos.size());
  uint64_t trade_id = 1;
  uint64_t action_id = 900'000;

  // O preço de cada instrumento anda de dia em dia — determinístico, e é o que faz o
  // `previous_close` do evento significar alguma coisa.
  std::vector<int64_t> preco(n_instr);
  for (uint32_t i = 0; i < n_instr; ++i) {
    preco[i] = 500'000'000 + static_cast<int64_t>(r.below(4'000'000'000u)) * 2;
  }

  for (uint32_t d = 0; d < calendario.size(); ++d) {
    const auto& hoje = calendario[d];
    const uint32_t d1 = d + 1 < calendario.size() ? calendario[d + 1].data : hoje.data;
    const uint32_t anterior = d > 0 ? calendario[d - 1].data : 0;

    // 1. Abertura do dia, em TODAS as partições: cada core precisa da mesma janela.
    for (uint32_t k = 0; k < cfg.particoes; ++k) {
      codec::DayOpened e{};
      e.business_date = hoje.data;
      e.prev_business_date = anterior;
      e.settle_d1 = d1;
      e.settle_d2 = hoje.liquidacao_d2;
      e.schema_version = codec::kSchemaVersion;
      e.partition_id = static_cast<uint16_t>(k);
      empurra(saida, PartitionId{static_cast<uint16_t>(k)}, e);
    }

    // 2. Cadastro + fechamento, um por instrumento, também em todas as partições: sem isso o
    //    motor não conhece o fator de cotação e rejeita o negócio (Err::InstrumentNotDescribed).
    for (uint32_t i = 0; i < n_instr; ++i) {
      const int64_t anterior_preco = preco[i];
      const auto variacao = static_cast<int64_t>(r.below(60'000'000u)) - 30'000'000;
      preco[i] = anterior_preco + variacao > 100'000'000 ? anterior_preco + variacao
                                                         : anterior_preco;
      for (uint32_t k = 0; k < cfg.particoes; ++k) {
        codec::ClosingPriceSet e{};
        e.instrument = instrumentos[i].id;
        e.date = hoje.data;
        e.price_factor = instrumentos[i].fator_cotacao;
        e.lot_size = instrumentos[i].lote;
        e.closing_price = preco[i];
        e.previous_close = anterior_preco;
        copia_campo_fixo(e.symbol, instrumentos[i].ticker);
        copia_campo_fixo(e.isin, instrumentos[i].isin);
        e.type = instrumentos[i].tipo;
        empurra(saida, PartitionId{static_cast<uint16_t>(k)}, e);
      }
    }

    // 3. Negócios do dia. Compra e venda; a venda só é emitida com a flag de descoberto quando o
    //    simulador decidiu que é um caso inválido — assim o log carrega rejeições de verdade.
    for (uint32_t t = 0; t < cfg.negocios_por_dia; ++t) {
      const uint32_t inv = r.below(cfg.investidores);
      const uint64_t doc = cpf_de(inv);
      const uint32_t ii = r.below(n_instr);
      const bool compra = r.below(100) < 62;
      const bool invalido = r.below(cfg.um_em_n_invalidos) == 0;

      codec::TradeExecuted e{};
      e.trade_id = trade_id++;
      e.broker_note_id = 7'000'000 + trade_id / 20;
      e.account = doc;
      e.qty = Qty::from_units(static_cast<int64_t>(instrumentos[ii].lote) *
                              (1 + r.below(20))).raw();
      e.price = preco[ii];
      e.brokerage_fee = 24'700;  // R$ 2,47
      e.exchange_fee = static_cast<int64_t>(r.below(5'000));
      e.clearing_fee = static_cast<int64_t>(r.below(3'000));
      e.instrument = instrumentos[ii].id;
      e.settlement_date = hoje.liquidacao_d2;
      e.side = static_cast<uint8_t>(compra ? codec::Side::Buy : codec::Side::Sell);
      e.market = static_cast<uint8_t>(codec::Market::RoundLot);
      // Vender sem posição e sem a flag: rejeitado por I3. É de propósito.
      if (!compra && !invalido) e.flags |= 1U << 1;
      const PartitionId part = p.of(DocumentId{doc});
      empurra(saida, part, e);

      codec::TradeAllocated a{};
      a.allocation_id = e.trade_id;
      a.trade_id = e.trade_id;
      a.from_account = doc;
      a.to_account = doc;
      a.qty = e.qty;
      a.instrument = e.instrument;
      a.settlement_date = e.settlement_date;
      a.side = e.side;
      empurra(saida, part, a);
    }

    // 4. Eventos corporativos e proventos, esparsos. Um a cada dia, num instrumento sorteado,
    //    para as contas que existem — o motor rejeita o que não achar, e isso também é dado.
    if (d > 0) {
      const uint32_t ii = r.below(n_instr);
      for (uint32_t inv = 0; inv < cfg.investidores; inv += 7) {
        const uint64_t doc = cpf_de(inv);
        codec::CorporateActionApplied e{};
        e.action_id = action_id++;
        e.account = doc;
        e.instrument = instrumentos[ii].id;
        e.result_instrument = e.instrument;
        e.ex_date = hoje.data;
        e.com_date = anterior;
        e.factor_num = 2;
        e.factor_den = 1;
        e.type = static_cast<uint8_t>(codec::ActionType::Split);
        empurra(saida, p.of(DocumentId{doc}), e);
      }
    }

    // 5. Fim do dia: reconciliação sem divergência e a marca. Os checksums ficam em zero — quem
    //    os preenche é o motor, no momento do EOD, porque só ele conhece o estado. Aqui vale a
    //    regra geral: o simulador não inventa número que o motor calcula.
    for (uint32_t k = 0; k < cfg.particoes; ++k) {
      codec::EodMarked e{};
      e.date = hoje.data;
      empurra(saida, PartitionId{static_cast<uint16_t>(k)}, e);
    }
  }
}

}  // namespace rv::ingress
