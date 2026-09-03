// Os cenários de tests/domain/golden/ rodados PONTA A PONTA pelo motor.
//
// O teste anterior (test_rounding_golden) provou que a aritmética bate. Este prova que o motor
// inteiro — internação, buckets, máquina de estados, idempotência — chega aos mesmos números
// aplicando a mesma sequência de eventos que o arquivo de cenário descreve.
//
// Cada TEST cita o arquivo de onde os números vieram. Se um número mudar aqui sem mudar lá, um
// dos dois está errado, e o arquivo é quem manda.

#include "engine_fixture.hpp"

using namespace rv;
using namespace rv::testing;
using codec::ActionType;
using codec::IncomeKind;
using codec::IncomeStage;
using codec::SettleOutcome;
using codec::Side;

namespace {
// Datas reais do calendário de pregões (data/calendario-b3-2026.csv).
constexpr uint32_t k0902 = 20260902, k0903 = 20260903, k0904 = 20260904;
constexpr uint32_t k0908 = 20260908, k0909 = 20260909, k0910 = 20260910;
constexpr uint32_t k0911 = 20260911, k0914 = 20260914, k0915 = 20260915;

void abre_dia_e_cadastra(Engine& m, uint32_t d, uint32_t d1, uint32_t d2) {
  ASSERT_TRUE(m.aplica(dia(d, d1, d2)).is_ok());
  ASSERT_TRUE(m.aplica(cadastro(kPetr4, "PETR4", 1, 3'245'000'000, d)).is_ok());
}
}  // namespace

// ---------------------------------------------------------------- golden/01
TEST(ApplyGolden01, CompraSimplesLiquidaEmD2) {
  Engine m;
  abre_dia_e_cadastra(m, k0902, k0903, k0904);

  // 100 ações a R$ 32,45 + R$ 5,20 de custos, liquidando em 04/09
  ASSERT_TRUE(m.aplica(negocio(1, kCpfA, kPetr4, Side::Buy, Qty::from_units(100).raw(),
                               3'245'000'000, 52'000, k0904)).is_ok());

  // Depois do Executado: nada em `disponivel`, tudo em `a_liquidar_compra`, preço médio AINDA ZERO
  EXPECT_EQ(m.disponivel(kCpfA, kPetr4).raw(), 0);
  EXPECT_EQ(m.preco_medio(kCpfA, kPetr4).raw(), 0) << "I4: preço médio não muda em Executado";
  EXPECT_EQ(m.i1(kCpfA, kPetr4).raw(), 0) << "compra pendente NÃO está na depositária";
  EXPECT_EQ(m.i13(kCpfA, kPetr4).raw(), Qty::from_units(100).raw()) << "mas está na projeção";
  EXPECT_EQ(m.a_liquidar(kCpfA, k0904).raw(), -32'502'000) << "−R$ 3.250,20";

  ASSERT_TRUE(m.aplica(alocacao(1, 1, kCpfA, kCpfA, kPetr4, Side::Buy,
                                Qty::from_units(100).raw(), -32'502'000, k0904)).is_ok());
  ASSERT_TRUE(m.aplica(net(1, kCpfA, -32'502'000, k0904, 1)).is_ok());

  // Avança para 04/09 e liquida
  ASSERT_TRUE(m.aplica(dia(k0904, 20260908, 20260909, k0903)).is_ok());
  ASSERT_TRUE(m.aplica(liquidacao(1, kCpfA, kPetr4, Side::Buy, Qty::from_units(100).raw(),
                                  -32'502'000, 32'502'000, k0904, SettleOutcome::Settled)).is_ok());

  EXPECT_EQ(m.disponivel(kCpfA, kPetr4).raw(), Qty::from_units(100).raw());
  EXPECT_EQ(m.preco_medio(kCpfA, kPetr4).raw(), 3'250'200'000) << "R$ 32,502 (golden/01)";
  EXPECT_EQ(m.i1(kCpfA, kPetr4).raw(), Qty::from_units(100).raw());
  EXPECT_EQ(m.caixa(kCpfA).raw(), -32'502'000) << "caixa inicial zero neste teste";
}

// ---------------------------------------------------------------- golden/03
TEST(ApplyGolden03, VendaNaoMoveOPrecoMedioEAsDuasIdentidadesValem) {
  Engine m;
  abre_dia_e_cadastra(m, k0908, k0909, k0910);

  // Posição inicial de 137 ações a 32,17072993, montada por uma compra que já liquidou.
  ASSERT_TRUE(m.aplica(negocio(10, kCpfA, kPetr4, Side::Buy, Qty::from_units(137).raw(),
                               3'217'072'993, 0, k0910)).is_ok());
  // A liquidação da montagem zera o financeiro que ela criou: senão o `a_liquidar` da data
  // carregaria o negócio de montagem junto com os do cenário, e o número conferido não seria
  // o de golden/03.
  ASSERT_TRUE(m.aplica(liquidacao(10, kCpfA, kPetr4, Side::Buy, Qty::from_units(137).raw(),
                                  -44'073'900, 44'073'900, k0910, SettleOutcome::Settled)).is_ok());
  ASSERT_EQ(m.preco_medio(kCpfA, kPetr4).raw(), 3'217'072'993);
  ASSERT_EQ(m.a_liquidar(kCpfA, k0910).raw(), 0) << "montagem liquidada";

  // Agora a tabela de golden/03, passo a passo.
  struct Passo { const char* nome; int64_t i1; int64_t i13; };
  auto confere = [&](const Passo& p) {
    EXPECT_EQ(m.i1(kCpfA, kPetr4).raw(), Qty::from_units(p.i1).raw()) << p.nome << " (I1)";
    EXPECT_EQ(m.i13(kCpfA, kPetr4).raw(), Qty::from_units(p.i13).raw()) << p.nome << " (I13)";
  };
  confere({"inicial", 137, 137});

  ASSERT_TRUE(m.aplica(negocio(11, kCpfA, kPetr4, Side::Buy, Qty::from_units(100).raw(),
                               3'000'000'000, 40'000, k0910)).is_ok());
  confere({"Executado(compra 100)", 137, 237});

  ASSERT_TRUE(m.aplica(negocio(12, kCpfA, kPetr4, Side::Sell, Qty::from_units(50).raw(),
                               3'500'000'000, 41'000, k0910)).is_ok());
  confere({"Executado(venda 50)", 137, 187});
  EXPECT_EQ(m.preco_medio(kCpfA, kPetr4).raw(), 3'217'072'993) << "I4: venda não move o preço médio";
  EXPECT_EQ(m.a_liquidar(kCpfA, k0910).raw(), -12'581'000) << "net −R$ 1.258,10 (golden/03)";

  ASSERT_TRUE(m.aplica(liquidacao(2, kCpfA, kPetr4, Side::Buy, Qty::from_units(100).raw(),
                                  -30'040'000, 30'040'000, k0910, SettleOutcome::Settled)).is_ok());
  confere({"Liquidado(compra)", 237, 187});
  EXPECT_EQ(m.preco_medio(kCpfA, kPetr4).raw(), 3'127'168'776)
      << "golden/03: base 237 (137 possuídas + 100), NÃO 187";

  ASSERT_TRUE(m.aplica(liquidacao(2, kCpfA, kPetr4, Side::Sell, Qty::from_units(50).raw(),
                                  17'459'000, 0, k0910, SettleOutcome::Settled)).is_ok());
  confere({"Liquidado(venda)", 187, 187});
  EXPECT_EQ(m.preco_medio(kCpfA, kPetr4).raw(), 3'127'168'776);
}

// ---------------------------------------------------------------- golden/04
TEST(ApplyGolden04, VendaDescobertaSemFlagEhRejeitadaESemEfeito) {
  Engine m;
  abre_dia_e_cadastra(m, k0908, k0909, k0910);
  ASSERT_TRUE(m.aplica(negocio(20, kCpfA, kPetr4, Side::Buy, Qty::from_units(137).raw(),
                               3'000'000'000, 0, k0910)).is_ok());
  ASSERT_TRUE(m.aplica(liquidacao(3, kCpfA, kPetr4, Side::Buy, Qty::from_units(137).raw(), 0,
                                  41'100'000, k0910, SettleOutcome::Settled)).is_ok());

  const uint32_t p = m.pos(kCpfA, kPetr4);
  const Qty antes_disp = m.disponivel(kCpfA, kPetr4);
  const Price antes_pm = m.preco_medio(kCpfA, kPetr4);
  const Money antes_caixa = m.caixa(kCpfA);

  // Caso A: sem a flag, vender 200 de 137 é rejeitado — e NADA se move.
  const Status st = m.aplica(negocio(21, kCpfA, kPetr4, Side::Sell, Qty::from_units(200).raw(),
                                     3'500'000'000, 0, k0910));
  EXPECT_EQ(st.code(), Err::ShortSaleNotAllowed);
  EXPECT_EQ(core::classify(st), core::ApplyClass::Rejected) << "entrada externa não derruba a partição";
  EXPECT_EQ(m.disponivel(kCpfA, kPetr4).raw(), antes_disp.raw());
  EXPECT_EQ(m.preco_medio(kCpfA, kPetr4).raw(), antes_pm.raw());
  EXPECT_EQ(m.caixa(kCpfA).raw(), antes_caixa.raw());
  EXPECT_EQ(m.estado().trades.count, 1u) << "negócio rejeitado não entra na tabela";

  // Caso B: com a flag, passa e `disponivel` fica negativo — o único bucket que I3 permite.
  ASSERT_TRUE(m.aplica(negocio(22, kCpfA, kPetr4, Side::Sell, Qty::from_units(200).raw(),
                               3'500'000'000, 0, k0910, 1U << 1)).is_ok());
  EXPECT_EQ(m.disponivel(kCpfA, kPetr4).raw(), Qty::from_units(-63).raw());
  EXPECT_EQ(m.i1(kCpfA, kPetr4).raw(), Qty::from_units(137).raw()) << "a depositária ainda tem 137";
  EXPECT_EQ(m.i13(kCpfA, kPetr4).raw(), Qty::from_units(-63).raw()) << "projeção short em 63";
  EXPECT_TRUE(m.estado().custody.buckets_non_negative(p)) << "I3 com a flag";
}

// ---------------------------------------------------------------- golden/06
TEST(ApplyGolden06, FalhaDeEntregaMantemAObrigacaoEI1) {
  Engine m;
  abre_dia_e_cadastra(m, k0908, k0909, k0910);
  ASSERT_TRUE(m.aplica(negocio(30, kCpfA, kPetr4, Side::Buy, Qty::from_units(137).raw(),
                               3'000'000'000, 0, k0910)).is_ok());
  ASSERT_TRUE(m.aplica(liquidacao(4, kCpfA, kPetr4, Side::Buy, Qty::from_units(137).raw(), 0,
                                  41'100'000, k0910, SettleOutcome::Settled)).is_ok());

  // Vende 50 para 10/09; a entrega falha.
  ASSERT_TRUE(m.aplica(negocio(31, kCpfA, kPetr4, Side::Sell, Qty::from_units(50).raw(),
                               3'500'000'000, 41'000, k0910)).is_ok());
  ASSERT_TRUE(m.aplica(alocacao(2, 31, kCpfA, kCpfA, kPetr4, Side::Sell,
                                Qty::from_units(50).raw(), 17'459'000, k0910)).is_ok());
  EXPECT_EQ(m.i1(kCpfA, kPetr4).raw(), Qty::from_units(137).raw());

  ASSERT_TRUE(m.aplica(liquidacao(5, kCpfA, kPetr4, Side::Sell, Qty::from_units(50).raw(),
                                  17'459'000, 0, k0910, SettleOutcome::DeliveryFailure)).is_ok());
  EXPECT_EQ(m.estado().custody.overdue_sell[m.pos(kCpfA, kPetr4)].raw(),
            Qty::from_units(50).raw());
  EXPECT_EQ(m.i1(kCpfA, kPetr4).raw(), Qty::from_units(137).raw())
      << "golden/06: as ações NÃO foram entregues, então continuam na depositária";
  EXPECT_GT(m.estado().exception_count, 0u) << "a falha vai para a fila de exceção";

  // Vira o dia duas vezes: a data 10/09 sai da janela e o bucket vencido NÃO se perde.
  ASSERT_TRUE(m.aplica(dia(k0910, k0911, k0914, k0909)).is_ok());
  ASSERT_TRUE(m.aplica(dia(k0911, k0914, k0915, k0910)).is_ok());
  EXPECT_EQ(m.i1(kCpfA, kPetr4).raw(), Qty::from_units(137).raw())
      << "o bucket vencido sobrevive à rotação da janela — é para isso que ele existe";

  // A recompra honra a entrega.
  ASSERT_TRUE(m.aplica(liquidacao(6, kCpfA, kPetr4, Side::Sell, Qty::from_units(50).raw(),
                                  0, 0, k0915, SettleOutcome::BoughtIn)).is_ok());
  EXPECT_EQ(m.estado().custody.overdue_sell[m.pos(kCpfA, kPetr4)].raw(), 0);
  EXPECT_EQ(m.i1(kCpfA, kPetr4).raw(), Qty::from_units(87).raw());
}

// ---------------------------------------------------------------- golden/07 e 08
TEST(ApplyGolden07, DesdobramentoUmPorDez) {
  Engine m;
  abre_dia_e_cadastra(m, k0908, k0909, k0910);
  ASSERT_TRUE(m.aplica(negocio(40, kCpfA, kPetr4, Side::Buy, Qty::from_units(137).raw(),
                               3'217'072'993, 0, k0910)).is_ok());
  ASSERT_TRUE(m.aplica(liquidacao(7, kCpfA, kPetr4, Side::Buy, Qty::from_units(137).raw(), 0,
                                  44'073'900, k0910, SettleOutcome::Settled)).is_ok());
  ASSERT_EQ(m.preco_medio(kCpfA, kPetr4).raw(), 3'217'072'993);

  ASSERT_TRUE(m.aplica(corporativo(901, kCpfA, kPetr4, ActionType::Split, 10, 1, k0911)).is_ok());
  EXPECT_EQ(m.disponivel(kCpfA, kPetr4).raw(), Qty::from_units(1370).raw());
  EXPECT_EQ(m.preco_medio(kCpfA, kPetr4).raw(), 321'707'299);
  EXPECT_EQ(m.sobras(kCpfA, kPetr4).raw(), 0) << "desdobramento não gera fração";
}

TEST(ApplyGolden08, GrupamentoDezPorUmDeixaSobraNaUnidadeCorrente) {
  Engine m;
  abre_dia_e_cadastra(m, k0908, k0909, k0910);
  ASSERT_TRUE(m.aplica(negocio(50, kCpfA, kPetr4, Side::Buy, Qty::from_units(1377).raw(),
                               321'707'299, 0, k0910)).is_ok());
  ASSERT_TRUE(m.aplica(liquidacao(8, kCpfA, kPetr4, Side::Buy, Qty::from_units(1377).raw(), 0,
                                  44'299'095, k0910, SettleOutcome::Settled)).is_ok());

  ASSERT_TRUE(m.aplica(corporativo(902, kCpfA, kPetr4, ActionType::ReverseSplit, 1, 10, k0915)).is_ok());
  EXPECT_EQ(m.disponivel(kCpfA, kPetr4).raw(), Qty::from_units(137).raw());
  EXPECT_EQ(m.sobras(kCpfA, kPetr4).raw(), 70'000'000) << "0,7 ação NOVA (golden/08)";
  EXPECT_EQ(m.i1(kCpfA, kPetr4).raw(), 13'770'000'000) << "137,7 — a fração conta em I1";

  // O leilão da sobra: 0,7 ação a R$ 314,00 = R$ 219,80 exatos.
  auto leilao = corporativo(903, kCpfA, kPetr4, ActionType::LeftoversAuction, 1, 1, k0915);
  leilao.cash_delta = 2'198'000;
  const Money antes = m.caixa(kCpfA);
  ASSERT_TRUE(m.aplica(leilao).is_ok());
  EXPECT_EQ(m.sobras(kCpfA, kPetr4).raw(), 0);
  EXPECT_EQ((m.caixa(kCpfA) - antes).raw(), 2'198'000);
}

// ---------------------------------------------------------------- golden/09
TEST(ApplyGolden09, BonificacaoComCustoAtribuido) {
  Engine m;
  abre_dia_e_cadastra(m, k0908, k0909, k0910);
  ASSERT_TRUE(m.aplica(negocio(60, kCpfA, kPetr4, Side::Buy, Qty::from_units(137).raw(),
                               3'217'072'993, 0, k0910)).is_ok());
  ASSERT_TRUE(m.aplica(liquidacao(9, kCpfA, kPetr4, Side::Buy, Qty::from_units(137).raw(), 0,
                                  44'073'900, k0910, SettleOutcome::Settled)).is_ok());

  auto b = corporativo(904, kCpfA, kPetr4, ActionType::Bonus, 105, 100, k0911);
  b.unit_cost = 120'000;  // R$ 12,00 por ação bonificada
  ASSERT_TRUE(m.aplica(b).is_ok());

  EXPECT_EQ(m.disponivel(kCpfA, kPetr4).raw(), Qty::from_units(143).raw());
  EXPECT_EQ(m.sobras(kCpfA, kPetr4).raw(), 85'000'000) << "0,85 ação (golden/09)";
  EXPECT_EQ(m.preco_medio(kCpfA, kPetr4).raw(), 3'132'440'559);
  EXPECT_EQ(m.i1(kCpfA, kPetr4).raw(), 14'385'000'000) << "143,85 = 137 × 1,05";
}

// ---------------------------------------------------------------- golden/10
TEST(ApplyGolden10, ProventoVerificadoNaoRecalculado) {
  Engine m;
  abre_dia_e_cadastra(m, k0908, k0909, k0910);
  const int64_t base = Qty::from_units(143).raw();
  // Cria a posição: o provento não a cria (ele só mexe em financeiro), e o teste confere I4 nela.
  ASSERT_TRUE(m.aplica(negocio(99, kCpfA, kPetr4, Side::Buy, base, 0, 0, k0910)).is_ok());

  // Dividendo isento: R$ 0,03714286/ação × 143 = R$ 5,31142898 → creditado R$ 5,31
  ASSERT_TRUE(m.aplica(provento(910, kCpfA, kPetr4, IncomeKind::Dividend, base, 3'714'286 / 10'000,
                                53'100, 0, k0911, IncomeStage::Accrued)).is_ok());
  EXPECT_EQ(m.proventos(kCpfA).raw(), 53'100);

  // JCP com IRRF: bruto 11,62, retido 1,74, líquido 9,88 — o motor confere a subtração.
  ASSERT_TRUE(m.aplica(provento(911, kCpfA, kPetr4, IncomeKind::Jcp, base, 8'123'456 / 10'000,
                                116'200, 17'400, k0911, IncomeStage::Accrued)).is_ok());
  EXPECT_EQ(m.proventos(kCpfA).raw(), 53'100 + 98'800);
  EXPECT_EQ(m.preco_medio(kCpfA, kPetr4).raw(), 0)
      << "I4: provento não toca o preço médio (a compra ainda não liquidou)";

  // bruto − retido ≠ líquido: rejeitado
  auto ruim = provento(912, kCpfA, kPetr4, IncomeKind::Jcp, base, 100, 10'000, 1'000, k0911,
                       IncomeStage::Accrued);
  ruim.net_amount = 8'000;  // deveria ser 9.000
  EXPECT_EQ(m.aplica(ruim).code(), Err::AmountMismatch);

  // bruto longe de posição × taxa: rejeitado e registrado na exceção
  const uint32_t exc_antes = m.estado().exception_count;
  EXPECT_EQ(m.aplica(provento(913, kCpfA, kPetr4, IncomeKind::Dividend, base, 100, 999'999, 0,
                              k0911, IncomeStage::Accrued)).code(), Err::AmountMismatch);
  EXPECT_GT(m.estado().exception_count, exc_antes);
}

// ---------------------------------------------------------------- golden/13
TEST(ApplyGolden13, IdempotenciaPorEventoEConta) {
  Engine m;
  abre_dia_e_cadastra(m, k0908, k0909, k0910);
  for (uint64_t doc : {kCpfA, kCpfB}) {
    ASSERT_TRUE(m.aplica(negocio(doc == kCpfA ? 70 : 71, doc, kPetr4, Side::Buy,
                                 Qty::from_units(137).raw(), 3'000'000'000, 0, k0910)).is_ok());
    ASSERT_TRUE(m.aplica(liquidacao(10, doc, kPetr4, Side::Buy, Qty::from_units(137).raw(), 0,
                                    41'100'000, k0910, SettleOutcome::Settled)).is_ok());
  }

  auto e = corporativo(905, kCpfA, kPetr4, ActionType::Split, 2, 1, k0911);
  ASSERT_TRUE(m.aplica(e).is_ok());
  EXPECT_EQ(m.disponivel(kCpfA, kPetr4).raw(), Qty::from_units(274).raw());

  // Duplicata: recusada, e o estado não se move.
  EXPECT_EQ(m.aplica(e).code(), Err::AlreadyApplied);
  EXPECT_EQ(m.disponivel(kCpfA, kPetr4).raw(), Qty::from_units(274).raw());

  // Mesmo evento, OUTRA conta: aplica — a chave inclui a conta.
  auto outra = e;
  outra.account = kCpfB;
  ASSERT_TRUE(m.aplica(outra).is_ok());
  EXPECT_EQ(m.disponivel(kCpfB, kPetr4).raw(), Qty::from_units(274).raw());

  // Outro evento, mesma conta e mesma data-ex: aplica — a chave é o evento, não a data.
  auto segundo = corporativo(906, kCpfA, kPetr4, ActionType::Split, 2, 1, k0911);
  ASSERT_TRUE(m.aplica(segundo).is_ok());
  EXPECT_EQ(m.disponivel(kCpfA, kPetr4).raw(), Qty::from_units(548).raw());
}

// ---------------------------------------------------------------- golden/14
TEST(ApplyGolden14, ReconciliacaoComDivergenciaMarcaMasNaoAjusta) {
  Engine m;
  abre_dia_e_cadastra(m, k0908, k0909, k0910);
  ASSERT_TRUE(m.aplica(negocio(80, kCpfA, kPetr4, Side::Buy, Qty::from_units(187).raw(),
                               3'000'000'000, 0, k0910)).is_ok());
  ASSERT_TRUE(m.aplica(liquidacao(11, kCpfA, kPetr4, Side::Buy, Qty::from_units(187).raw(), 0,
                                  56'100'000, k0910, SettleOutcome::Settled)).is_ok());
  const Qty antes = m.disponivel(kCpfA, kPetr4);

  codec::CustodyReconciled cab{};
  cab.date = k0910;
  cab.chunk_count = 1;
  cab.flags = 1;
  codec::CustodyReconciledDivergence d{};
  d.account = kCpfA;
  d.instrument = kPetr4;
  d.qty_delta = -Qty::from_units(2).raw();  // depositária tem 185, motor tem 187
  ASSERT_TRUE(m.aplica_reconciliacao(cab, &d, 1).is_ok());

  EXPECT_EQ(m.disponivel(kCpfA, kPetr4).raw(), antes.raw()) << "o ledger NÃO é ajustado";
  EXPECT_EQ(m.estado().exception_count, 1u);
  EXPECT_TRUE(m.estado().flags & core::PartitionState::kFlagReconDivergence);

  // O EOD acontece assim mesmo, e a âncora de I11 bate.
  ASSERT_TRUE(m.aplica(eod(k0910, m.estado().custody_checksum(), m.estado().cash_checksum(), 1))
                  .is_ok());
}

TEST(ApplyEodMarked, ChecksumDivergenteEhFatal) {
  Engine m;
  abre_dia_e_cadastra(m, k0908, k0909, k0910);
  ASSERT_TRUE(m.aplica(negocio(90, kCpfA, kPetr4, Side::Buy, Qty::from_units(10).raw(),
                               3'000'000'000, 0, k0910)).is_ok());

  const Status st = m.aplica(eod(k0910, 12345, 67890));
  EXPECT_EQ(st.code(), Err::StateCorrupt);
  EXPECT_EQ(core::classify(st), core::ApplyClass::Fatal)
      << "replay que não reproduz a execução original tem de PARAR, não publicar";
}

// ---------------------------------------------------------------- fronteiras
TEST(ApplyFronteira, InstrumentoNaoDescritoRejeitaNegocio) {
  Engine m;
  ASSERT_TRUE(m.aplica(dia(k0908, k0909, k0910)).is_ok());
  // sem ClosingPriceSet: o motor não conhece o fator de cotação
  const Status st = m.aplica(negocio(1, kCpfA, kPetr4, Side::Buy, Qty::from_units(1).raw(),
                                     3'000'000'000, 0, k0910));
  EXPECT_EQ(st.code(), Err::InstrumentNotDescribed);
  EXPECT_EQ(core::classify(st), core::ApplyClass::Rejected)
      << "lacuna no cadastro da B3 não pode derrubar um core inteiro";
}

TEST(ApplyFronteira, DataForaDaJanelaEhRejeitada) {
  Engine m;
  abre_dia_e_cadastra(m, k0908, k0909, k0910);
  EXPECT_EQ(m.aplica(negocio(1, kCpfA, kPetr4, Side::Buy, Qty::from_units(1).raw(),
                             3'000'000'000, 0, 20261231)).code(),
            Err::OutsideSettlementWindow);
}

TEST(ApplyFronteira, DiaNaoAndaParaTras) {
  Engine m;
  abre_dia_e_cadastra(m, k0908, k0909, k0910);
  EXPECT_EQ(m.aplica(dia(k0902, k0903, k0904)).code(), Err::InvalidTransition);
}

TEST(ApplyFronteira, TemplateDesconhecidoEhRejeitado) {
  Engine m;
  alignas(8) std::byte lixo[64]{};
  const core::EventView ev{Lsn{1}, 0, lixo, 999, 64, 0};
  core::ApplyContext ctx{&m.outbox(), &m.metricas()};
  EXPECT_EQ(core::apply(m.estado(), ev, ctx).code(), Err::UnknownTemplate);
}

TEST(ApplyFronteira, PayloadCurtoNaoLeForaDosLimites) {
  Engine m;
  alignas(8) std::byte curto[8]{};
  const core::EventView ev{Lsn{1}, 0, curto, codec::TradeExecuted::kTemplateId, 8, 0};
  core::ApplyContext ctx{&m.outbox(), &m.metricas()};
  EXPECT_EQ(core::apply(m.estado(), ev, ctx).code(), Err::ShortPayload);
}
