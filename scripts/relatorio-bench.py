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
  2. `--sigmas` vezes o ruído DO NÚMERO PUBLICADO de cada lado.

O segundo teste já esteve errado, e o erro tinha um sintoma medido: uma regressão de 2× passava
batido, cinco vezes em cinco. A causa era o estimador. O ruído era a DISPERSÃO — o maior CV interno
visto somado em quadratura com o CV entre execuções — e dispersão responde "quanto esta série
balança", que não é a pergunta do gate. A pergunta do gate é "quanto balança a MEDIANA que estou
comparando", e essa é o erro padrão dela: `cv_intra/√repetições` e `cv_entre/√execuções`, somados
em quadratura. A diferença não é acadêmica: com dispersão, `exigido` ficava em ~56 % e um Δ de
−47,5 % era "dentro do ruído"; com o erro padrão, `exigido` fica entre 24 % e 53 % e a mesma queda
é regressão. Dispersão não encolhe medindo mais; erro padrão encolhe com √n, que é exatamente o que
se compra ao repetir.

O que NÃO entra nessa conta é o `estavel` do harness. Aquela marca é o filtro de CV intra-execução
(5 %) que decide se um número pode virar BASELINE — pergunta diferente. Usá-la como pré-condição da
comparação fechava o portão: `nucleo.loop` saiu estável em 16 de 30 execuções e `snapshot.salva` em
23 de 30, e como uma execução instável marca o lado inteiro, os dois lados sobreviverem juntos tinha
~2 % de chance. Série instável é comparada; o ruído maior dela entra no limiar exigido, que é onde
a instabilidade deve pesar, e a marca continua visível no relatório.
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
        reps_harness = (d.get("harness") or {}).get("repeticoes", 0)
        for s in d["series"]:
            e = fora.setdefault(
                s["nome"],
                {
                    "grupo": s["grupo"],
                    "unidade": s["unidade"],
                    "direcao": s.get("direcao", "maior_melhor"),
                    "valores": [],
                    "cvs": [],
                    "reps": [],
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
                e["reps"].append(len(s.get("amostras") or []) or reps_harness)
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
        e["execucoes"] = len(e["valores"])
        e["repeticoes"] = max(1, int(mediana(e["reps"]))) if e["reps"] else 1

        # ------------------------------------------------------------------ dois números, dois usos
        # DISPERSÃO (o que a tabela mostra): quanto a série balança. É descrição, e usa estimadores
        # conservadores — o MAIOR CV interno visto e o CV entre execuções.
        e["cv_intra_max"] = max(e["cvs"]) if e["cvs"] else 0.0
        e["cv_entre"] = cv_pct(e["valores"])
        e["cv"] = math.sqrt(e["cv_intra_max"] ** 2 + e["cv_entre"] ** 2)

        # ERRO PADRÃO DA MEDIANA (o que o gate usa): quanto o NÚMERO PUBLICADO — a mediana das
        # execuções — balança. É outra pergunta, e a diferença entre as duas é o que fazia o gate
        # não ver uma regressão de 2×: com dispersão, `exigido` ficava em ~56 % e um Δ de −47 %
        # passava batido. A dispersão não encolhe com mais medições; o erro da mediana encolhe com
        # √n, que é exatamente o que se compra ao repetir a medida.
        #
        # Mediana dos CVs internos, e não o máximo: o máximo é o pior caso de UMA execução e não
        # descreve o erro da mediana de todas. Somados em quadratura porque as duas fontes — dentro
        # de uma execução e entre execuções — são independentes.
        cv_intra = mediana(e["cvs"]) if e["cvs"] else 0.0
        erro_intra = cv_intra / math.sqrt(e["repeticoes"])
        erro_entre = e["cv_entre"] / math.sqrt(e["execucoes"]) if e["execucoes"] else 0.0
        e["erro_padrao"] = math.sqrt(erro_intra**2 + erro_entre**2)
    return fora


# --------------------------------------------------------------------------- comparação


# Teto de ruído: acima disto a comparação não decide nada.
#
# O número não é gosto: ele ESPELHA uma frase do bench/README.md — "o gate pega regressão grossa —
# um fator". Um fator é 2×, que é um Δ de −50 %. As duas constantes vivem em arquivos diferentes e
# uma é o argumento da outra, então elas mudam juntas: prometer pegar 1,5× obriga a baixar o teto
# para 33 %, e subir o teto obriga a reescrever a promessa. Se o limiar exigido passa de 50 %, uma regressão de 2× cabe dentro
# dele: o gate deixou de conseguir cumprir o que promete, e chamar isso de "sem regressão" é a
# mesma aprovação vazia que o resto desta rodada corrigiu. Então acima do teto a série contratual
# conta como NÃO COMPARADA (código 2), e não como aprovada.
#
# Tirar o filtro de `estavel` foi certo — ele respondia outra pergunta — mas nada tinha ficado no
# lugar dele como teto: com CV de 200 % dos dois lados, uma queda de 80 % saía "dentro do ruído".
TETO_EXIGIDO_PCT = 50.0


def compara(cabeca, base, limiar_pct, sigmas, teto_pct=TETO_EXIGIDO_PCT):
    """Confronta os dois lados série a série.

    O que NÃO é pré-condição aqui: o `estavel` do harness. Aquela marca é o filtro de CV
    intra-execução (5 %, `bench/harness.cpp`) e responde a pergunta "este número pode virar
    baseline?" — que não é a pergunta deste script. Usá-la como porta de entrada da comparação
    fechava o portão A/B: medido neste projeto, `nucleo.loop` saiu estável em 16 de 30 execuções e
    `snapshot.salva` em 23 de 30; como uma execução instável marca o lado inteiro, os dois lados
    sobreviverem juntos tinha probabilidade de ~2 %. O gate avisava e não olhava nada.

    Uma série instável não é descartada: ela é comparada e o ruído dela — maior — entra no limiar
    exigido. Até o teto: passando dele, a linha sai com `decide = False`, porque um limiar maior
    que a regressão que o gate promete pegar não é um limiar, é uma vista grossa.
    """
    linhas, nao_comparadas = [], []
    for nome, h in sorted(cabeca.items()):
        b = base.get(nome)
        if b is None:
            nao_comparadas.append((nome, "não existe na base"))
            continue
        if not h["medida"]:
            nao_comparadas.append((nome, "não medida neste PR"))
            continue
        if not b["medida"]:
            nao_comparadas.append((nome, "não medida na base"))
            continue
        if b["valor"] == 0:
            nao_comparadas.append((nome, "valor zero na base"))
            continue

        delta = 100.0 * (h["valor"] - b["valor"]) / b["valor"]
        ruido = math.sqrt(h["erro_padrao"] ** 2 + b["erro_padrao"] ** 2)
        exigido = max(limiar_pct, sigmas * ruido)
        decide = exigido <= teto_pct
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
                "decide": decide,
                "regressao": pior and decide,
                "ganho": melhorou and decide,
                "instavel": not (h["estavel"] and b["estavel"]),
            }
        )
    return linhas, nao_comparadas


