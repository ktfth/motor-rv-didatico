#!/usr/bin/env python3
"""Gerador de codecs SBE do motor-rv — ADR-0017.

Lê `schema/*.xml` no dialeto do `sbe-tool` e emite cabeçalhos C++ em um diretório de saída.
Substitui o `sbe-tool` (que exige JVM, ausente na máquina de referência — `docs/ambiente.md`).

Cobre o SUBCONJUNTO do SBE que o motor usa:
  - `<types>` com `<type>`, `<enum>`, `<set>` e `<composite>` de campos primitivos;
  - `<sbe:message>` com `<field>` de tipo primitivo, enum ou set, e no máximo UM `<group>` no fim.

Tudo fora disso é **erro ruidoso**, nunca geração silenciosa de código errado. A lista de
construções recusadas está em `NAO_SUPORTADO`, com o motivo, para que a mensagem de erro ensine.

O contrato com o resto do build:
  entrada : schema/events.xml
  saída   : <out>/template_ids.hpp, events.hpp, events_decode.hpp, events_debug.cpp
  garantia: para toda mensagem, `sizeof(Msg) == blockLength` e o offset de cada campo é
            afirmado por `static_assert`. Se o gerador regredir, o build quebra antes do motor.
"""

from __future__ import annotations

import argparse
import pathlib
import sys
import xml.etree.ElementTree as ET

NS = {"sbe": "http://fixprotocol.io/2016/sbe"}

# ---------------------------------------------------------------- tipos primitivos
# (tipo C++, tamanho em bytes). A ordem de declaração dentro da mensagem é a ordem dos bytes:
# SBE é layout fixo, não é serialização com tags.
PRIMITIVOS: dict[str, tuple[str, int]] = {
    "int8": ("int8_t", 1),      "uint8": ("uint8_t", 1),
    "int16": ("int16_t", 2),    "uint16": ("uint16_t", 2),
    "int32": ("int32_t", 4),    "uint32": ("uint32_t", 4),
    "int64": ("int64_t", 8),    "uint64": ("uint64_t", 8),
    "char": ("char", 1),
}

NAO_SUPORTADO = {
    "float": "o motor não tem ponto flutuante em lugar nenhum (CODING_RULES §2)",
    "double": "o motor não tem ponto flutuante em lugar nenhum (CODING_RULES §2)",
    "varData": "dado de tamanho variável obrigaria o payload a não ter tamanho conhecido em compilação",
    "varString": "idem varData",
}


class ErroDeSchema(Exception):
    """Erro do schema, com a linha do XML quando possível. Falhar aqui é o objetivo."""


def erro(msg: str) -> None:
    raise ErroDeSchema(msg)


# ---------------------------------------------------------------- modelo
class Campo:
    def __init__(self, nome: str, tipo_c: str, tamanho: int, contagem: int, comentario: str):
        self.nome = nome
        self.tipo_c = tipo_c          # tipo C++ do elemento
        self.tamanho = tamanho        # bytes de UM elemento
        self.contagem = contagem      # 1, ou N para char[N]
        self.comentario = comentario
        self.offset = -1              # preenchido pelo cálculo de layout

    @property
    def bytes(self) -> int:
        return self.tamanho * self.contagem

    @property
    def decl(self) -> str:
        if self.contagem > 1:
            return f"{self.tipo_c} {self.nome}[{self.contagem}];"
        return f"{self.tipo_c} {self.nome};"


class Mensagem:
    def __init__(self, nome: str, template_id: int, descricao: str):
        self.nome = nome
        self.template_id = template_id
        self.descricao = descricao
        self.campos: list[Campo] = []
        self.block_declarado: "int | None" = None
        self.grupo: "Mensagem | None" = None
        self.grupo_nome = ""
        self.grupo_max = 0

    @property
    def block_length(self) -> int:
        return sum(c.bytes for c in self.campos)


class Enum:
    def __init__(self, nome: str, tipo_c: str, valores: list[tuple[str, str]], descricao: str):
        self.nome, self.tipo_c, self.valores, self.descricao = nome, tipo_c, valores, descricao


