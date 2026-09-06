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
é melhor"), o limiar, QUAIS séries são contratuais e como cada uma se chama em português corrente
vêm DO PRÓPRIO JSON, escritos pelo harness a partir de `bench/contrato.hpp`. Duas verdades sobre a
mesma decisão é o começo de um dia ruim; aqui há uma só, e ela mora no C++.

A lista de séries obrigatórias já foi um default digitado neste arquivo — a terceira cópia de uma
tabela que também estava em `saida.cpp` e em `comparador.cpp`. Renomear a série atualizava uma
delas, e as outras passavam a procurar um nome inexistente sem que nada acusasse: o relatório dizia
"nenhuma regressão" sobre uma métrica que não tinha olhado. Agora a lista vem do bloco `contrato`
do JSON de medição, e um rename só existe em um lugar.

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


# Os campos que definem a carga. São os MESMOS cinco que o comparador em C++ confere: `eventos`
# fica de fora de propósito, porque ele é consequência dos outros e pode variar por uma mudança no
# simulador, o que é a diferença que se quer medir e não a que invalida a medição.
CAMPOS_CARGA = ("dias", "negocios_por_dia", "investidores", "particoes", "semente")


def carga_divergente(doc_a, doc_b):
    """Como as cargas dos dois lados diferem, ou None quando batem.

    O comparador em C++ recusa confrontar cargas diferentes (código 4) e este script comparava
    qualquer par que lhe dessem — mesmo defeito, lado diferente. Um pregão dá 7,3 M eventos/s e
    três dão 5,4 M da MESMA métrica: comparados entre si, isso vira uma "regressão" de 26 % que não
    existe, e o gate reprova um PR inocente por medir dois experimentos.
    """
    a, b = doc_a.get("carga") or {}, doc_b.get("carga") or {}
    if not a or not b:
        return "um dos lados não declara a carga"
    difs = [f"{c} ({b[c]} na base, {a[c]} neste)" for c in CAMPOS_CARGA
            if c in a and c in b and a[c] != b[c]]
    return ", ".join(difs) if difs else None


def contrato_de(doc):
    """As séries contratuais e o nome de cada uma em português corrente.

    Vem do bloco `contrato`, que o harness emite de `bench/contrato.hpp`. JSON sem o bloco é de um
    harness anterior a ele: aqui a lista sai vazia, e quem chama DIZ isso no relatório em vez de
    seguir como se não houvesse métrica obrigatória nenhuma.
    """
    return [c for c in doc.get("contrato", []) if c.get("serie")]


def linha_resumo(e, l, unidade):
    """Uma métrica contratual em três colunas: o que se mediu, e como ficou contra a base."""
    if e is None or not e["medida"]:
        return "não medida nesta execução", "—"
    valor = f"{num(e['valor'])} {unidade}"
    if l is None:
        # Medida, mas fora da comparação: instável de um dos lados, ou sem base. Dizer qual dos
        # dois é o que separa "não piorou" de "não deu para olhar".
        return valor, ("instável — fora do veredito" if not e["estavel"] else "sem base")
    if l["regressao"]:
        return valor, f"**{l['delta']:+.1f}% — pior**"
    if l["ganho"]:
        return valor, f"{l['delta']:+.1f}% — melhor"
    return valor, f"{l['delta']:+.1f}% — igual, dentro do ruído"