def veredito_de(linhas, nao_comparadas, exigidas, tem_base, carga_ruim, sem_exigencia):
    """O veredito inteiro: as duas frases E o código de saída, decididos no mesmo lugar.

    Eram dois lugares com precedências diferentes, e a contradição aparecia no relatório: cabeça
    schema 1 contra base schema 2 imprimia "Não dá para dar veredito" no resumo e devolvia 1, com o
    job anunciando `::error::regressão de desempenho`.

    Quem decide o veredito são as séries DO CONTRATO. As outras aparecem no relatório e não votam:
    medido em 15 rodadas de PR inocente com os parâmetros do job, olhar todas dava 1 vermelho
    falso; olhar só as contratuais, nenhum. O contrato existe exatamente para dizer quais métricas
    decidem — e era o próprio contrato que o gate ignorava na hora de reprovar.
    """
    por_nome = {l["nome"]: l for l in linhas}
    motivos = dict(nao_comparadas)
    decisivas = [por_nome[n] for n in exigidas if n in por_nome and por_nome[n]["decide"]]
    houve_regressao = any(l["regressao"] for l in decisivas)

    # Por que cada contratual ficou de fora, na linguagem de quem lê o resumo.
    fora = {}
    for n in exigidas:
        if n in por_nome and por_nome[n]["decide"]:
            continue
        if n in por_nome:
            fora[n] = (f"ruído acima do teto: exigiria {por_nome[n]['exigido']:.1f}%, "
                       f"teto {TETO_EXIGIDO_PCT:.0f}%")
        else:
            fora[n] = motivos.get(n, "não comparada")
    # "Não existe na base" é métrica nova deste PR: não há comparação possível, e reprovar por isso
    # seria reprovar quem acrescenta métrica. As outras causas são cegueira do gate.
    novas = [n for n, m in fora.items() if m == "não existe na base"]
    cegas = [n for n in fora if n not in novas]

    if sem_exigencia:
        return (
            "**Não dá para dar veredito.** Nenhuma série foi declarada como bloqueante — ou este "
            "JSON não traz o bloco `contrato` (é de um harness anterior a ele), ou `--exigir` veio "
            "sem valores. Sem uma lista do que tem de ser comparado, \"sem regressão\" seria uma "
            "afirmação sobre coisa nenhuma.",
            "nenhuma série bloqueante declarada: não havia o que exigir.",
            2, cegas, novas, fora,
        )
    if carga_ruim:
        return (
            "**Não dá para comparar os dois lados:** eles mediram cargas diferentes — "
            f"{carga_ruim}. Números de sessões diferentes não se confrontam; o que está abaixo "
            "descreve este commit e nada mais.",
            f"comparação recusada — a base mediu outra carga ({carga_ruim}).",
            2, cegas, novas, fora,
        )
    if not tem_base:
        return (
            "**Medição sem comparação.** Não houve uma base para confrontar: os números abaixo "
            "descrevem este commit, e não dizem se ele ficou mais rápido ou mais lento.",
            "", 0, cegas, novas, fora,
        )
    if houve_regressao:
        return (
            "**Ficou mais lento.** Pelo menos uma métrica obrigatória piorou além do que a "
            "variação natural da máquina explica — a tabela abaixo diz qual.",
            "há regressão acima do ruído. ADR-0016: nenhuma otimização é aprovável contra um "
            "baseline que ela mesma derrubou.",
            1, cegas, novas, fora,
        )
    if exigidas and not decisivas:
        # Nenhuma métrica obrigatória decidiu nada. Vale inclusive quando TODAS são novas — o
        # estado que um PR produz ao renomear a série no contrato e no ponto de registro, que é
        # uma edição de uma linha que este desenho incentiva. Verde ali seria o gate aprovando
        # sem ter comparado coisa nenhuma, de novo.
        return (
            "**Não deu para dizer.** Nenhuma das métricas obrigatórias pôde ser comparada. As "
            "outras séries não regrediram, mas as que decidem não foram olhadas.",
            "nenhuma regressão entre as séries comparadas — e NENHUMA das métricas obrigatórias "
            "entrou nessa conta.",
            2, cegas, novas, fora,
        )
    if cegas:
        return (
            "**Sem regressão no que deu para comparar.** Nada piorou além da variação natural da "
            "máquina — mas parte das métricas obrigatórias ficou de fora, e o veredito não fala "
            "por elas.",
            "nenhuma regressão acima do ruído entre as séries comparadas; parte das obrigatórias "
            "ficou de fora.",
            2, cegas, novas, fora,
        )
    if novas:
        return (
            "**Sem regressão.** Nenhuma métrica obrigatória piorou além da variação natural da "
            "máquina — e " + ", ".join(f"`{n}`" for n in novas)
            + " é nova neste PR, então não havia base para ela.",
            "nenhuma regressão acima do ruído; métrica nova sem base ainda.",
            0, cegas, novas, fora,
        )
    return (
        "**Sem regressão.** Nenhuma métrica obrigatória piorou além da variação natural da "
        "máquina.",
        "nenhuma regressão acima do ruído.",
        0, cegas, novas, fora,
    )


