#include "bench/carga.hpp"

namespace rv::bench {

Carga gera_carga(const std::string& dados_dir, uint32_t dias, uint32_t negocios,
                 uint32_t investidores, uint64_t semente, uint32_t data_inicial) {
  Carga c;
  c.cfg.dias = dias;
  c.cfg.negocios_por_dia = negocios;
  c.cfg.investidores = investidores;
  c.cfg.semente = semente;
  c.cfg.particoes = 1;  // eventos/s POR CORE: uma partição, um loop, um escritor

  std::vector<ingress::Instrumento> instrumentos;
  std::vector<ingress::DiaDePregao> calendario;
  if (!ingress::carrega_instrumentos(dados_dir + "/instrumentos.csv", instrumentos, c.erro)) {
    return c;
  }
  if (!ingress::carrega_calendario(dados_dir + "/calendario-b3-2026.csv", data_inicial, dias,
                                   calendario, c.erro)) {
    return c;
  }
  ingress::gera(c.cfg, instrumentos, calendario, c.eventos);
  c.ok = !c.eventos.empty();
  if (!c.ok) c.erro = "o simulador não produziu evento nenhum";
  return c;
}

}  // namespace rv::bench
