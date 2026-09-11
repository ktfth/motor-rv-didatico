#!/usr/bin/env bash
# ==============================================================================
# gate-local.sh — Verificação de Pré-Voo Local do motor-rv
#
# Executa localmente os mesmos gates de conformidade, determinismo, sanitizers
# e invariantes exigidos pelo CI (.github/workflows/ci.yml).
#
# Modos:
#   ./scripts/gate-local.sh          # Modo rápido (~15s): format, debug build, testes, invariantes, determinismo
#   ./scripts/gate-local.sh --full   # Modo completo: quick + release (LTO), asan, tsan, SBE, escala de partição
# ==============================================================================

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

if [[ -d "$ROOT_DIR/.toolchain/bin" ]]; then
    export PATH="$ROOT_DIR/.toolchain/bin:$PATH"
fi

MODE="quick"
if [[ "${1:-}" == "--full" ]]; then
    MODE="full"
elif [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    echo "Uso: $0 [--quick | --full]"
    echo "  --quick (padrão): clang-format, build debug, ctest debug, check_invariants, determinismo básico"
    echo "  --full          : tudo do --quick mais release, asan, tsan, gates de simulação e schemas"
    exit 0
fi

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

pass() { echo -e "  [${GREEN}OK${NC}] $1"; }
info() { echo -e "${BLUE}==>${NC} $1"; }
fail() { echo -e "  [${RED}FALHA${NC}] $1"; exit 1; }

echo "=================================================================="
echo "  motor-rv: Pré-Voo Local (Modo: ${MODE})"
echo "=================================================================="

# 1. Formatação de código
info "1. Verificando clang-format..."
if command -v clang-format >/dev/null 2>&1; then
    FORMAT_FILES=()
    if [[ "${CHECK_ALL_FORMAT:-0}" == "1" ]]; then
        while IFS= read -r f; do FORMAT_FILES+=("$f"); done < <(git ls-files '*.cpp' '*.hpp')
    else
        while IFS= read -r f; do
            [[ -n "$f" && -f "$f" && ( "$f" == *.cpp || "$f" == *.hpp ) ]] && FORMAT_FILES+=("$f")
        done < <(git status --porcelain | awk '{print $NF}')
        # Se não há arquivos modificados no momento, confere os modificados no último commit
        if [[ ${#FORMAT_FILES[@]} -eq 0 ]]; then
            while IFS= read -r f; do
                [[ -n "$f" && -f "$f" && ( "$f" == *.cpp || "$f" == *.hpp ) ]] && FORMAT_FILES+=("$f")
            done < <(git diff --name-only HEAD~1 HEAD 2>/dev/null || true)
        fi
    fi

    FORMAT_ERRORS=0
    for f in "${FORMAT_FILES[@]}"; do
        if ! clang-format --dry-run --Werror "$f" >/dev/null 2>&1; then
            echo -e "    ${RED}Arquivo não formatado:${NC} $f"
            FORMAT_ERRORS=$((FORMAT_ERRORS + 1))
        fi
    done

    if [[ "$FORMAT_ERRORS" -gt 0 ]]; then
        fail "Encontrados $FORMAT_ERRORS arquivos desalinhados com o .clang-format. Use: clang-format -i <arquivos>"
    fi
    if [[ ${#FORMAT_FILES[@]} -gt 0 ]]; then
        pass "Arquivos avaliados (${#FORMAT_FILES[@]}) estão em conformidade com o clang-format"
    else
        pass "Nenhum arquivo C++ modificado para conferir"
    fi
else
    echo -e "  [${YELLOW}PULADO${NC}] clang-format não encontrado no PATH"
fi

# 2. Configurar e Compilar Debug
info "2. Compilando preset debug..."
cmake --preset debug >/dev/null 2>&1 || cmake -B build/debug --preset debug
cmake --build build/debug
pass "Compilação debug concluída sem erros (-Werror ativo)"

# 3. Testes unitários e de integração em Debug
info "3. Executando testes unitários e de caos (preset debug)..."
ctest --test-dir build/debug --output-on-failure
pass "Todos os testes em debug passaram"

# 4. Invariantes
info "4. Conferindo cobertura de invariantes..."
python3 scripts/check_invariants.py build/debug
pass "Todos os invariantes I1..I13 possuem testes verificados"

# 5. Determinismo básico do simulador
info "5. Verificando determinismo estrito do motor-rv-sim..."
TMP_DIR=$(mktemp -d /tmp/motor-rv-gate-XXXXXX)
trap 'rm -rf "$TMP_DIR"' EXIT

./build/debug/src/app/motor-rv-sim --dias 2 --negocios 2000 --investidores 300 > "$TMP_DIR/sim_a.txt"
./build/debug/src/app/motor-rv-sim --dias 2 --negocios 2000 --investidores 300 > "$TMP_DIR/sim_b.txt"

if ! diff -u "$TMP_DIR/sim_a.txt" "$TMP_DIR/sim_b.txt" >/dev/null 2>&1; then
    fail "Simulação produziu resultados divergentes com a mesma semente!"
fi

if ! grep -q "I3  (bucket não negativo)  : ok" "$TMP_DIR/sim_a.txt"; then
    fail "Invariante I3 violado na execução do simulador!"
fi

if grep -q "NAO BATE COM O TOTAL" "$TMP_DIR/sim_a.txt"; then
    fail "Contagem de rejeições do simulador está inconsistente!"
fi
pass "Determinismo estrito e contabilidade de rejeições confirmados"

if [[ "$MODE" == "quick" ]]; then
    echo "=================================================================="
    echo -e "${GREEN}✓ Pré-voo rápido aprovado com sucesso!${NC}"
    echo "=================================================================="
    exit 0
fi

# ----------------------------------------------------------------------
# MODO FULL (Equivalente completo ao CI)
# ----------------------------------------------------------------------
info "6. Verificando geração de calendário B3..."
python3 scripts/gera-calendario.py 2026 "$TMP_DIR/cal.csv"
if ! diff -q "$TMP_DIR/cal.csv" data/calendario-b3-2026.csv >/dev/null 2>&1; then
    fail "data/calendario-b3-2026.csv difere do que o gerador produz!"
fi
pass "Calendário B3 versionado confere com gerador oficial"

info "7. Verificando gerador SBE (positivo e rejeição de double)..."
python3 scripts/sbe_gen.py schema/events.xml --check
printf '%s\n' '<?xml version="1.0"?>' \
  '<sbe:messageSchema xmlns:sbe="http://fixprotocol.io/2016/sbe" package="p" id="1" version="1">' \
  '<types/><sbe:message name="A" id="1"><field name="x" id="1" type="double"/></sbe:message>' \
  '</sbe:messageSchema>' > "$TMP_DIR/ruim.xml"

if python3 scripts/sbe_gen.py "$TMP_DIR/ruim.xml" --check 2>/dev/null; then
    fail "Gerador SBE aceitou tipo float/double proibido pelas regras!"
fi
pass "Gerador SBE validado e blindado contra tipos proibidos"

info "8. Compilando e testando preset release..."
cmake --preset release >/dev/null 2>&1 || cmake -B build/release --preset release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
pass "Preset release aprovado com sucesso"

info "9. Compilando e testando preset asan (Address & Undefined Sanitizers)..."
if cmake --preset asan >/dev/null 2>&1; then
    cmake --build build/asan
    ctest --test-dir build/asan --output-on-failure
    pass "Preset asan aprovado sem vazamentos ou UB"
else
    echo -e "  [${YELLOW}PULADO${NC}] Preset asan não pôde ser configurado"
fi

info "10. Compilando e testando preset tsan (Thread Sanitizer)..."
if cmake --preset tsan >/dev/null 2>&1; then
    cmake --build build/tsan
    ctest --preset tsan-concorrencia --output-on-failure
    pass "Preset tsan aprovado sem race conditions"
else
    echo -e "  [${YELLOW}PULADO${NC}] Preset tsan não pôde ser configurado"
fi

info "11. Verificando invariância com número de partições..."
for n in 1 2 4 8; do
    ./build/debug/src/app/motor-rv-sim --dias 2 --negocios 2000 --investidores 300 --particoes $n > "$TMP_DIR/p$n.txt"
    printf '%s %s\n' "$(grep '^  TOTAL' "$TMP_DIR/p$n.txt" | sed 's/.*custódia=//')" \
                      "$(grep -E '^tot' "$TMP_DIR/p$n.txt" | awk '{print $3}')"
done > "$TMP_DIR/part.txt"
if [[ "$(sort -u "$TMP_DIR/part.txt" | wc -l)" -ne 1 ]]; then
    fail "O estado agregado contábil mudou com a variação do número de partições!"
fi
pass "Estado agregado independente da contagem de partições (1, 2, 4, 8)"

info "12. Verificando que a imagem de recuperação escala com os dados..."
pequeno=$(./build/debug/src/app/motor-rv-sim --dias 1 --negocios 10 --investidores 5 | awk '/partição 0:/{print $3}')
grande=$(./build/debug/src/app/motor-rv-sim --dias 1 --negocios 20000 --investidores 2000 | awk '/partição 0:/{print $3}')
awk -v p="$pequeno" -v g="$grande" 'BEGIN{
  if (p > 5.0)     { print "imagem de 10 negócios = " p " MiB: não escala com o dado"; exit 1 }
  if (g <= p * 5)  { print "a imagem não cresce com o volume"; exit 1 }
}' || fail "Verificação de escala da imagem de recuperação falhou!"
pass "Imagem de recuperação escala com dados (10 negócios: ${pequeno} MiB, 20000 negócios: ${grande} MiB)"

info "13. Verificando estabilização da tabela de negócios (liquidação D+2)..."
for d in 4 16; do
    ./build/debug/src/app/motor-rv-sim --dias $d --negocios 3000 --investidores 300 | awk -v d=$d '/^0 /{print d, $7}'
done > "$TMP_DIR/tab.txt"
a=$(awk 'NR==1{print $2}' "$TMP_DIR/tab.txt")
b=$(awk 'NR==2{print $2}' "$TMP_DIR/tab.txt")
if [[ "$b" -ge $((a * 2)) ]]; then
    fail "A tabela de negócios não estabiliza e cresce indefinidamente com os dias!"
fi
pass "Tabela de negócios estabiliza com o tempo (4 dias: $a, 16 dias: $b)"

echo "=================================================================="
echo -e "${GREEN}✓ Pré-voo COMPLETO aprovado com sucesso! Pronto para PR / CI.${NC}"
echo "=================================================================="
