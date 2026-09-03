#!/usr/bin/env bash
# Recria .toolchain/ — CMake e Ninja locais ao projeto.
#
# Por que local: a máquina de referência (docs/ambiente.md) não tem cmake nem ninja no sistema e
# não temos sudo. Um venv Python com os wheels oficiais do Kitware resolve sem tocar no sistema e
# sem pedir permissão a ninguém. O diretório está no .gitignore: é ferramenta, não fonte.
set -euo pipefail
raiz="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$raiz"

if [[ -x .toolchain/bin/cmake && -x .toolchain/bin/ninja ]]; then
  echo "toolchain já presente: $(.toolchain/bin/cmake --version | head -1)"
  exit 0
fi

echo "criando venv em $raiz/.toolchain"
python3 -m venv .toolchain
./.toolchain/bin/pip install --quiet --upgrade pip
./.toolchain/bin/pip install --quiet cmake ninja

echo "cmake : $(./.toolchain/bin/cmake --version | head -1)"
echo "ninja : $(./.toolchain/bin/ninja --version)"
cat <<'FIM'

Para usar sem prefixo, exporte o PATH nesta sessão:
    export PATH="$PWD/.toolchain/bin:$PATH"
FIM
