// Implementação de `probe_block_align`. Ver o cabeçalho para o PORQUÊ do formato do retorno.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "wal/block_align.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

// Compilar contra um cabeçalho de kernel antigo não pode ser motivo para o arquivo sumir: sem a
// constante, o `statx` devolve máscara sem o bit e o caminho de fallback assume — que é
// exatamente o que acontece na máquina de referência, com cabeçalho novo e btrfs.
#ifndef STATX_DIOALIGN
#define STATX_DIOALIGN 0x00002000U
#endif

namespace rv::wal {

const char* to_string(BlockSource s) noexcept {
  switch (s) {
    case BlockSource::Unknown: return "desconhecida";
    case BlockSource::StatxDioAlign: return "statx(STATX_DIOALIGN)";
    case BlockSource::Fallback: return "fallback 4096 (fs não informou)";
    case BlockSource::Override: return "forçado por configuração";
  }
  return "?";
}

namespace {

// O valor medido só é aceito se o formato souber gravá-lo. Um dispositivo que exigisse blocos de
// 8 KiB tornaria o cabeçalho de segmento de 4 KiB um offset desalinhado para o primeiro registro:
// é falha de compatibilidade, não de I/O, e tem de aparecer na abertura e não no primeiro commit.
[[nodiscard]] Result<BlockAlign> aceitar(uint32_t block, uint32_t mem, BlockSource src) noexcept {
  if (!is_valid_block(block)) return Status::fail(Err::IoError, block);
  // Alinhamento de memória fora do intervalo é menos grave (só encarece o buffer), mas um valor
  // absurdo indica leitura errada da struct — melhor recusar do que alocar 2 GiB alinhados.
  if (mem == 0 || (mem & (mem - 1u)) != 0u || mem > kMaxBlock) return Status::fail(Err::IoError, mem);
  return BlockAlign{block, mem, src};
}

}  // namespace

Result<BlockAlign> probe_block_align(int fd) noexcept {
  if (fd < 0) return Status::fail(Err::IoError, static_cast<uint32_t>(EBADF));

  struct statx stx {};
  // `AT_EMPTY_PATH` com caminho vazio: pergunta sobre o próprio fd. Pedir só `STATX_DIOALIGN`
  // deixa o kernel livre para não pagar por nada mais.
  const int rc = ::statx(fd, "", AT_EMPTY_PATH, STATX_DIOALIGN, &stx);
  if (rc != 0) {
    const int err = errno;
    // ENOSYS/EINVAL/EOPNOTSUPP: kernel ou fs sem a consulta. Não é erro do motor — é o caso
    // normal desta máquina. Qualquer outro errno (EBADF, EACCES) é problema real do chamador.
    if (err == ENOSYS || err == EINVAL || err == EOPNOTSUPP) {
      return aceitar(kFallbackBlock, kFallbackBlock, BlockSource::Fallback);
    }
    return Status::fail(Err::IoError, static_cast<uint32_t>(err));
  }

  // O bit pode faltar na máscara mesmo com `statx` bem-sucedido: é assim que o kernel diz "esse
  // filesystem não sabe". E, mesmo presente, o valor pode ser 0 — que significa "I/O direto não
  // suportado aqui". Os dois casos caem no mesmo lugar, com a mesma etiqueta.
  if ((stx.stx_mask & STATX_DIOALIGN) == 0 || stx.stx_dio_offset_align == 0) {
    return aceitar(kFallbackBlock, kFallbackBlock, BlockSource::Fallback);
  }

  const uint32_t off_align = stx.stx_dio_offset_align;
  const uint32_t mem_align = stx.stx_dio_mem_align != 0 ? stx.stx_dio_mem_align : off_align;
  return aceitar(off_align, mem_align, BlockSource::StatxDioAlign);
}

Result<BlockAlign> probe_block_align_path(const char* path) noexcept {
  if (path == nullptr) return Status::fail(Err::InvalidArgument);
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return Status::fail(Err::IoError, static_cast<uint32_t>(errno));
  const auto r = probe_block_align(fd);
  ::close(fd);
  return r;
}

}  // namespace rv::wal
