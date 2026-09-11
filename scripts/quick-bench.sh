#!/usr/bin/env bash
# ==============================================================================
# quick-bench.sh — Execução Rápida de Sanidade de Desempenho
#
# Compila e roda o harness em modo rápido (--rapido).
# Opcionalmente compara com baseline oficial se passado --comparar.
# ==============================================================================

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

if [[ -d "$ROOT_DIR/.toolchain/bin" ]]; then
    export PATH="$ROOT_DIR/.toolchain/bin:$PATH"
fi

COMPARE=0
if [[ "${1:-}" == "--comparar" || "${1:-}" == "-c" ]]; then
    COMPARE=1
fi

echo "=================================================================="
echo "  motor-rv: Quick Benchmark (Modo de Sanidade Rápida)"
echo "=================================================================="

# 1. Garantir que o binário release esteja atualizado
echo "==> Verificando compilação do preset release..."
cmake --build build/release --target motor-rv-bench

# 2. Executar
BENCH_BIN="./build/release/bench/motor-rv-bench"
if [[ "$COMPARE" -eq 1 ]]; then
    echo "==> Executando com comparação contra bench/baseline.json..."
    "$BENCH_BIN" --rapido --comparar bench/baseline.json
else
    echo "==> Executando medição rápida..."
    "$BENCH_BIN" --rapido
fi

echo "=================================================================="
echo "✓ Benchmark de sanidade concluído."
echo "=================================================================="
