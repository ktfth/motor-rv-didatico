#include "core/outbox.hpp"

#include <cstring>

namespace rv::core {

bool Outbox::init(Arena& a, uint32_t slots, uint32_t payload_bytes) noexcept {
  uint32_t n = 16;
  while (n < slots) n <<= 1U;  // potência de dois: o índice vira máscara
  slots_ = a.alloc_array<OutboxEntry>(n);
  buf_ = a.alloc_array<std::byte>(payload_bytes);
  if (slots_ == nullptr || buf_ == nullptr) return false;
  mask_ = n - 1;
  buf_cap_ = payload_bytes;
  return true;
}

Status Outbox::stage(Lsn lsn, OutKind kind, ByteSpan payload) noexcept {
  if (count_ > mask_) return Status::fail(Err::ArenaExhausted, count_);
  const auto len = static_cast<uint32_t>(payload.size());
  if (len > buf_cap_) return Status::fail(Err::OutOfRange, len);

  // O buffer é circular, mas um payload NÃO é partido em dois pedaços: se não couber até o fim,
  // recomeça do zero. Perde-se um pouco de espaço no fim da volta e ganha-se que
  // `payload(entry)` seja um `span` contíguo — que é o que o publicador quer escrever de uma vez.
  //
  // E a volta precisa RESPEITAR quem ainda não foi liberado. A primeira versão deste método
  // apenas voltava para zero quando não cabia até o fim — e sobrescrevia, sem avisar, o payload
  // de entradas que ainda esperavam `durable_lsn`. O bug é invisível em teste curto (o outbox
  // esvazia antes de dar a volta) e corrompe a saída sob carga, que é o pior par de propriedades
  // possível. Agora a falta de espaço é `ArenaExhausted`, que é FATAL: perder saída em silêncio
  // seria pior que parar a partição.
  uint32_t escrita = buf_head_;
  if (count_ == 0) {
    escrita = 0;  // ninguém pendente: o buffer inteiro está livre
  } else {
    const uint32_t vivo = slots_[head_].offset;  // início do payload mais antigo ainda pendente
    if (buf_head_ >= vivo) {
      if (buf_head_ + len > buf_cap_) {
        if (len > vivo) return Status::fail(Err::ArenaExhausted, len);
        escrita = 0;
      }
    } else {
      if (buf_head_ + len > vivo) return Status::fail(Err::ArenaExhausted, len);
    }
  }
  std::memcpy(buf_ + escrita, payload.data(), len);
  buf_head_ = escrita;

  slots_[(head_ + count_) & mask_] = OutboxEntry{lsn, buf_head_, static_cast<uint16_t>(len), kind, 0};
  buf_head_ += len;
  ++count_;
  ++staged_total_;
  return kOk;
}

uint32_t Outbox::ready(Lsn durable) const noexcept {
  if (frozen_) return 0;
  uint32_t n = 0;
  while (n < count_ && slots_[(head_ + n) & mask_].lsn.v <= durable.v) ++n;
  return n;
}

void Outbox::commit(uint32_t n) noexcept {
  if (n > count_) n = count_;
  head_ = (head_ + n) & mask_;
  count_ -= n;
  released_total_ += n;
}

}  // namespace rv::core