# ---------------------------------------------------------------- leitura do XML
def resolve_tipo(nome_tipo: str, tipos: dict, enums: dict[str, Enum]) -> tuple[str, int, int]:
    """Devolve (tipo C++, tamanho de um elemento, contagem)."""
    if nome_tipo in NAO_SUPORTADO:
        erro(f"tipo '{nome_tipo}' não é suportado: {NAO_SUPORTADO[nome_tipo]}")
    if nome_tipo in PRIMITIVOS:
        c, n = PRIMITIVOS[nome_tipo]
        return c, n, 1
    if nome_tipo in enums:
        e = enums[nome_tipo]
        return e.tipo_c, PRIMITIVOS[e.tipo_c_sbe][1], 1  # type: ignore[attr-defined]
    if nome_tipo in tipos:
        base, contagem = tipos[nome_tipo]
        if base not in PRIMITIVOS:
            erro(f"tipo '{nome_tipo}' deriva de '{base}', que não é primitivo suportado")
        c, n = PRIMITIVOS[base]
        return c, n, contagem
    erro(f"tipo '{nome_tipo}' não foi declarado em <types>")
    raise AssertionError  # inalcançável; só para o type checker


def ler_schema(caminho: pathlib.Path):
    raiz = ET.parse(caminho).getroot()
    if not raiz.tag.endswith("messageSchema"):
        erro(f"{caminho}: raiz esperada <sbe:messageSchema>, veio <{raiz.tag}>")

    schema_id = int(raiz.get("id", "0"))
    versao = int(raiz.get("version", "0"))
    pacote = raiz.get("package", "rv.codec")
    if raiz.get("byteOrder", "littleEndian") != "littleEndian":
        erro("só littleEndian: o formato do WAL é little-endian (docs/wal.md)")

    tipos: dict[str, tuple[str, int]] = {}
    enums: dict[str, Enum] = {}

    for bloco in raiz.findall("types"):
        for t in bloco.findall("type"):
            nome = t.get("name") or erro("<type> sem name")
            prim = t.get("primitiveType") or erro(f"<type name={nome}> sem primitiveType")
            if prim in NAO_SUPORTADO:
                erro(f"<type name={nome}>: {NAO_SUPORTADO[prim]}")
            if prim not in PRIMITIVOS:
                erro(f"<type name={nome}>: primitiveType '{prim}' desconhecido")
            tipos[nome] = (prim, int(t.get("length", "1")))

        for e in bloco.findall("enum"):
            nome = e.get("name") or erro("<enum> sem name")
            prim = e.get("encodingType", "uint8")
            if prim not in PRIMITIVOS:
                erro(f"<enum name={nome}>: encodingType '{prim}' não é primitivo suportado")
            valores = [(v.get("name", ""), (v.text or "").strip()) for v in e.findall("validValue")]
            if not valores:
                erro(f"<enum name={nome}> sem validValue")
            en = Enum(nome, PRIMITIVOS[prim][0], valores, e.get("description", ""))
            en.tipo_c_sbe = prim  # type: ignore[attr-defined]
            enums[nome] = en

        for c in bloco.findall("composite"):
            erro(f"<composite name={c.get('name')}>: composites não são usados por este motor; "
                 "declare os campos direto na mensagem para que o layout fique explícito")

    mensagens: list[Mensagem] = []
    vistos: dict[int, str] = {}
    for m in raiz.findall("sbe:message", NS) or raiz.findall("message"):
        nome = m.get("name") or erro("<message> sem name")
        tid = int(m.get("id") or erro(f"<message name={nome}> sem id"))
        if tid in vistos:
            erro(f"templateId {tid} repetido: '{nome}' e '{vistos[tid]}'. "
                 "templateId é imutável e único (contrato de determinismo D7).")
        vistos[tid] = nome
        msg = Mensagem(nome, tid, m.get("description", ""))
        msg.block_declarado = int(m.get("blockLength")) if m.get("blockLength") else None

        for f in m.findall("field"):
            fn = f.get("name") or erro(f"<field> sem name em {nome}")
            ft = f.get("type") or erro(f"<field name={fn}> sem type em {nome}")
            tc, tam, cnt = resolve_tipo(ft, tipos, enums)
            msg.campos.append(Campo(fn, tc, tam, cnt, f.get("description", "")))

        grupos = m.findall("group")
        if len(grupos) > 1:
            erro(f"<message name={nome}> tem {len(grupos)} grupos; o motor aceita no máximo um, "
                 "no fim da mensagem (docs/wal.md: o payload cabe em 64 KiB e em um slot do ring)")
        if grupos:
            g = grupos[0]
            gn = g.get("name") or erro("<group> sem name")
            sub = Mensagem(f"{nome}{gn[0].upper()}{gn[1:]}", 0, g.get("description", ""))
            for f in g.findall("field"):
                fn = f.get("name") or erro(f"<field> sem name no grupo {gn}")
                ft = f.get("type") or erro(f"<field name={fn}> sem type no grupo {gn}")
                tc, tam, cnt = resolve_tipo(ft, tipos, enums)
                sub.campos.append(Campo(fn, tc, tam, cnt, f.get("description", "")))
            sub.block_declarado = int(g.get("blockLength")) if g.get("blockLength") else None
            msg.grupo, msg.grupo_nome = sub, gn
            msg.grupo_max = int(g.get("maxOccurrences", "0")) or erro(
                f"<group name={gn}> sem maxOccurrences: sem teto, o payload não tem tamanho máximo")

        mensagens.append(msg)

    if not mensagens:
        erro(f"{caminho}: nenhuma <sbe:message>")
    return schema_id, versao, pacote, tipos, enums, mensagens


