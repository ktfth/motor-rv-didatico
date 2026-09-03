#include "ingress/simulator.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
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
  // `n == 0` devolve 0 em vez de dividir por zero. A validação de argumentos do `main` já impede
  // o caso, mas o analisador estático provou que o caminho existe — e uma função que só é segura
  // porque quem chama se lembrou de conferir é uma armadilha esperando o segundo chamador.
  constexpr uint32_t below(uint32_t n) noexcept {
    return n == 0 ? 0U : static_cast<uint32_t>(next() % n);
  }

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

// As chaves compostas dos mapas do simulador. Documento tem 63 bits úteis, então a chave guarda o
// índice do investidor, não o documento — e a Perna carrega o documento de volta.
struct Perna {
  uint64_t documento = 0;
  uint32_t investidor = 0;
  uint32_t instrumento = 0;
  uint32_t data = 0;
  int64_t qty = 0;
  int64_t caixa = 0;
  uint8_t lado = 0;
};

// As chaves compostas usam o ÍNDICE do investidor, não um hash do documento. Um hash truncado
// colidiria, e uma colisão aqui produziria um net errado — que o motor recusaria com
// `AmountMismatch`, transformando um erro do simulador num alarme sobre o motor.
[[nodiscard]] constexpr uint64_t chave_net(uint32_t inv, uint32_t data) noexcept {
  return (static_cast<uint64_t>(inv) << 32) | data;
}
[[nodiscard]] constexpr uint64_t chave_perna(uint32_t inv, uint32_t inst, uint8_t lado,
                                             uint32_t data) noexcept {
  return (static_cast<uint64_t>(inv) << 40) | (static_cast<uint64_t>(inst) << 24) |
         (static_cast<uint64_t>(lado) << 20) | (static_cast<uint64_t>(data % (1U << 20)));
}
[[nodiscard]] constexpr uint64_t chave_posicao(uint32_t inv, uint32_t inst) noexcept {
  return (static_cast<uint64_t>(inv) << 32) | inst;
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
  uint64_t batch_id = 1;
  uint64_t action_id = 900'000;

  // O simulador precisa saber o que o motor vai calcular, porque `BatchNetted` e `TradeSettled`
  // carregam números que o motor CONFERE — mandar um net divergente produz `AmountMismatch`, que
  // é o comportamento certo do motor e um simulador inútil. Então ele mantém três mapas mínimos.
  //
  // (Isto é o simulador fazendo o papel da câmara e da depositária, que no mundo real é quem
  // calcula esses números. Nenhum deles é estado do motor.)
  std::map<uint64_t, int64_t> net_por_conta_data;   // (investidor, data) -> financeiro líquido
  std::map<uint64_t, Perna> pernas;                 // (investidor, instr, lado, data)
  std::map<uint64_t, int64_t> posicao_liquidada;    // (investidor, instr) -> quantidade liquidada

  std::vector<int64_t> preco(n_instr);
  for (uint32_t i = 0; i < n_instr; ++i) {
    preco[i] = 500'000'000 + static_cast<int64_t>(r.below(4'000'000'000u)) * 2;
  }

  for (uint32_t d = 0; d < calendario.size(); ++d) {
    const auto& hoje = calendario[d];
    const uint32_t d1 = d + 1 < calendario.size() ? calendario[d + 1].data : hoje.data;
    const uint32_t anterior = d > 0 ? calendario[d - 1].data : 0;

    // 1. Abertura do dia em todas as partições: cada core precisa da mesma janela.
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

    // 2. Cadastro + fechamento por instrumento: sem isso o motor não conhece o fator de cotação.
    for (uint32_t i = 0; i < n_instr; ++i) {
      const int64_t fechamento_anterior = preco[i];
      const auto variacao = static_cast<int64_t>(r.below(60'000'000u)) - 30'000'000;
      if (fechamento_anterior + variacao > 100'000'000) preco[i] = fechamento_anterior + variacao;
      for (uint32_t k = 0; k < cfg.particoes; ++k) {
        codec::ClosingPriceSet e{};
        e.instrument = instrumentos[i].id;
        e.date = hoje.data;
        e.price_factor = instrumentos[i].fator_cotacao;
        e.lot_size = instrumentos[i].lote;
        e.closing_price = preco[i];
        e.previous_close = fechamento_anterior;
        copia_campo_fixo(e.symbol, instrumentos[i].ticker);
        copia_campo_fixo(e.isin, instrumentos[i].isin);
        e.type = instrumentos[i].tipo;
        empurra(saida, PartitionId{static_cast<uint16_t>(k)}, e);
      }
    }

    // 3. LIQUIDAÇÃO do que vence hoje. Vem ANTES dos negócios do dia porque é assim no mundo:
    //    a janela da câmara fecha de manhã, o pregão roda depois.
    for (auto it = pernas.begin(); it != pernas.end();) {
      const Perna& perna = it->second;
      if (perna.data != hoje.data) {
        ++it;
        continue;
      }
      const bool falha = r.below(cfg.um_em_n_falhas) == 0;
      codec::TradeSettled e{};
      e.batch_id = batch_id++;
      e.account = perna.documento;
      e.qty = perna.qty;
      e.cash_amount = perna.caixa;
      e.cost_basis = perna.caixa < 0 ? -perna.caixa : 0;  // só a compra carrega custo de aquisição
      e.instrument = perna.instrumento;
      e.settlement_date = perna.data;
      e.side = perna.lado;
      e.outcome = static_cast<uint8_t>(falha ? codec::SettleOutcome::DeliveryFailure
                                             : codec::SettleOutcome::Settled);
      empurra(saida, p.of(DocumentId{perna.documento}), e);
      if (!falha) {
        posicao_liquidada[chave_posicao(perna.investidor, perna.instrumento)] +=
            perna.lado == static_cast<uint8_t>(codec::Side::Buy) ? perna.qty : -perna.qty;
      }
      it = pernas.erase(it);
    }

    // 4. Negócios do dia, com alocação direta ao investidor.
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
      // O negócio "inválido" precisa ser inequivocamente rejeitável, e não apenas provável.
      //
      // A primeira versão só omitia a flag de descoberto e supunha que o motor recusaria. Assim
      // que o simulador passou a emitir liquidação, algumas contas ganharam posição e a venda
      // passou a ser ACEITA — o net do simulador não a continha, o do motor sim, e sobravam 23
      // `AmountMismatch` por pregão. Uma quantidade que nenhuma posição cobre remove a suposição.
      e.qty = invalido && !compra
                  ? Qty::from_units(1'000'000'000).raw()
                  : Qty::from_units(static_cast<int64_t>(instrumentos[ii].lote) *
                                    (1 + r.below(20))).raw();
      e.price = preco[ii];
      e.brokerage_fee = 24'700;
      e.exchange_fee = static_cast<int64_t>(r.below(5'000));
      e.clearing_fee = static_cast<int64_t>(r.below(3'000));
      e.instrument = instrumentos[ii].id;
      e.settlement_date = hoje.liquidacao_d2;
      e.side = static_cast<uint8_t>(compra ? codec::Side::Buy : codec::Side::Sell);
      e.market = static_cast<uint8_t>(codec::Market::RoundLot);
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

      // O simulador replica a MESMA conta do motor. Se as duas divergirem, o `BatchNetted` será
      // recusado — e essa recusa é o teste mais barato de que os dois concordam.
      const Money bruto = notional_half_even(Qty::from_raw(e.qty), Price::from_raw(e.price),
                                             instrumentos[ii].fator_cotacao);
      const Money custos = Money::from_raw(e.brokerage_fee) + Money::from_raw(e.exchange_fee) +
                           Money::from_raw(e.clearing_fee);
      const int64_t financeiro = compra ? -(bruto + custos).raw() : (bruto - custos).raw();

      // O negócio inválido será REJEITADO pelo motor, logo ele não entra no `a_liquidar` de lá —
      // e não pode entrar no net daqui. A primeira versão somava antes do desvio, e o resultado
      // eram 201 `AmountMismatch` num pregão de quatro dias: o simulador acusando o motor de um
      // erro que era dele.
      if (invalido && !compra) continue;
      net_por_conta_data[chave_net(inv, e.settlement_date)] += financeiro;
      Perna& perna = pernas[chave_perna(inv, e.instrument, e.side, e.settlement_date)];
      perna.documento = doc;
      perna.investidor = inv;
      perna.instrumento = e.instrument;
      perna.lado = e.side;
      perna.data = e.settlement_date;
      perna.qty += e.qty;
      perna.caixa += financeiro;
    }

    // 5. NET DA CÂMARA por conta, para a data de liquidação de hoje.
    for (const auto& [chave, valor] : net_por_conta_data) {
      const uint32_t data = static_cast<uint32_t>(chave & 0xFFFFFFFFu);
      if (data != hoje.liquidacao_d2) continue;
      codec::BatchNetted e{};
      e.batch_id = batch_id++;
      e.account = cpf_de(static_cast<uint32_t>(chave >> 32));
      e.net_amount = valor;
      e.settlement_date = data;
      empurra(saida, p.of(DocumentId{e.account}), e);
    }

    // 6. PROVENTO num instrumento sorteado, sobre a posição JÁ LIQUIDADA. O motor confere
    //    `bruto ≈ qty × taxa` dentro de um centavo e `bruto − retido == líquido`.
    if (d > 0) {
      const uint32_t ii = r.below(n_instr);
      const int64_t taxa = 100 + static_cast<int64_t>(r.below(900));  // R$ 0,0100 a R$ 0,1000
      const bool jcp = r.below(2) == 0;
      for (const auto& [chave, qty] : posicao_liquidada) {
        if (qty <= 0) continue;
        const uint32_t instrumento = static_cast<uint32_t>(chave & 0xFFFFFFFFu);
        if (instrumento != instrumentos[ii].id) continue;
        const int64_t bruto = mul_div<Rounding::HalfEven>(qty, taxa, Qty::kOne);
        const int64_t retido = jcp ? withhold_trunc(Money::from_raw(bruto), 1500).raw() : 0;
        codec::DividendPaid e{};
        e.action_id = action_id++;
        e.account = cpf_de(static_cast<uint32_t>(chave >> 32));
        e.qty_basis = qty;
        e.rate_per_share = taxa;
        e.gross_amount = bruto;
        e.withheld_tax = retido;
        e.net_amount = bruto - retido;
        e.instrument = instrumento;
        e.ex_date = hoje.data;
        e.payment_date = hoje.data;
        e.kind = static_cast<uint8_t>(jcp ? codec::IncomeKind::Jcp : codec::IncomeKind::Dividend);
        e.stage = static_cast<uint8_t>(codec::IncomeStage::Accrued);
        empurra(saida, p.of(DocumentId{e.account}), e);
      }
    }

    // 7. Evento corporativo esparso.
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

    // 8. RECONCILIAÇÃO com a depositária, sem divergência — e uma com divergência no último dia,
    //    para que o caminho da flag do snapshot também seja exercitado (cenário golden 14).
    for (uint32_t k = 0; k < cfg.particoes; ++k) {
      EventoRoteado ev{};
      ev.particao = PartitionId{static_cast<uint16_t>(k)};
      ev.tmpl = codec::CustodyReconciled::kTemplateId;
      codec::CustodyReconciled cab{};
      cab.date = hoje.data;
      cab.chunk_count = 1;
      cab.flags = 1;
      const bool com_divergencia = (d + 1 == calendario.size()) && k == 0;
      const uint16_t n = com_divergencia ? 1 : 0;
      codec::GroupHeader gh{static_cast<uint16_t>(sizeof(codec::CustodyReconciledDivergence)), n, 0};
      std::memcpy(ev.bytes, &cab, sizeof cab);
      std::memcpy(ev.bytes + sizeof cab, &gh, sizeof gh);
      uint16_t len = sizeof cab + sizeof gh;
      if (com_divergencia) {
        codec::CustodyReconciledDivergence div{};
        div.account = cpf_de(0);
        div.instrument = instrumentos[0].id;
        div.qty_delta = -Qty::from_units(2).raw();
        std::memcpy(ev.bytes + len, &div, sizeof div);
        len = static_cast<uint16_t>(len + sizeof div);
      }
      ev.len = len;
      saida.push_back(ev);
    }

    // 9. A marca do fim do dia. Os checksums ficam em zero: quem os conhece é o motor, e o
    //    simulador não inventa número que o motor calcula.
    for (uint32_t k = 0; k < cfg.particoes; ++k) {
      codec::EodMarked e{};
      e.date = hoje.data;
      empurra(saida, PartitionId{static_cast<uint16_t>(k)}, e);
    }
  }
}

}  // namespace rv::ingress