def vereditos(tem_base, houve_regressao, faltando, obrigatorias, comparadas, carga_ruim):
    """O veredito em duas formas: a do resumo (para quem abre o job) e a do detalhe.

    Uma função só porque as duas frases já divergiram: o resumo dizia "não deu para dizer" e, vinte
    linhas abaixo, o detalhe dizia "nenhuma regressão". Duas conclusões diferentes no mesmo
    documento é pior que nenhuma — quem lê escolhe a que preferir.
    """
    if carga_ruim:
        return (
            "**Não dá para comparar os dois lados:** eles mediram cargas diferentes — "
            f"{carga_ruim}. Números de sessões diferentes não se confrontam; o que está abaixo "
            "descreve este commit e nada mais.",
            f"comparação recusada — a base mediu outra carga ({carga_ruim}).",
        )
    if not tem_base:
        return (
            "**Medição sem comparação.** Não houve uma base para confrontar: os números abaixo "
            "descrevem este commit, e não dizem se ele ficou mais rápido ou mais lento.",
            "",
        )
    if houve_regressao:
        return (
            "**Ficou mais lento.** Pelo menos uma métrica piorou além do que a variação natural "
            "da máquina explica — a tabela abaixo diz qual.",
            "há regressão acima do ruído. ADR-0016: nenhuma otimização é aprovável contra um "
            "baseline que ela mesma derrubou.",
        )
    if obrigatorias and not comparadas:
        # "Nada regrediu" quando nenhuma métrica obrigatória pôde ser comparada é a frase que este
        # projeto chama de gate verde sem ter feito nada. O relatório diz o que houve: cegueira.
        return (
            "**Não deu para dizer.** Nenhuma das métricas obrigatórias pôde ser comparada — elas "
            "ficaram instáveis demais em algum dos lados. As outras séries não regrediram, mas as "
            "que decidem não foram olhadas.",
            "nenhuma regressão entre as séries que deu para comparar — e NENHUMA das métricas "
            "obrigatórias entrou nessa conta.",
        )
    if faltando:
        return (
            "**Sem regressão no que deu para comparar.** Nada piorou além da variação natural da "
            "máquina — mas parte das métricas obrigatórias ficou de fora, e o veredito não fala "
            "por elas.",
            "nenhuma regressão acima do ruído entre as séries comparadas; parte das obrigatórias "
            "ficou de fora.",
        )
    return (
        "**Sem regressão.** Nenhuma métrica piorou além da variação natural da máquina.",
        "nenhuma regressão acima do ruído.",
    )


def secao_resumo(doc, cabeca, linhas, tem_base, frase, faltando, saida):
    """As primeiras linhas do relatório: passou ou não, e os poucos números que decidem isso.

    Existe porque o resumo de um job do GitHub é lido por quem quer uma resposta, não por quem vai
    auditar 24 séries. O detalhe continua embaixo, inteiro; o que muda é que ninguém precisa
    reconstruir o veredito lendo uma tabela de medianas.
    """
    contrato = contrato_de(doc)
    por_nome = {l["nome"]: l for l in linhas}

    saida.append("## Resumo\n")
    saida.append(frase + "\n")
    if contrato:
        cab = "| o que se mediu | este commit | contra a base |" if tem_base else (
            "| o que se mediu | medido |"
        )
        saida.append(cab)
        saida.append("|---|---:|---|" if tem_base else "|---|---:|")
        for c in contrato:
            e = cabeca.get(c["serie"])
            unidade = e["unidade"] if e else ""
            valor, veredito = linha_resumo(e, por_nome.get(c["serie"]), unidade)
            saida.append(
                f"| {c['rotulo']} | {valor} | {veredito} |" if tem_base
                else f"| {c['rotulo']} | {valor} |"
            )
    else:
        saida.append(
            "_Este JSON não traz o bloco `contrato` (foi gerado por um harness anterior a ele): "
            "não dá para dizer aqui quais séries são obrigatórias. O detalhe está abaixo._"
        )

    carga = doc.get("carga") or {}
    if carga.get("eventos"):
        saida.append(
            f"\nMedido sobre {carga['dias']} pregão(ões) simulado(s) — {carga['negocios_por_dia']} "
            f"negócios por dia e {carga['investidores']} investidores, {carga['eventos']} eventos "
            "no total."
        )
    if faltando:
        saida.append(
            "\n> **Atenção:** "
            + ", ".join(f"`{n}`" for n in faltando)
            + " não entrou na comparação (ficou instável ou ausente em um dos lados). O veredito "
            "acima não fala por ela."
        )
    if not doc["ambiente"].get("valido_para_baseline", True):
        saida.append(
            f"\n> **Estes números não valem como baseline:** "
            f"{doc['ambiente'].get('por_que_invalido', '?')}."
        )
    saida.append("")


