#!/usr/bin/env bash
CHANGED="${1:-Sessão concluída e código validado.}"
NEXT="${2:-Continuar implementação do marco atual.}"
BLOCKER="${3:-Nenhum}"
focus-contract agent handoff --changed "$CHANGED" --next "$NEXT" --blocker "$BLOCKER"