# ---------------------------------------------------------------- layout
def calcula_layout(msg: Mensagem) -> None:
    """Confere alinhamento natural e preenche os offsets.

    O motor lê o payload por reinterpretação direta (zero cópia): o struct C++ precisa ter
    exatamente o layout dos bytes. Isso exige que o schema já venha ordenado por alinhamento
    decrescente e com padding explícito. O gerador não reordena nada — ele **recusa** o schema
    mal ordenado, para que a decisão de layout fique visível no XML e não escondida aqui.
    """
    off = 0
    for c in msg.campos:
        if c.tamanho > 1 and off % c.tamanho != 0:
            erro(f"{msg.nome}.{c.nome}: offset {off} não é múltiplo de {c.tamanho}. "
                 f"Ordene os campos por alinhamento decrescente e declare o padding "
                 f"explicitamente (um campo uint8 pad[N]).")
        c.offset = off
        off += c.bytes
    if off % 8 != 0:
        erro(f"{msg.nome}: blockLength = {off} não é múltiplo de 8. "
             f"Acrescente {8 - off % 8} bytes de padding explícito ao fim.")

    # A conferência que dá ao gerador uma AUTORIDADE fora dele mesmo.
    #
    # Sem ela, `blockLength` era derivado da soma dos campos, e os `static_assert` emitidos
    # comparavam o compilador com a aritmética do próprio gerador — um erro no gerador passaria
    # despercebido porque os dois lados da comparação vinham da mesma fonte. Com o valor declarado
    # no XML, o schema manda e o gerador é o verificado.
    if msg.block_declarado is None:
        erro(f"{msg.nome}: falta blockLength no XML. Ele é a autoridade sobre o layout do fio; "
             f"pelos campos declarados, o valor é {off}.")
    if msg.block_declarado != off:
        erro(f"{msg.nome}: blockLength declarado {msg.block_declarado}, mas os campos somam {off}. "
             f"Um dos dois está errado — e é justamente por isso que os dois existem.")


# ---------------------------------------------------------------- emissão
CABECALHO = """// GERADO por scripts/sbe_gen.py — NÃO EDITE.
// Fonte: {fonte}   schemaId={sid} version={ver}
//
// Este arquivo é artefato de build (ADR-0006, ADR-0017): não é versionado e ninguém o edita.
// Para mudar uma mensagem, mude o XML. Mudar o SIGNIFICADO de um templateId existente é
// proibido (contrato de determinismo D7) — comportamento novo pede template novo.
#pragma once
"""