# --------------------------------------------------------------------------- escrita


def num(v):
    if abs(v) >= 1000:
        return f"{v:,.0f}".replace(",", " ")
    return f"{v:.2f}"


# Os cinco campos da carga eram copiados à mão aqui, com um comentário admitindo que eram "os
# MESMOS cinco" de `comparador.cpp`. Agora eles chegam no bloco `contrato` do JSON, escritos por
# `bench/contrato.hpp` — a mesma lista que o comparador em C++ confere. A constante abaixo é só o
# que se usa quando o JSON é antigo demais para trazê-la, e nesse caso o relatório o diz.
CAMPOS_CARGA_ANTIGOS = ("dias", "negocios_por_dia", "investidores", "particoes", "semente")


def campos_carga_de(doc):
    """Os campos que definem a carga, ditos pelo JSON; None quando ele não os declara."""
    c = doc.get("contrato")
    if isinstance(c, dict) and c.get("campos_carga"):
        return list(c["campos_carga"])
    return None


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
    campos = campos_carga_de(doc_a) or campos_carga_de(doc_b) or CAMPOS_CARGA_ANTIGOS
    difs = [f"{c} ({b[c]} na base, {a[c]} neste)" for c in campos
            if c in a and c in b and a[c] != b[c]]
    return ", ".join(difs) if difs else None


