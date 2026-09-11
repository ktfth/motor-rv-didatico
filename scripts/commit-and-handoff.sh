#!/usr/bin/env bash
# ==============================================================================
# commit-and-handoff.sh — Commit Atômico com Sincronização no Focus Contract
#
# Uso:
#   ./scripts/commit-and-handoff.sh "tipo(escopo): mensagem" "Próximo passo a ser executado"
# ==============================================================================

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

if [[ $# -lt 2 ]]; then
    echo "Uso: $0 \"<mensagem_do_commit>\" \"<proximo_passo>\" [bloqueador]"
    exit 1
fi

MSG="$1"
NEXT="$2"
BLOCKER="${3:-Nenhum}"

# 1. Executar o commit (acionará o pre-commit hook automaticamente)
git commit -m "$MSG"

# 2. Registrar no Focus Contract se a ferramenta estiver presente
if command -v focus-contract >/dev/null 2>&1; then
    echo ""
    echo "==> Sincronizando com o Focus Contract..."
    focus-contract agent handoff --changed "$MSG" --next "$NEXT" --blocker "$BLOCKER"
fi
