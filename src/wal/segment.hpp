#pragma once
// O ciclo de vida de um segmento do log: criar, pré-zerar, ativar, rotacionar.
//
// Um segmento é um arquivo de 1 GiB (docs/wal.md) com um cabeçalho de 4 KiB e, depois dele, uma
// sequência de registros. Três coisas neste arquivo não são óbvias e é para elas que ele existe:
//
// 1. PRÉ-ZERAR DE VERDADE. `fallocate` sozinho cria um extent NÃO INICIALIZADO: o filesystem
//    anota "estes blocos existem mas ainda não têm conteúdo". A primeira escrita em cima de um
//    extent assim não é só escrita de dados — é também conversão de metadado, e o `O_DSYNC` do
//    commit passa a esperar por esse metadado. O custo aparece exatamente onde não pode aparecer:
//    na latência do primeiro commit de cada região do segmento. Por isso, depois do `fallocate`,
//    escrevemos zeros de verdade e sincronizamos UMA vez. É trabalho pago fora do caminho quente,
//    numa thread de fundo, para que o caminho quente encontre extents já convertidos.
//
// 2. NOME PELO PRIMEIRO LSN — que só se conhece na ATIVAÇÃO. Um segmento é criado dois à frente,
//    quando ainda não se sabe qual LSN cairá nele. A saída é a de sempre para "publicar algo já
//    pronto": o arquivo nasce com nome temporário, e no momento da ativação recebe o cabeçalho, é
//    LIGADO (`linkat`) ao nome definitivo e o diretório é sincronizado. `linkat` em vez de
//    `rename` porque `link` falha com EEXIST se o nome já existir — e sobrescrever um segmento
//    antigo que ainda faz parte do log seria a pior falha possível deste arquivo.
//
// 3. EPOCH ALEATÓRIO POR SEGMENTO, INJETÁVEL. O epoch é o que impede que o conteúdo de um
//    segmento reciclado passe por válido na recuperação: LSN e CRC de um registro antigo continuam
//    coerentes entre si, só o epoch é que não bate com o do cabeçalho. Ele é sorteado UMA vez, na
//    criação do segmento, e vai para o log — portanto não é não-determinismo dentro do replay
//    (I12): é um dado de entrada, gravado, e o replay apenas o confere. `EpochSource` é injetável
//    porque um teste que precisa de bytes reprodutíveis não pode depender de `getrandom`.

#include <pthread.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "base/ids.hpp"
#include "base/spsc_ring.hpp"
#include "base/status.hpp"
#include "wal/block_align.hpp"
#include "wal/wal_format.hpp"

namespace rv::wal {

// docs/wal.md: "sempre dois à frente". Dois e não um porque a criação de 1 GiB leva segundos: com
// um só, uma rotação que acontecesse durante a criação do seguinte pararia a partição.
inline constexpr uint32_t kSegmentsAhead = 2;
inline constexpr uint32_t kMaxSegmentsAhead = 3;

// Caminho do diretório copiado para dentro do gerente. Guardar o `const char*` do chamador seria
// um ponteiro pendurado esperando acontecer — a configuração é montada na pilha de quem abre.
inline constexpr size_t kMaxDirBytes = 192;
inline constexpr size_t kMaxNameBytes = 64;

// Nome definitivo: partição e primeiro LSN, em HEXADECIMAL de largura fixa. Hexadecimal porque a
// ordem lexicográfica dos nomes passa a ser a ordem numérica dos LSNs — `ls` já lista o log na
// ordem em que ele é lido — e porque é o mesmo número que aparece num `hexdump` do cabeçalho.
// Devolve o número de bytes escritos (0 se não coube).
[[nodiscard]] uint32_t segment_file_name(char* out, size_t cap, PartitionId p,
                                         uint64_t first_lsn) noexcept;

// Nome temporário do segmento ainda não ativado. Só o gerente o conhece; ele existe para que um
// segmento em preparação nunca seja confundido com um segmento do log por quem lista o diretório.
[[nodiscard]] uint32_t segment_temp_name(char* out, size_t cap, PartitionId p,
                                         uint32_t seq) noexcept;

// Fonte do epoch. Nunca devolve 0: zero é o que uma região pré-zerada contém, e um epoch 0 seria
// o único valor capaz de "casar" com lixo por acidente.
[[nodiscard]] uint32_t random_epoch() noexcept;

struct EpochSource {
  uint32_t (*fn)(void*) noexcept = nullptr;
  void* ctx = nullptr;

  [[nodiscard]] uint32_t next() const noexcept {
    const uint32_t v = (fn != nullptr) ? fn(ctx) : random_epoch();
    return v != 0 ? v : 1u;
  }
};

// Um segmento ATIVO: o arquivo já tem cabeçalho, já tem nome definitivo e aceita escrita a partir
// de `segment_data_offset()`. Dono do fd — daí não ser copiável.
class Segment {
 public:
  Segment() noexcept = default;
  ~Segment() noexcept { close(); }

  Segment(const Segment&) = delete;
  Segment& operator=(const Segment&) = delete;
  Segment(Segment&& o) noexcept { adotar(o); }
  Segment& operator=(Segment&& o) noexcept {
    if (this != &o) {
      close();
      adotar(o);
    }
    return *this;
  }