def emite_template_ids(f, ctx, mensagens):
    f.write(CABECALHO.format(**ctx))
    f.write("\n#include <cstdint>\n\nnamespace rv::codec {\n\n")
    f.write(f"inline constexpr uint16_t kSchemaId      = {ctx['sid']};\n")
    f.write(f"inline constexpr uint16_t kSchemaVersion = {ctx['ver']};\n\n")
    f.write("enum class Tmpl : uint16_t {\n")
    for m in mensagens:
        d = f"  // {m.descricao}" if m.descricao else ""
        f.write(f"  {m.nome} = {m.template_id},{d}\n")
    f.write("};\n\n")
    f.write(f"inline constexpr uint16_t kTemplateCount = {len(mensagens)};\n")
    f.write(f"inline constexpr uint16_t kMaxTemplateId = {max(m.template_id for m in mensagens)};\n\n")
    f.write("[[nodiscard]] constexpr bool is_known_template(uint16_t t) noexcept {\n")
    f.write("  switch (t) {\n")
    for m in mensagens:
        f.write(f"    case {m.template_id}:\n")
    f.write("      return true;\n    default:\n      return false;\n  }\n}\n\n")
    f.write("}  // namespace rv::codec\n")


def emite_struct(f, msg: Mensagem, indent="") -> None:
    if msg.descricao:
        f.write(f"{indent}// {msg.descricao}\n")
    f.write(f"{indent}struct alignas(8) {msg.nome} {{\n")
    if msg.template_id:
        f.write(f"{indent}  static constexpr uint16_t kTemplateId  = {msg.template_id};\n")
    f.write(f"{indent}  static constexpr uint16_t kBlockLength = {msg.block_length};\n\n")
    largura = max((len(c.tipo_c) for c in msg.campos), default=8)
    for c in msg.campos:
        decl = (f"{c.tipo_c:<{largura}} {c.nome}[{c.contagem}];" if c.contagem > 1
                else f"{c.tipo_c:<{largura}} {c.nome};")
        com = f"  // +{c.offset:<3} {c.comentario}".rstrip()
        f.write(f"{indent}  {decl}{com}\n")
    f.write(f"{indent}}};\n")
    # As afirmações que tornam o gerador confiável: tamanho, alinhamento, POD e CADA offset.
    f.write(f"{indent}static_assert(sizeof({msg.nome}) == {msg.nome}::kBlockLength,\n")
    f.write(f"{indent}              \"{msg.nome}: o compilador inseriu padding — o schema está mal ordenado\");\n")
    f.write(f"{indent}static_assert(alignof({msg.nome}) == 8);\n")
    f.write(f"{indent}static_assert(std::is_trivially_copyable_v<{msg.nome}>);\n")
    f.write(f"{indent}static_assert(std::is_standard_layout_v<{msg.nome}>);\n")
    for c in msg.campos:
        f.write(f"{indent}static_assert(offsetof({msg.nome}, {c.nome}) == {c.offset});\n")
    f.write("\n")


def emite_events(f, ctx, enums, mensagens):
    f.write(CABECALHO.format(**ctx))
    f.write("\n#include <cstddef>\n#include <cstdint>\n#include <type_traits>\n\n")
    f.write('#include "codec/template_ids.hpp"\n\nnamespace rv::codec {\n\n')
    for e in enums.values():
        if e.descricao:
            f.write(f"// {e.descricao}\n")
        f.write(f"enum class {e.nome} : {e.tipo_c} {{\n")
        for nome, valor in e.valores:
            f.write(f"  {nome} = {valor},\n")
        f.write("};\n\n")
    for m in mensagens:
        emite_struct(f, m)
        if m.grupo:
            f.write(f"// grupo repetido de {m.nome}: no máximo {m.grupo_max} ocorrências.\n")
            emite_struct(f, m.grupo)
            f.write(f"inline constexpr uint16_t kMax{m.grupo.nome} = {m.grupo_max};\n\n")
    f.write("}  // namespace rv::codec\n")


def snake(nome: str) -> str:
    saida = []
    for i, ch in enumerate(nome):
        if ch.isupper() and i:
            saida.append("_")
        saida.append(ch.lower())
    return "".join(saida)


