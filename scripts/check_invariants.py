#!/usr/bin/env python3
"""Cruza docs/invariantes.md com os testes — e falha se algum invariante ficar sem VERIFICAÇÃO.

"Invariante sem teste não existe" (docs/invariantes.md) é uma regra que só vale se alguém a
verificar. Este script é esse alguém.

Ele faz DUAS perguntas, e a segunda foi acrescentada depois de uma revisão independente apontar
que a primeira sozinha não valia muito:

  1. Existe teste com o rótulo `Ix`?  (`ctest -N -L ^Ix$`)
  2. O CÓDIGO-FONTE desse teste menciona `Ix`?

Sem a segunda, o gate media a existência de um rótulo — e o rótulo é auto-declarado no
`CMakeLists.txt`. Foi exatamente o que aconteceu com I8 e I9: `tests/wal/test_format.cpp` carregava
os dois, verificava o layout dos registros e não afirmava nada sobre LSN. O gate reportava verde.

A segunda pergunta não prova que o teste verifica o invariante — nada prova isso automaticamente.
Ela prova que alguém escreveu o número do invariante ao lado da afirmação, que é o mínimo para a
revisão humana ter onde olhar. É uma âncora, não uma demonstração; e está escrito aqui para que
ninguém confunda as duas coisas.

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


# ---------------------------------------------------------------- fontes por teste
CHAMADA = re.compile(
    r"motor_rv_test\s*\(\s*(?P<nome>\w+)(?P<corpo>.*?)\)", re.S)
FONTES = re.compile(r"FONTES\s+(?P<lista>[^\n]*(?:\n(?!\s*(?:LIGA|ROTULOS)\b)[^\n]*)*)")


def fontes_por_teste(raiz: pathlib.Path) -> dict[str, list[pathlib.Path]]:
    """Lê os CMakeLists de tests/ e mapeia nome do teste -> arquivos de fonte."""
    mapa: dict[str, list[pathlib.Path]] = {}
    for cml in (raiz / "tests").rglob("CMakeLists.txt"):
        texto = cml.read_text()
        for m in CHAMADA.finditer(texto):
            f = FONTES.search(m.group("corpo"))
            if not f:
                continue
            arquivos = [cml.parent / w for w in f.group("lista").split() if w.endswith((".cpp", ".hpp"))]
            mapa[m.group("nome")] = [a for a in arquivos if a.exists()]
    return mapa


def menciona(arquivos: list[pathlib.Path], ident: str) -> bool:
    padrao = re.compile(rf"\b{ident}\b")
    return any(padrao.search(a.read_text()) for a in arquivos)


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

    raiz = args.doc.resolve().parent.parent
    mapa = fontes_por_teste(raiz)

    linhas = args.doc.read_text().splitlines(keepends=True)
    saida, sem_teste, com_teste, so_rotulo = [], [], [], []

    for linha in linhas:
        m = re.match(r"^\| (I\d+) \| (.*) \| (.*) \| (.*) \|\s*$", linha)
        if not m:
            saida.append(linha)
            continue
        ident, enunciado, verificacao, _antigo = m.groups()
        nomes = testes_com_rotulo(args.build, ident)
        # A segunda pergunta: entre os testes rotulados, algum MENCIONA o invariante no código?
        afirmam = [n for n in nomes if menciona(mapa.get(n, []), ident)]
        if afirmam:
            com_teste.append(ident)
            coluna = ", ".join(f"`{n}`" for n in afirmam)
        elif nomes:
            so_rotulo.append((ident, nomes))
            coluna = "— (só rótulo)"
        else:
            sem_teste.append(ident)
            coluna = "—"
        saida.append(f"| {ident} | {enunciado} | {verificacao} | {coluna} |\n")

    if args.escreve:
        args.doc.write_text("".join(saida))
        print(f"check_invariants: {args.doc} atualizado")

    print(f"com teste : {len(com_teste)} — {' '.join(com_teste)}")
    falhou = False
    if so_rotulo:
        print(f"SÓ RÓTULO : {len(so_rotulo)}")
        for ident, nomes in so_rotulo:
            print(f"  {ident}: {', '.join(nomes)} carrega(m) o rótulo, mas o código não menciona {ident}.")
        print("  Rótulo é auto-declarado no CMakeLists; ele não prova verificação nenhuma.")
        falhou = True
    if sem_teste:
        print(f"SEM TESTE : {len(sem_teste)} — {' '.join(sem_teste)}")
        falhou = True
    if falhou:
        print("\nInvariante sem teste não existe (docs/invariantes.md). Falhando.")
        return 1
    print("todos os invariantes têm teste que os menciona.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