def contrato_de(doc):
    """As métricas de baseline do contrato, com o rótulo de cada uma em português corrente.

    Vem do bloco `contrato`, que o harness emite de `bench/contrato.hpp`: no schema 2 ele é um
    objeto com `metricas` e `campos_carga`; a primeira versão do bloco era a lista de métricas
    direto, e continua sendo lida. JSON sem o bloco (schema 1) devolve lista vazia — e quem chama
    RECUSA dar veredito verde nesse caso, em vez de concluir que nada era obrigatório.
    """
    c = doc.get("contrato")
    metricas = c.get("metricas", []) if isinstance(c, dict) else (c or [])
    return [m for m in metricas if m.get("serie")]


def bloqueantes_de(doc):
    """As séries que REPROVAM um PR, com o rótulo de cada uma (`contrato.bloqueantes`, schema 3).

    Não é a mesma lista das métricas de baseline, e a diferença é o ponto: enquanto era, só duas
    das dezoito séries podiam reprovar, e uma piora de 195 % em `snapshot.carrega.duracao_ms`
    saía como "informativa, não vota". Quais séries entram na lista é decisão MEDIDA — ver
    bench/contrato.hpp e a campanha em bench/README.md.

    JSON anterior ao schema 3 não declara a lista; aí valem as métricas de baseline, que era o
    conjunto bloqueante da versão que escreveu aquele arquivo.
    """
    c = doc.get("contrato")
    if isinstance(c, dict) and c.get("bloqueantes"):
        return [b for b in c["bloqueantes"] if b.get("serie")]
    return contrato_de(doc)


def linha_resumo(e, l, unidade, motivo):
    """Uma métrica contratual em três colunas: o que se mediu, e como ficou contra a base.

    `motivo` é o que o veredito registrou — e não um palpite. Antes esta função olhava só o
    `estavel` do lado da cabeça e, quando o descartado era a BASE, imprimia "sem base": afirmação
    falsa (a base existia) e incompatível com a frase do próprio resumo três linhas acima.

    E a palavra "igual" saiu: com um limiar de 26 %, um Δ de −26,1 % era impresso como
    "igual, dentro do ruído". Não é igual — é indistinguível com a precisão desta medição, que é
    outra coisa, e o número que separa as duas vai junto.
    """
    if e is None or not e["medida"]:
        return "não medida nesta execução", (motivo or "—")
    valor = f"{num(e['valor'])} {unidade}"
    if l is None or motivo:
        return valor, (motivo or "não comparada")
    if l["regressao"]:
        return valor, f"**{l['delta']:+.1f}% — pior**"
    if l["ganho"]:
        return valor, f"{l['delta']:+.1f}% — melhor"
    return valor, f"{l['delta']:+.1f}% — dentro do ruído (exigido {l['exigido']:.1f}%)"




def secao_resumo(doc, cabeca, linhas, fora, tem_base, frase, cegas, saida):
    """As primeiras linhas do relatório: passou ou não, e os poucos números que decidem isso.

    Existe porque o resumo de um job do GitHub é lido por quem quer uma resposta, não por quem vai
    auditar 24 séries. O detalhe continua embaixo, inteiro; o que muda é que ninguém precisa
    reconstruir o veredito lendo uma tabela de medianas.
    """
    # A tabela do resumo mostra primeiro as séries que DECIDEM o veredito, e depois as métricas de
    # baseline que não decidem — marcadas como informativas. As duas listas são pequenas e dizem
    # coisas diferentes: uma é o que reprovou ou não reprovou o PR, a outra é o que vai virar
    # baseline. Omitir a segunda faria o resumo esconder metade do contrato; misturá-las sem marca
    # faria o leitor achar que a segunda vota.
    bloqueia = {b["serie"] for b in bloqueantes_de(doc)}
    contrato = list(bloqueantes_de(doc))
    contrato += [m for m in contrato_de(doc) if m["serie"] not in bloqueia]
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
            valor, veredito = linha_resumo(
                e, por_nome.get(c["serie"]), unidade, fora.get(c["serie"])
            )
            if c["serie"] not in bloqueia:
                veredito += " · informativa, não vota"
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
    # Só quando HOUVE comparação. Sem base não existe veredito para a série não falar por ele, nem
    # job para reprovar: a frase saía numa medição avulsa dizendo "o job reprova por isso" logo
    # abaixo da tabela onde a série aparece medida e sadia. Aviso que descreve uma situação que não
    # é a do documento é a mesma classe de defeito que este relatório existe para não cometer.
    if cegas and tem_base:
        saida.append(
            "\n> **Atenção:** "
            + ", ".join(f"`{n}`" for n in cegas)
            + " não decidiu nada nesta comparação. O veredito acima não fala por ela, e o job "
            "reprova por isso."
        )
    if not doc["ambiente"].get("valido_para_baseline", True):
        saida.append(
            f"\n> **Estes números não valem como baseline:** "
            f"{doc['ambiente'].get('por_que_invalido', '?')}."
        )
    saida.append("")


