#pragma once
// De quantos em quantos bytes este filesystem aceita I/O direto — e de onde veio a resposta.
//
// `O_DIRECT` recusa escrita cujo buffer, offset ou tamanho não sejam múltiplos do alinhamento do
// dispositivo. O kernel sabe o número e o expõe em `statx(STATX_DIOALIGN)`; docs/wal.md diz
// "nunca assumir 4 KiB". Só que na máquina de referência o btrfs NÃO devolve o bit
// (docs/ambiente.md), então assumir é inevitável — o que não é inevitável é assumir em silêncio.
//
// Daí a forma do retorno: valor E origem, juntos, sempre. A origem vai para o log de abertura do
// segmento e para `SegmentHdr::block_source`, de modo que um log gravado com 4096 assumidos seja
// distinguível de um log gravado com 4096 medidos. São bytes idênticos; a diferença só importa
// no dia em que a escrita curta aparecer, e nesse dia a pergunta "o kernel tinha respondido?" não
// pode depender da memória de alguém.

#include <cstdint>
#include <type_traits>

#include "base/status.hpp"
#include "wal/wal_format.hpp"

namespace rv::wal {

struct BlockAlign {
  uint32_t block = kFallbackBlock;      // alinhamento de offset e de tamanho da escrita
  uint32_t mem_align = kFallbackBlock;  // alinhamento exigido do buffer em memória
  BlockSource source = BlockSource::Unknown;
};

static_assert(sizeof(BlockAlign) == 12);
static_assert(std::is_trivially_copyable_v<BlockAlign>);

// Consulta o kernel sobre um arquivo JÁ ABERTO — é o único jeito honesto: o alinhamento é do
// dispositivo que hospeda aquele inode, não do caminho que alguém digitou.
//
// Nunca falha por "não suportado": ausência de resposta é `BlockSource::Fallback` com
// `kFallbackBlock`, que é o comportamento útil. Falha de verdade — `Err::IoError` — fica para fd
// inválido e para o caso em que o kernel responde um valor que o formato não comporta (não é
// potência de dois, ou maior que `kSegmentHdrBytes`): aí o motor recusa a máquina na abertura do
// segmento, em vez de gravar um log que ele mesmo não consegue reler.
[[nodiscard]] Result<BlockAlign> probe_block_align(int fd) noexcept;

// Conveniência para quem ainda não abriu o arquivo (criação de segmento, diagnóstico). Abre
// somente para leitura; o `O_DIRECT` do segmento de verdade vem depois.
[[nodiscard]] Result<BlockAlign> probe_block_align_path(const char* path) noexcept;

}  // namespace rv::wal
