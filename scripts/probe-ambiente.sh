#!/usr/bin/env bash
# Redescobre os fatos de plataforma de docs/ambiente.md nesta máquina.
#
# Rode antes de confiar em qualquer número de bench/baseline.json vindo de outro lugar. As três
# perguntas que decidem o desenho do WAL: io_uring aceita SINGLE_ISSUER|DEFER_TASKRUN? o filesystem
# aceita O_DIRECT|O_DSYNC? o kernel reporta o alinhamento de I/O direto via statx?
set -uo pipefail
raiz="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
alvo="${1:-$raiz}"   # diretório onde o WAL viveria; o filesystem dele é o que importa

echo "== máquina =="
printf "cpu            : %s\n" "$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
printf "threads        : %s\n" "$(nproc)"
printf "kernel         : %s\n" "$(uname -r)"
printf "filesystem     : %s (%s)\n" "$(stat -f -c '%T' "$alvo")" "$alvo"
for f in sse4_2 pclmulqdq avx2 bmi2; do
  printf "%-15s: %s\n" "$f" "$(grep -qm1 " $f " /proc/cpuinfo && echo sim || echo NAO)"
done
printf "nivel x86-64   : %s\n" "$(grep -qm1 ' avx2 ' /proc/cpuinfo && echo v3+ || { grep -qm1 ' sse4_2 ' /proc/cpuinfo && echo v2 || echo v1; })"

echo
echo "== bibliotecas =="
for m in liburing openssl fmt; do printf "%-15s: %s\n" "$m" "$(pkg-config --modversion "$m" 2>/dev/null || echo AUSENTE)"; done
printf "%-15s: %s\n" "gtest" "$(pkg-config --modversion gtest 2>/dev/null || echo AUSENTE)"
printf "%-15s: %s\n" "cmake" "$([ -x "$raiz/.toolchain/bin/cmake" ] && "$raiz/.toolchain/bin/cmake" --version | head -1 || command -v cmake || echo 'AUSENTE — rode scripts/bootstrap-toolchain.sh')"

echo
echo "== sondas de I/O =="
cat > "$tmp/probe.c" <<'C'
#define _GNU_SOURCE
#include <liburing.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <linux/stat.h>
int main(int argc, char** argv) {
  const char* dir = argc > 1 ? argv[1] : ".";
  char caminho[4096]; snprintf(caminho, sizeof caminho, "%s/.probe-wal.dat", dir);
  struct io_uring ring; struct io_uring_params p; memset(&p, 0, sizeof p);
  p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
  int r = io_uring_queue_init_params(64, &ring, &p);
  printf("%-34s: %s\n", "io_uring SINGLE_ISSUER|DEFER", r == 0 ? "sim" : strerror(-r));
  if (r) { memset(&p, 0, sizeof p); r = io_uring_queue_init_params(64, &ring, &p);
           printf("%-34s: %s\n", "io_uring simples", r == 0 ? "sim" : strerror(-r));
           if (r) return 1; }
  int fd = open(caminho, O_CREAT | O_RDWR | O_DIRECT | O_DSYNC, 0644);
  printf("%-34s: %s\n", "open(O_DIRECT|O_DSYNC)", fd < 0 ? strerror(errno) : "sim");
  if (fd < 0) return 1;
  struct statx stx; memset(&stx, 0, sizeof stx);
  int sr = statx(fd, "", AT_EMPTY_PATH, STATX_DIOALIGN, &stx);
  int tem = sr == 0 && (stx.stx_mask & STATX_DIOALIGN);
  printf("%-34s: %s\n", "statx(STATX_DIOALIGN)", tem ? "sim" : "NAO — usar fallback 4096");
  if (tem) printf("%-34s: mem=%u offset=%u\n", "  alinhamento reportado", stx.stx_dio_mem_align, stx.stx_dio_offset_align);
  unsigned bloco = tem && stx.stx_dio_offset_align ? stx.stx_dio_offset_align : 4096;
  void* buf = NULL;
  if (posix_memalign(&buf, bloco, bloco)) return 1;
  memset(buf, 0xAB, bloco);
  struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
  io_uring_prep_write(sqe, fd, buf, bloco, 0);
  io_uring_submit(&ring);
  struct io_uring_cqe* cqe; io_uring_wait_cqe(&ring, &cqe);
  printf("%-34s: res=%d (esperado %u)\n", "write O_DIRECT via io_uring", cqe->res, bloco);
  io_uring_cqe_seen(&ring, cqe);
  close(fd); unlink(caminho); free(buf);
  return 0;
}
C
if cc -O1 -o "$tmp/probe" "$tmp/probe.c" -luring 2>"$tmp/erro"; then
  "$tmp/probe" "$alvo"
else
  echo "não compilou a sonda (liburing ausente?):"; sed 's/^/  /' "$tmp/erro" | head -5
fi