def secao_ambiente(doc, saida):
    # `.get` nos três: um JSON sem `harness` derrubava o relatório inteiro com KeyError, e o
    # documento que falta um bloco é justamente o que mais precisa ser lido.
    amb = doc.get("ambiente") or {}
    carga = doc.get("carga") or {}
    h = doc.get("harness") or {}
    saida.append("\n## Ambiente e método\n")
    saida.append("| | |")
    saida.append("|---|---|")
    saida.append(f"| commit | `{amb.get('commit', '?')}` |")
    saida.append(f"| máquina | {amb.get('maquina', '?')} |")
    saida.append(f"| kernel | {amb.get('kernel', '?')} |")
    saida.append(f"| flags | `{amb.get('flags', '?')}` |")
    saida.append(f"| WAL | {amb.get('dispositivo_wal', '?')} |")
    if carga.get("eventos"):
        saida.append(
            f"| carga | {carga['dias']} pregão(ões), {carga['negocios_por_dia']} negócios/dia, "
            f"{carga['investidores']} investidores, {carga['particoes']} partição(ões), "
            f"semente {carga['semente']} → **{carga['eventos']} eventos** |"
        )
    if h:
        saida.append(
            f"| harness | {h['aquecimento']} aquecimentos, {h['repeticoes']} repetições, "
            f"descarte acima de {h['limiar_cv_pct']:.0f}% de CV, até {h['tentativas']} tentativas |"
        )
    else:
        saida.append("| harness | o documento não declara o bloco `harness` |")
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
            saida.append("| série | valor | unidade | dispersão | erro da mediana | |")
            saida.append("|---|---:|---|---:|---:|---|")
        if not e["medida"]:
            saida.append(
                f"| `{nome}` | — | {e['unidade']} | — | — | pulada: {'; '.join(e['notas'])} |"
            )
            continue
        # Duas colunas porque são duas perguntas: a dispersão descreve quanto a série balança; o
        # erro da mediana é o que o gate compara contra os 3σ. Mostrar só a primeira foi o que fez
        # ninguém notar que o portão exigia 56 % para declarar uma queda de 47 %.
        estado = "" if e["estavel"] else "**instável — não vira baseline**"
        cv = (
            f"{e['cv']:.2f}%"
            if e.get("execucoes", 1) < 2
            else f"{e['cv']:.2f}% ({e['cv_intra_max']:.1f} intra + {e['cv_entre']:.1f} entre)"
        )
        erro = f"±{e['erro_padrao']:.2f}%"
        saida.append(
            f"| `{nome}` | {num(e['valor'])} | {e['unidade']} | {cv} | {erro} | {estado} |"
        )
        if e.get("quantis"):
            q = e["quantis"]
            saida.append(
                f"| ↳ quantis (ns) | p50 {q['p50']} · p99 {q['p99']} · p999 {q['p999']} · "
                f"máx {q['max']} | | | | |"
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


def secao_comparacao(linhas, nao_comparadas, exigidas, limiar_pct, sigmas, saida):
    saida.append("\n## Comparação com a base (mesmo runner, mesma execução)\n")
    saida.append(
        f"Regressão exige passar dos dois testes: **{limiar_pct:.0f}%** (limiar do projeto) "
        f"E **{sigmas:.0f}× o ruído** medido das duas séries. A coluna *exigido* mostra qual "
        "dos dois mandou em cada linha.\n"
    )
    saida.append(
        "Só as séries **do contrato** decidem o veredito; as outras são informação. Medido em 15 "
        "rodadas de PR inocente com os parâmetros do job, deixar todas votarem dava 1 vermelho "
        "falso; só as contratuais, nenhum."
        f" E acima de **{TETO_EXIGIDO_PCT:.0f}%** de exigido a linha não decide nada: um limiar "
        "maior que a regressão de 2× que este gate promete pegar não é limiar, é vista grossa.\n"
    )
    if linhas:
        saida.append("| série | base | este PR | Δ | ruído | exigido | |")
        saida.append("|---|---:|---:|---:|---:|---:|---|")
        for l in sorted(linhas, key=lambda x: (not x["regressao"], x["nome"])):
            if not l["decide"]:
                marca = f"não decide (exigido > {TETO_EXIGIDO_PCT:.0f}%)"
            elif l["regressao"]:
                marca = "**REGRESSÃO**"
            elif l["ganho"]:
                marca = "ganho"
            else:
                marca = "ok"
            if l["nome"] not in exigidas and marca not in ("ok",):
                marca += " · informativa, não vota"
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
        "--apendice",
        action="store_true",
        help="documento para ser concatenado embaixo de outro: sem seção de resumo e com o "
        "título em nível 2 (o relatório informativo do WAL, no workflow)",
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
    # As séries que decidem vêm do JSON, não daqui. Ver bloqueantes_de().
    bloqueantes = bloqueantes_de(docs[0])
    exigidas = a.exigir if a.exigir is not None else [b["serie"] for b in bloqueantes]
    # Lista de exigidas VAZIA não é "nada a reprovar": é "não há o que exigir", e nesse estado o
    # veredito não pode sair verde. Vale para as duas maneiras de chegar nele — JSON sem o bloco
    # `contrato`, e `--exigir` passada sem valores (que `--exigir $VAR` com a variável vazia
    # produz sem ninguém perceber). Com R3 aplicada, este estado significava que NADA podia
    # reprovar: uma regressão real de 2× saía `rc=0`, "Sem regressão".
    #
    # Só vale quando há comparação: sem `--contra` não existe veredito para falhar aberto, e o
    # resumo já diz, na tabela, quando o JSON não declara contrato. `--apendice` não dá veredito.
    sem_exigencia = not exigidas and bool(a.contra) and not a.apendice

    # A comparação é calculada ANTES de escrever qualquer coisa: o resumo abre o relatório e
    # precisa do veredito que antes só existia no meio dele.
    linhas, nao_comparadas, docs_base = [], [], None
    carga_ruim = None
    if a.contra:
        docs_base = carrega(a.contra)
        carga_ruim = carga_divergente(docs[0], docs_base[0])
    if a.contra and not carga_ruim:
        linhas, nao_comparadas = compara(cabeca, consolida(docs_base), limiar, a.sigmas)

    # Frases e código de saída saem da MESMA função: não existe mais relatório dizendo "não deu
    # para dizer" e processo devolvendo 0, nem resumo dizendo "sem veredito" e job anunciando
    # regressão.
    frase_resumo, frase_detalhe, rc, cegas, novas, fora = veredito_de(
        linhas, nao_comparadas, exigidas, bool(a.contra), carga_ruim, sem_exigencia
    )

    saida = []
    if a.apendice:
        # Apêndice: entra concatenado embaixo de outro relatório (o do WAL, no workflow). Um
        # segundo "## Resumo" no mesmo resumo de job dava dois vereditos, e o de baixo listava
        # como "não medida" métricas que aquela execução nem tenta medir.
        saida.append(f"## {a.titulo}\n")
    else:
        saida.append(f"# {a.titulo}\n")
        secao_resumo(docs[0], cabeca, linhas, fora, bool(a.contra), frase_resumo, cegas, saida)
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
        if cegas:
            saida.append(
                "\n> **Atenção:** a comparação NÃO decidiu sobre "
                + ", ".join(f"`{n}` ({fora[n]})" for n in cegas)
                + ". O veredito acima vale para o resto, não para ela — e o job reprova.\n"
            )
        secao_comparacao(linhas, nao_comparadas, exigidas, limiar, a.sigmas, saida)

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

    # 0 = sem regressão; 1 = regressão; 2 = não deu para olhar. Vem de `veredito_de`, que é
    # também quem escreve as duas frases — um lugar só, para que o texto e o código nunca mais
    # discordem. O 2 REPROVA o job desde a devolução do verificador: um gate cego que avisa é um
    # gate que se aprende a ignorar.
    return rc


if __name__ == "__main__":
    sys.exit(main())