  [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
  [[nodiscard]] int fd() const noexcept { return fd_; }
  [[nodiscard]] Lsn first_lsn() const noexcept { return first_lsn_; }
  [[nodiscard]] uint32_t epoch() const noexcept { return epoch_; }
  // Capacidade TOTAL do arquivo, cabeçalho incluído: é com ela que o WAL compara o offset do
  // próximo grupo para decidir a rotação.
  [[nodiscard]] uint64_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] const char* name() const noexcept { return name_; }

  void close() noexcept;

 private:
  friend class SegmentManager;
  void adotar(Segment& o) noexcept;

  int fd_ = -1;
  Lsn first_lsn_{};
  uint32_t epoch_ = 0;
  uint64_t capacity_ = 0;
  char name_[kMaxNameBytes]{};
};

struct SegmentConfig {
  const char* dir = nullptr;  // diretório do log DESTA partição; é copiado
  PartitionId partition{};
  uint64_t segment_bytes = kSegmentBytes;
  // Tamanho do bloco de zeros. 1 MiB amortiza a syscall sem prender memória: o buffer é único e
  // vive enquanto o gerente viver.
  uint32_t zero_chunk_bytes = 1u << 20;
  uint32_t ahead = kSegmentsAhead;
  // `false` só para filesystem que recusa O_DIRECT (tmpfs de CI). NÃO é equivalente em
  // durabilidade — sem O_DSYNC, "completar a escrita" deixa de significar "estar no disco"
  // (ADR-0012). Quem liga isto assume que o teste não está medindo durabilidade.
  bool direct = true;
  BlockAlign align{};
  EpochSource epoch{};
};

// Criação em thread de fundo, ativação na thread da partição.
//
// A comunicação entre as duas é um SPSC ring de fds prontos — sem mutex, sem condvar
// (CODING_RULES §5). A thread de fundo produz; a thread da partição consome. Quando o ring está
// cheio a thread de fundo dorme; quando está vazio, `activate` devolve `WalFull`, que é falha
// TRANSITÓRIA: o loop tenta de novo, e a métrica `stalls` conta as vezes em que a criação não
// acompanhou o consumo — que é exatamente o número que decide se `ahead` precisa crescer.
class SegmentManager {
 public:
  SegmentManager() noexcept = default;
  ~SegmentManager() noexcept { stop(); }

  SegmentManager(const SegmentManager&) = delete;
  SegmentManager& operator=(const SegmentManager&) = delete;

  // Valida a configuração, prepara o PRIMEIRO segmento de forma síncrona e sobe a thread de
  // fundo. Síncrono no primeiro porque não há nada útil a fazer antes de existir um segmento: o
  // motor não pode aceitar o primeiro evento sem lugar para gravá-lo.
  [[nodiscard]] Status start(const SegmentConfig& cfg) noexcept;
  void stop() noexcept;
  [[nodiscard]] bool started() const noexcept { return running_; }

  // Transforma um segmento preparado em segmento ativo: sorteia o epoch, grava o cabeçalho de
  // 4 KiB (durável, porque o fd é O_DSYNC), liga ao nome definitivo e sincroniza o diretório.
  // `WalFull` quando não há segmento pronto — transitório, tente na próxima volta.
  [[nodiscard]] Status activate(Lsn first_lsn, uint64_t now_ns, Segment& out) noexcept;

  // Quantos segmentos prontos existem AGORA. Só a thread da partição pode chamar (o cursor de
  // leitura do ring é dela).
  [[nodiscard]] uint32_t ready() noexcept;

  struct Stats {
    uint64_t prepared = 0;        // segmentos criados e zerados
    uint64_t activated = 0;       // segmentos que viraram log
    uint64_t stalls = 0;          // ativações que não acharam segmento pronto
    uint64_t zero_bytes = 0;      // bytes de zeros efetivamente escritos
    uint64_t prepare_errors = 0;  // falhas na thread de fundo
    uint32_t last_errno = 0;
    bool fallocate_ok = true;  // false se o fs recusou fallocate (os zeros cobrem o caso)
  };
  [[nodiscard]] Stats stats() const noexcept;

 private:
  // 8 bytes, POD: é o que atravessa o ring. O nome temporário é derivável de `seq`, então não
  // precisa viajar junto.
  struct Prepared {
    int fd = -1;
    uint32_t seq = 0;
  };
  static_assert(sizeof(Prepared) == 8);

  static void* thread_main(void* self) noexcept;
  void loop() noexcept;
  [[nodiscard]] Status prepare_one() noexcept;
  void descartar(Prepared p) noexcept;

  SegmentConfig cfg_{};
  char dir_[kMaxDirBytes]{};

  SpscRing<Prepared, 4> ready_{};
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> prepared_{0};
  std::atomic<uint64_t> prepare_errors_{0};
  std::atomic<uint64_t> zero_bytes_{0};
  std::atomic<uint32_t> last_errno_{0};
  std::atomic<uint32_t> next_seq_{0};
  std::atomic<bool> fallocate_ok_{true};

  // Só a thread da partição toca nestes dois.
  uint64_t activated_ = 0;
  uint64_t stalls_ = 0;

  int dir_fd_ = -1;
  std::byte* zero_ = nullptr;  // buffer de zeros alinhado, alocado uma vez
  pthread_t thread_{};
  bool thread_up_ = false;
  bool running_ = false;
};

}  // namespace rv::wal
