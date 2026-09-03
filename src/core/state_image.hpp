#pragma once
// A imagem de recuperação: *stall-and-copy* (ADR-0014).
//
// A ideia é mais simples do que parece, e a simplicidade vem de uma propriedade que já
// existia por outra razão: **a arena aloca em ordem determinística**. `PartitionState::init`
// pede os mesmos blocos, na mesma ordem, com os mesmos tamanhos, toda vez. Logo, o deslocamento
// de cada coluna dentro da arena é sempre o mesmo para uma dada configuração de capacidade.
//
// Então salvar é: copiar os bytes da arena, mais os poucos escalares que moram no próprio struct
// (contadores, datas, janela de liquidação). E restaurar é: rodar `init` numa arena nova — que
// atribui os MESMOS deslocamentos, logo ponteiros válidos —, copiar os bytes por cima e devolver
// os escalares. Nenhum ponteiro é serializado, nenhum é reajustado.
//
// É por isso que "só offsets, nunca ponteiros" (docs/wal.md) vale mesmo quando o formato guarda
// ponteiros na memória: o que se persiste é o CONTEÚDO da arena, e a estrutura é reconstruída por
// código determinístico. A alternativa — gravar ponteiros e rebasear na leitura — funcionaria
// também, mas transformaria toda mudança de layout numa mudança do carregador.
//
// A imagem é EXATA no LSN em que o loop foi pausado. É esse "exata" que faz I11 valer:
// estado após N eventos == estado após imagem em k + replay de k+1..N, para todo k.

#include <cstdint>

#include "base/arena.hpp"
#include "base/bytes.hpp"
#include "base/pair_index.hpp"
#include "base/status.hpp"
#include "core/partition_state.hpp"

namespace rv::core {

inline constexpr uint32_t kStateImageMagic = 0x314D5352U;  // "RSM1"
inline constexpr uint16_t kStateImageVersion = 1;

// Tudo o que NÃO está na arena. Se um campo novo entrar em `PartitionState` e não entrar aqui,
// o teste de ida e volta (`tests/core/test_state_image.cpp`) quebra — que é o ponto.
struct StateImageHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t partition_id;
  uint64_t arena_bytes;      // quantos bytes da arena seguem
  uint64_t applied_lsn;

  uint32_t business_date;
  uint32_t prev_business_date;
  uint32_t window_dates[kSettlementSlots];
  uint32_t flags;

  uint32_t custody_count;
  uint32_t cash_count;
  uint32_t instrument_count;
  uint32_t trade_count;
  uint32_t exception_count;

  uint32_t account_index_size;
  uint32_t position_index_size;
  uint32_t instrument_index_size;
  uint32_t trade_index_size;

  uint32_t actions_size0;
  uint32_t actions_size1;
  uint32_t actions_current;
  uint32_t actions_rotation_date;

  PartitionCapacity cap;      // a configuração: restaurar exige a MESMA
  uint32_t header_crc32c;
  uint32_t reserved;
};
static_assert(sizeof(StateImageHeader) % 8 == 0);

// Quantos bytes a imagem ocupa para uma arena de `arena_used` bytes.
[[nodiscard]] constexpr uint64_t state_image_bytes(uint64_t arena_used) noexcept {
  return sizeof(StateImageHeader) + arena_used;
}

// Grava a imagem em `out`. `arena_base` é a base da arena que serviu ao `init`; o tamanho vem de
// `s.arena_bytes`, a fronteira do estado — nunca de `arena.used()`, que já inclui o outbox e os
// buffers do WAL.
[[nodiscard]] Status save_state_image(const PartitionState& s, const std::byte* arena_base,
                                      MutBytes out, uint64_t* written) noexcept;

// Reconstrói `s` a partir da imagem, usando `arena` (vazia). A arena tem de comportar o mesmo
// que a original; a configuração de capacidade tem de ser idêntica. Divergência é recusa
// explícita, nunca leitura de estado meio restaurado.
[[nodiscard]] Status load_state_image(PartitionState& s, Arena& arena, ByteSpan image) noexcept;

}  // namespace rv::core
