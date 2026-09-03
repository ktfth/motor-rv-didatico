#!/usr/bin/env python3
"""Cruza docs/invariantes.md com os rótulos do CTest — e falha se algum invariante ficar sem teste.

"Invariante sem teste não existe" (docs/invariantes.md) é uma regra que só vale se alguém a
verificar. Este script é esse alguém: ele pergunta ao CTest quais testes carregam o rótulo `Ix`,
preenche a coluna "Teste" da tabela e sai com código diferente de zero se alguma linha ficar
vazia. Rodado no CI, transforma a frase do documento em gate de merge.

Uso:
    scripts/check_invariants.py build/debug            # confere e falha se faltar
    scripts/check_invariants.py build/debug --escreve  # confere e ATUALIZA a tabela
"""

import argparse
import pathlib
import re
import subprocess
import sys


def acha_ctest() -> str:
    """O ctest do sistema, ou o local do projeto. A máquina de referência só tem o local."""
    import shutil
    if (c := shutil.which("ctest")):
        return c
    local = pathlib.Path(__file__).resolve().parent.parent / ".toolchain" / "bin" / "ctest"
    if local.exists():
        return str(local)
    print("check_invariants: ctest não encontrado. Rode scripts/bootstrap-toolchain.sh.",
          file=sys.stderr)
    sys.exit(3)


CTEST = acha_ctest()


def testes_com_rotulo(build: pathlib.Path, rotulo: str) -> list[str]:
    """Nomes dos testes que carregam exatamente este rótulo."""
    r = subprocess.run([CTEST, "--test-dir", str(build), "-N", "-L", f"^{rotulo}$"],
                       capture_output=True, text=True)
    return re.findall(r"Test\s+#\d+:\s+(\S+)", r.stdout)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("build", type=pathlib.Path)
    ap.add_argument("--escreve", action="store_true", help="atualiza a coluna Teste da tabela")
    ap.add_argument("--doc", type=pathlib.Path,
                    default=pathlib.Path(__file__).resolve().parent.parent / "docs" / "invariantes.md")
    args = ap.parse_args()

    if not (args.build / "CTestTestfile.cmake").exists():
        print(f"check_invariants: {args.build} não parece um diretório de build configurado",
              file=sys.stderr)
        return 3

    linhas = args.doc.read_text().splitlines(keepends=True)
    saida, sem_teste, com_teste = [], [], []

    for linha in linhas:
        m = re.match(r"^\| (I\d+) \| (.*) \| (.*) \| (.*) \|\s*$", linha)
        if not m:
            saida.append(linha)
            continue
        ident, enunciado, verificacao, _antigo = m.groups()
        nomes = testes_com_rotulo(args.build, ident)
        if nomes:
            com_teste.append(ident)
            coluna = ", ".join(f"`{n}`" for n in nomes)
        else:
            sem_teste.append(ident)
            coluna = "—"
        saida.append(f"| {ident} | {enunciado} | {verificacao} | {coluna} |\n")

    if args.escreve:
        args.doc.write_text("".join(saida))
        print(f"check_invariants: {args.doc} atualizado")

    print(f"com teste : {len(com_teste)} — {' '.join(com_teste)}")
    if sem_teste:
        print(f"SEM TESTE : {len(sem_teste)} — {' '.join(sem_teste)}")
        print("\nInvariante sem teste não existe (docs/invariantes.md). Falhando.")
        return 1
    print("todos os invariantes têm teste vinculado.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
