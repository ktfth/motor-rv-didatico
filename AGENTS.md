# Focus Contract — Regras Operacionais para Agentes de IA

Este repositório é gerenciado pelo **Focus Contract** no desktop Omarchy Quattro.

## 🎯 Regras Inegociáveis de Trabalho em Progresso (WIP)
1. **Foco Único**: Desenvolva exclusivamente se este projeto estiver em foco (`state: focus`).
2. **Capacidade Máxima**: Exatamente 1 projeto em foco, no máximo 2 em espera (`waiting`), os demais estacionados (`parked`).
3. **Contrato de Handoff**: Ao encerrar cada iteração ou sessão, registre o handoff:
   ```bash
   focus-contract agent handoff --changed "O que mudou" --next "Próxima ação executável"
   ```
4. **Conclusão de Marco**: Ao finalizar critérios de um marco:
   ```bash
   focus-contract agent complete-milestone --completed "Marco concluído" --next-milestone "M...: ..." --next-block "..."
   ```

## 🛠️ Comandos Rápidos
- Status: `focus-contract agent status --human`
- Handoff: `./.agents/skills/focus-contract/scripts/quick-handoff.sh "O que mudou" "Próximo passo"`
