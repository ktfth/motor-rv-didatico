#!/usr/bin/env python3
"""Transforma a saída do harness (`motor-rv-bench --out`) em relatório legível — e, quando
recebe dois lados, em veredito de regressão.

Por que existe, se o harness já tem comparador
----------------------------------------------
O comparador em C++ (`motor-rv-bench --comparar`) responde uma pergunta: "esta medição regrediu
contra `bench/baseline.json`?". Ele é o gate de ADR-0016, e só funciona onde o baseline vale —
a máquina de referência de ADR-0022.

No CI a pergunta é outra: "este PR está mais lento que a base?". A resposta não pode vir de um
baseline fixado em outra máquina; ela vem de medir OS DOIS LADOS NA MESMA EXECUÇÃO, no mesmo
runner, minutos um do outro. É o único arranjo em que a comparação isola a mudança de código do
hardware — e é o que este script faz.

O que ele NÃO faz é reimplementar política. A direção de cada métrica ("maior é melhor" ou "menor
é melhor") e o limiar vêm DO PRÓPRIO JSON, escritos pelo harness. Duas verdades sobre a mesma
decisão é o começo de um dia ruim; aqui há uma só, e ela mora no C++.

A regra de ruído
----------------
Um runner compartilhado não repete um número: nesta mesma sessão, duas execuções idênticas deram
7,30 e 7,63 M eventos/s — 4,5 % de diferença sem uma linha de código mudada. Um limiar fixo de 5 %
nesse ambiente reprova PR inocente, e um gate que reprova por motivo errado é desligado com raiva.

Então a regressão só é declarada quando a diferença passa de DOIS testes ao mesmo tempo:

  1. o limiar do projeto (`limiar_regressao_pct`, 5 % por padrão), e
  2. `--sigmas` vezes o ruído combinado das duas séries — e "ruído" aqui soma DUAS fontes: a
     dispersão dentro de uma execução (que o harness mede) e a dispersão ENTRE as execuções do
     mesmo lado (que só aparece quando se roda mais de uma vez). A segunda costuma ser a maior:
     nesta sessão, uma série de snapshot com 3,8 % de CV interno andou 23 % entre duas execuções
     idênticas. Usar só a primeira é o jeito de construir um gate que reprova PR inocente.

O segundo é auto-calibrado: numa máquina quieta ele quase não aparece e o limiar do projeto manda;
num runner barulhento ele cresce sozinho e evita o falso positivo. Séries instáveis (recusadas pelo
harness) não entram na comparação — elas são listadas, com o motivo.
"""

import argparse
import json
import math
import sys

# --------------------------------------------------------------------------- leitura


def carrega(caminhos):
    docs = []
    for c in caminhos:
        with open(c, encoding="utf-8") as f:
            docs.append(json.load(f))
    return docs


