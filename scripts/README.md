# scripts

Dono: `toolchain`. Exceção: `relatorio-bench.py` é de `desempenho` — ele decide o veredito de
regressão do workflow de medição, e essa política é do papel que responde por desempenho.

| script | o que faz |
|---|---|
| `bootstrap-toolchain.sh` | recria `.toolchain/` (CMake e Ninja locais, via venv) |
| `probe-ambiente.sh` | sonda io_uring, `O_DIRECT` e `statx`; regenera a tabela de `docs/ambiente.md` |
| `check_invariants.py` | gate: todo invariante numerado tem teste que o menciona |
| `gera-calendario.py` | gera `data/calendario-b3-2026.csv` (o CI confere que o versionado é o gerado) |
| `sbe_gen.py` | gerador de codecs SBE a partir de `schema/*.xml` (ADR-0017) |
| `relatorio-bench.py` | transforma a saída de `motor-rv-bench` em relatório Markdown; com dois lados, dá o veredito de regressão do workflow `bench` |
