#!/usr/bin/env bash
# Configura o Git para utilizar o diretório versionado scripts/hooks/ como hooks ativos.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

chmod +x scripts/hooks/*
git config core.hooksPath scripts/hooks

echo "✓ Git hooks configurados com sucesso para usar scripts/hooks/."
echo "  - pre-commit  : clang-format e check_invariants.py nos arquivos staged"
echo "  - post-commit : lembrete amigável de Focus Contract"