def emite_decode(f, ctx, mensagens):
    f.write(CABECALHO.format(**ctx))
    f.write("\n#include <cstring>\n\n")
    f.write('#include "base/bytes.hpp"\n#include "base/status.hpp"\n#include "codec/events.hpp"\n')
    f.write('#include "codec/sbe_runtime.hpp"\n\nnamespace rv::codec {\n\n')
    for m in mensagens:
        n = snake(m.nome)
        f.write(f"[[nodiscard]] inline Result<const {m.nome}*> decode_{n}(ByteSpan b) noexcept {{\n")
        f.write(f"  return view_as<{m.nome}>(b);\n}}\n\n")
        f.write(f"[[nodiscard]] inline Result<uint16_t> encode(MutBytes b, const {m.nome}& m) noexcept {{\n")
        f.write(f"  if (b.size() < sizeof({m.nome})) return Status::fail(Err::OutOfRange);\n")
        f.write(f"  std::memcpy(b.data(), &m, sizeof({m.nome}));\n")
        f.write(f"  return static_cast<uint16_t>(sizeof({m.nome}));\n}}\n\n")
    # Tabela de blockLength por templateId: o validador do WAL usa para recusar payload curto
    # antes de qualquer reinterpretação.
    f.write("[[nodiscard]] constexpr uint16_t block_length_of(uint16_t tmpl) noexcept {\n")
    f.write("  switch (tmpl) {\n")
    for m in mensagens:
        f.write(f"    case {m.template_id}: return {m.nome}::kBlockLength;\n")
    f.write("    default: return 0;\n  }\n}\n\n")
    f.write("}  // namespace rv::codec\n")


def emite_debug(f, ctx, mensagens):
    f.write(CABECALHO.format(**ctx).replace("#pragma once\n", ""))
    f.write('\n#include "codec/events.hpp"\n\n#include <cstdio>\n\nnamespace rv::codec {\n\n')
    f.write("const char* template_name(uint16_t tmpl) noexcept {\n  switch (tmpl) {\n")
    for m in mensagens:
        f.write(f'    case {m.template_id}: return "{m.nome}";\n')
    f.write('    default: return "?";\n  }\n}\n\n')
    f.write("}  // namespace rv::codec\n")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("schema", type=pathlib.Path)
    ap.add_argument("-o", "--out", type=pathlib.Path,
                    help="diretório de saída; obrigatório exceto com --check")
    ap.add_argument("--check", action="store_true", help="só valida o schema, não escreve")
    args = ap.parse_args()

    try:
        sid, ver, pacote, tipos, enums, mensagens = ler_schema(args.schema)
        for m in mensagens:
            calcula_layout(m)
            if m.grupo:
                calcula_layout(m.grupo)
    except ErroDeSchema as e:
        print(f"sbe_gen: ERRO em {args.schema}: {e}", file=sys.stderr)
        return 2

    total = sum(m.block_length for m in mensagens)
    maior = max(mensagens, key=lambda m: m.block_length)
    print(f"sbe_gen: {len(mensagens)} mensagens, {len(enums)} enums, "
          f"maior bloco {maior.block_length} B ({maior.nome}), soma {total} B")
    if args.check:
        return 0
    if args.out is None:
        print("sbe_gen: -o/--out é obrigatório sem --check", file=sys.stderr)
        return 2

    args.out.mkdir(parents=True, exist_ok=True)
    ctx = {"fonte": str(args.schema), "sid": sid, "ver": ver, "pacote": pacote}
    for arquivo, fn in (("template_ids.hpp", lambda f: emite_template_ids(f, ctx, mensagens)),
                        ("events.hpp",       lambda f: emite_events(f, ctx, enums, mensagens)),
                        ("events_decode.hpp",lambda f: emite_decode(f, ctx, mensagens)),
                        ("events_debug.cpp", lambda f: emite_debug(f, ctx, mensagens))):
        with open(args.out / arquivo, "w") as f:
            fn(f)
        print(f"  {args.out / arquivo}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