def secao_ambiente(doc, saida):
    amb, carga, h = doc["ambiente"], doc.get("carga", {}), doc["harness"]
    saida.append("\n## Ambiente e método\n")
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
        default=None,
        help="séries que TÊM de ser comparáveis; se alguma não for, o script sai com 2. "
        "Padrão: as do bloco `contrato` do JSON de medição (que o harness emite de "
        "bench/contrato.hpp) — não há lista digitada aqui",
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
    # A lista de séries obrigatórias vem do JSON, não daqui. Ver contrato_de().
    exigidas = a.exigir if a.exigir is not None else [c["serie"] for c in contrato_de(docs[0])]

    # A comparação é calculada ANTES de escrever qualquer coisa: o resumo abre o relatório e
    # precisa do veredito que antes só existia no meio dele.
    linhas, nao_comparadas, docs_base = [], [], None
    houve_regressao = False
    faltando = []
    carga_ruim = None
    if a.contra:
        docs_base = carrega(a.contra)
        carga_ruim = carga_divergente(docs[0], docs_base[0])
    if a.contra and not carga_ruim:
        base = consolida(docs_base)
        linhas, nao_comparadas = compara(cabeca, base, limiar, a.sigmas)
        houve_regressao = any(l["regressao"] for l in linhas)
        # Uma métrica que o projeto trata como contratual não pode sumir da comparação sem que
        # alguém veja. Aconteceu no teste deste workflow: `nucleo.loop.eventos_por_s_por_core`
        # ficou instável de um dos lados, saiu para o rodapé de "não comparadas", e o relatório
        # dizia "nenhuma regressão" sobre a métrica que mais importa — que ele não tinha olhado.
        # Sumir em silêncio é a única coisa que um relatório não pode fazer.
        comparadas = {l["nome"] for l in linhas}
        faltando = [n for n in exigidas if n not in comparadas]

    # As duas frases do veredito saem da MESMA função: o resumo e o detalhe não podem discordar.
    obrigatorias = [c["serie"] for c in contrato_de(docs[0])]
    obrig_comparadas = [n for n in obrigatorias if any(l["nome"] == n for l in linhas)]
    frase_resumo, frase_detalhe = vereditos(
        bool(a.contra), houve_regressao, faltando, obrigatorias, obrig_comparadas, carga_ruim
    )

    saida = [f"# {a.titulo}\n"]
    secao_resumo(docs[0], cabeca, linhas, bool(a.contra), frase_resumo, faltando, saida)
    secao_ambiente(docs[0], saida)
    if len(a.medicao) > 1:
        saida.append(
            f"_{len(a.medicao)} execuções deste commit; cada série mostra a **mediana** delas, e o "
            "CV soma a dispersão dentro de uma execução com a dispersão entre execuções._\n"
        )

    if a.contra and carga_ruim:
        saida.append(
            f"\n> **Comparação recusada:** a base mediu outra carga — {carga_ruim}. É a mesma "
            "recusa que `motor-rv-bench --comparar` faz com código 4.\n"
        )
    if a.contra and not carga_ruim:
        # O commit da base sai no cabeçalho junto com o do PR. Um relatório de comparação que não
        # nomeia os dois lados obriga quem lê a confiar que o CI pegou o par certo.
        saida.append(f"\n_Base comparada: `{docs_base[0]['ambiente']['commit']}`._\n")
        saida.append("\n> **Veredito:** " + frase_detalhe + "\n")
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
    # Carga divergente cai no 2 — "não deu para olhar" — e não no 1: não houve regressão medida,
    # houve uma comparação que não podia ser feita.
    return 2 if (faltando or carga_ruim) else 0


if __name__ == "__main__":
    sys.exit(main())
