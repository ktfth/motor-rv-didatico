#pragma once
// As suítes de medição. Uma função por grupo; o `main` decide quais rodar.

#include <string>

#include "bench/carga.hpp"
#include "bench/harness.hpp"

namespace rv::bench {

// Primitivas de `src/base/`: CRC32C (os dois caminhos), SPSC ring, índice denso, aritmética de
// ponto fixo. É o piso do motor — nenhum número do núcleo faz sentido sem estes.
void registra_base(Runner& r);

// O núcleo: o loop da partição com a sessão completa, a latência por evento e o portão de saída.
void registra_nucleo(Runner& r, const Carga& carga);

// A imagem de recuperação (stall-and-copy): gravar, carregar, e quanto ela ocupa.
void registra_snapshot(Runner& r, const Carga& carga);

// Os backends de I/O do WAL contra o dispositivo real e o escritor completo
// (Wal/group_commit/recovery).
void registra_wal(Runner& r, const Carga& carga, const std::string& dir);

// O pipeline de ingress: decodificação de framing SBE e roteamento para os rings das partições.
void registra_ingress(Runner& r);

}  // namespace rv::bench