def mediana(valores):
    v = sorted(valores)
    n = len(v)
    if n == 0:
        return 0.0
    return v[n // 2] if n % 2 else (v[n // 2 - 1] + v[n // 2]) / 2.0


def cv_pct(valores):
    """Coeficiente de variação ENTRE execuções, em porcento."""
    n = len(valores)
    if n < 2:
        return 0.0
    m = sum(valores) / n
    if m == 0:
        return 0.0
    var = sum((x - m) ** 2 for x in valores) / (n - 1)
    return 100.0 * math.sqrt(var) / abs(m)


def consolida(docs):
    """Junta N execuções do mesmo lado numa tabela por nome de série."""
    fora = {}
    for d in docs:
        for s in d["series"]:
            e = fora.setdefault(
                s["nome"],
                {
                    "grupo": s["grupo"],
                    "unidade": s["unidade"],
                    "direcao": s.get("direcao", "maior_melhor"),
                    "valores": [],
                    "cvs": [],
                    "estavel": True,
                    "medida": True,
                    "notas": [],
                    "quantis": s.get("quantis_ns"),
                },
            )
            if not s["medida"]:
                e["medida"] = False
            if not s["estavel"]:
                e["estavel"] = False
            if s["medida"]:
                e["valores"].append(s["mediana"])
                e["cvs"].append(s["cv_pct"])
            if s.get("nota") and s["nota"] not in e["notas"]:
                e["notas"].append(s["nota"])
            if s.get("quantis_ns"):
                e["quantis"] = s["quantis_ns"]
    for nome, e in fora.items():
        e["nome"] = nome
        # Mediana, e não a melhor das execuções. A melhor descarta justamente a informação de que
        # o ruído precisa: se uma execução saiu 20 % pior, isso É o ruído desta máquina, e escondê-lo
        # produz um σ pequeno demais e um gate que reprova PR inocente. A mediana resiste a um
        # outlier em qualquer direção e deixa o spread visível para a linha de baixo.
        e["valor"] = mediana(e["valores"])
        # O ruído de uma série tem DUAS fontes, e usar só a primeira foi um erro que esta própria
        # sessão cometeu e mediu: dentro de uma execução o harness viu 2-4 % de CV, e entre
        # execuções idênticas a mesma métrica andou 23 %. Quem gasta o orçamento do gate é a
        # segunda. Somadas em quadratura, porque são independentes.
        e["cv_intra"] = max(e["cvs"]) if e["cvs"] else 0.0
        e["cv_entre"] = cv_pct(e["valores"])
        e["cv"] = math.sqrt(e["cv_intra"] ** 2 + e["cv_entre"] ** 2)
        e["execucoes"] = len(e["valores"])
    return fora


# --------------------------------------------------------------------------- comparação


def compara(cabeca, base, limiar_pct, sigmas):
    linhas, nao_comparadas = [], []
    for nome, h in sorted(cabeca.items()):
        b = base.get(nome)
        if b is None:
            nao_comparadas.append((nome, "não existe na base"))
            continue
        if not (h["medida"] and h["estavel"]):
            nao_comparadas.append((nome, "instável ou pulada neste PR"))
            continue
        if not (b["medida"] and b["estavel"]):
            nao_comparadas.append((nome, "instável ou pulada na base"))
            continue
        if b["valor"] == 0:
            nao_comparadas.append((nome, "valor zero na base"))
            continue

        delta = 100.0 * (h["valor"] - b["valor"]) / b["valor"]
        ruido = math.sqrt(h["cv"] ** 2 + b["cv"] ** 2)
        exigido = max(limiar_pct, sigmas * ruido)
        pior = delta < -exigido if h["direcao"] == "maior_melhor" else delta > exigido
        melhorou = delta > exigido if h["direcao"] == "maior_melhor" else delta < -exigido
        linhas.append(
            {
                "nome": nome,
                "grupo": h["grupo"],
                "unidade": h["unidade"],
                "base": b["valor"],
                "cabeca": h["valor"],
                "delta": delta,
                "ruido": ruido,
                "exigido": exigido,
                "regressao": pior,
                "ganho": melhorou,
            }
        )
    return linhas, nao_comparadas


# --------------------------------------------------------------------------- escrita


def num(v):
    if abs(v) >= 1000:
        return f"{v:,.0f}".replace(",", " ")
    return f"{v:.2f}"


def secao_ambiente(doc, saida):
    amb, carga, h = doc["ambiente"], doc.get("carga", {}), doc["harness"]
    saida.append("| | |")
    saida.append("|---|---|")
    saida.append(f"| commit | `{amb['commit']}` |")
    saida.append(f"| máquina | {amb['maquina']} |")
    saida.append(f"| kernel | {amb['kernel']} |")
    saida.append(f"| flags | `{amb['flags']}` |")
    saida.append(f"| WAL | {amb['dispositivo_wal']} |")
    if carga.get("eventos"):
        saida.append(
            f"| carga | {carga['dias']} pregão(ões), {carga['negocios_por_dia']} negócios/dia, "
            f"{carga['investidores']} investidores, {carga['particoes']} partição(ões), "
            f"semente {carga['semente']} → **{carga['eventos']} eventos** |"
        )
    saida.append(
        f"| harness | {h['aquecimento']} aquecimentos, {h['repeticoes']} repetições, "
        f"descarte acima de {h['limiar_cv_pct']:.0f}% de CV, até {h['tentativas']} tentativas |"
    )
    if not amb.get("valido_para_baseline", True):
        saida.append(
            f"\n> **Esta medição não vale como baseline:** {amb.get('por_que_invalido', '?')}."
        )
    saida.append("")


def secao_series(tabela, saida):
    grupo_atual = None
    for nome, e in sorted(tabela.items(), key=lambda kv: (kv[1]["grupo"], kv[0])):
        if e["grupo"] != grupo_atual:
            grupo_atual = e["grupo"]
            saida.append(f"\n### `{grupo_atual}`\n")
            saida.append("| série | valor | unidade | CV | |")
            saida.append("|---|---:|---|---:|---|")
        if not e["medida"]:
            saida.append(f"| `{nome}` | — | {e['unidade']} | — | pulada: {'; '.join(e['notas'])} |")
            continue
        estado = "" if e["estavel"] else "**instável — não vira baseline**"
        cv = (
            f"{e['cv']:.2f}%"
            if e.get("execucoes", 1) < 2
            else f"{e['cv']:.2f}% ({e['cv_intra']:.1f} intra + {e['cv_entre']:.1f} entre)"
        )
        saida.append(f"| `{nome}` | {num(e['valor'])} | {e['unidade']} | {cv} | {estado} |")
        if e.get("quantis"):
            q = e["quantis"]
            saida.append(
                f"| ↳ quantis (ns) | p50 {q['p50']} · p99 {q['p99']} · p999 {q['p999']} · "
                f"máx {q['max']} | | | |"
            )
    saida.append("")


def secao_ausentes(doc, saida):
    ausentes = doc.get("metricas_ausentes") or {}
    if not ausentes:
        return
    saida.append("\n## Métricas do esquema ainda não medidas\n")
    saida.append("| métrica | por quê |")
    saida.append("|---|---|")
    for k, v in ausentes.items():
        saida.append(f"| `{k}` | {v} |")
    saida.append("")


def secao_comparacao(linhas, nao_comparadas, limiar_pct, sigmas, saida):
    saida.append("\n## Comparação com a base (mesmo runner, mesma execução)\n")
    saida.append(
        f"Regressão exige passar dos dois testes: **{limiar_pct:.0f}%** (limiar do projeto) "
        f"E **{sigmas:.0f}× o ruído** medido das duas séries. A coluna *exigido* mostra qual "
        "dos dois mandou em cada linha.\n"
    )
    if linhas:
        saida.append("| série | base | este PR | Δ | ruído | exigido | |")
        saida.append("|---|---:|---:|---:|---:|---:|---|")
        for l in sorted(linhas, key=lambda x: (not x["regressao"], x["nome"])):
            marca = "**REGRESSÃO**" if l["regressao"] else ("ganho" if l["ganho"] else "ok")
            saida.append(
                f"| `{l['nome']}` | {num(l['base'])} | {num(l['cabeca'])} | {l['delta']:+.2f}% | "
                f"±{l['ruido']:.2f}% | {l['exigido']:.2f}% | {marca} |"
            )
    else:
        saida.append("_Nenhuma série comparável._")
    if nao_comparadas:
        saida.append("\n<details><summary>Séries não comparadas</summary>\n")
        for nome, motivo in nao_comparadas:
            saida.append(f"- `{nome}` — {motivo}")
        saida.append("\n</details>")
    saida.append("")


# --------------------------------------------------------------------------- principal


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--medicao", nargs="+", required=True, help="JSON(s) deste commit")
    p.add_argument("--contra", nargs="*", default=[], help="JSON(s) da base, para comparar")
    p.add_argument("--saida", help="arquivo Markdown (padrão: stdout)")
    p.add_argument("--titulo", default="Relatório de medição — motor-rv")
    p.add_argument("--limiar-pct", type=float, default=None, help="sobrepõe o do JSON")
    p.add_argument(
        "--exigir",
        nargs="*",
        default=["nucleo.loop.eventos_por_s_por_core"],
        help="séries que TÊM de ser comparáveis; se alguma não for, o script sai com 2",
    )
    p.add_argument(
        "--sigmas",
        type=float,
        default=3.0,
        help="quantos desvios do ruído medido a diferença tem de passar (padrão 3)",
    )
    a = p.parse_args()

    docs = carrega(a.medicao)
    cabeca = consolida(docs)
    limiar = a.limiar_pct if a.limiar_pct is not None else docs[0].get("limiar_regressao_pct", 5.0)

    saida = [f"# {a.titulo}\n"]
    secao_ambiente(docs[0], saida)
    if len(a.medicao) > 1:
        saida.append(
            f"_{len(a.medicao)} execuções deste commit; cada série mostra a **mediana** delas, e o "
            "CV soma a dispersão dentro de uma execução com a dispersão entre execuções._\n"
        )

    houve_regressao = False
    faltando = []
    if a.contra:
        docs_base = carrega(a.contra)
        base = consolida(docs_base)
        # O commit da base sai no cabeçalho junto com o do PR. Um relatório de comparação que não
        # nomeia os dois lados obriga quem lê a confiar que o CI pegou o par certo.
        saida.append(f"\n_Base comparada: `{docs_base[0]['ambiente']['commit']}`._\n")
        linhas, nao_comparadas = compara(cabeca, base, limiar, a.sigmas)
        houve_regressao = any(l["regressao"] for l in linhas)

        # Uma métrica que o projeto trata como contratual não pode sumir da comparação sem que
        # alguém veja. Aconteceu no teste deste workflow: `nucleo.loop.eventos_por_s_por_core`
        # ficou instável de um dos lados, saiu para o rodapé de "não comparadas", e o relatório
        # dizia "nenhuma regressão" sobre a métrica que mais importa — que ele não tinha olhado.
        # Sumir em silêncio é a única coisa que um relatório não pode fazer.
        comparadas = {l["nome"] for l in linhas}
        faltando = [n for n in a.exigir if n not in comparadas]
        saida.append(
            "\n> **Veredito:** "
            + (
                "há regressão acima do ruído. ADR-0016: nenhuma otimização é aprovável contra um "
                "baseline que ela mesma derrubou."
                if houve_regressao
                else "nenhuma regressão acima do ruído."
            )
            + "\n"
        )
        if faltando:
            saida.append(
                "\n> **Atenção:** a comparação NÃO cobriu "
                + ", ".join(f"`{n}`" for n in faltando)
                + " — a série ficou instável ou ausente em um dos lados. O veredito acima vale "
                "para o resto, não para ela.\n"
            )
        secao_comparacao(linhas, nao_comparadas, limiar, a.sigmas, saida)

    saida.append("\n## Séries medidas\n")
    secao_series(cabeca, saida)
    secao_ausentes(docs[0], saida)
    saida.append(
        "\n---\nGerado por `scripts/relatorio-bench.py` a partir da saída de `motor-rv-bench` "
        "(ADR-0021). Método, limitações e o que os números já revelaram: "
        "`bench/reports/2026-09-06-medicao-inicial.md`."
    )

    texto = "\n".join(saida) + "\n"
    if a.saida:
        with open(a.saida, "w", encoding="utf-8") as f:
            f.write(texto)
    else:
        sys.stdout.write(texto)

    # 0 = sem regressão; 1 = regressão; 2 = uma série exigida não pôde ser comparada. O 2 é
    # separado do 1 de propósito: "está mais lento" e "não deu para olhar" são fatos diferentes,
    # e quem chama decide o que fazer com cada um.
    if houve_regressao:
        return 1
    return 2 if faltando else 0


if __name__ == "__main__":
    sys.exit(main())
