#!/usr/bin/env python3
"""Gera data/calendario-b3-<ano>.csv.

Por que um script e não uma constante no código: feriado é dado que muda por lei e por decisão da
B3, e a data de liquidação de um negócio precisa ser um *campo do evento* — se o motor calculasse
D+2 lendo um calendário durante o `apply`, o replay leria arquivo externo e violaria I12.
O calendário vive fora do motor, o ingress o consulta uma vez, e o log guarda o resultado.
"""
import csv
import sys
from datetime import date, timedelta


def pascoa(ano: int) -> date:
    """Domingo de Páscoa pelo algoritmo de Meeus/Jones/Butcher (calendário gregoriano)."""
    a, b, c = ano % 19, ano // 100, ano % 100
    d, e = b // 4, b % 4
    f, g = (b + 8) // 25, (b - (b + 8) // 25 + 1) // 3
    h = (19 * a + b - d - g + 15) % 30
    i, k = c // 4, c % 4
    l = (32 + 2 * e + 2 * i - h - k) % 7
    m = (a + 11 * h + 22 * l) // 451
    return date(ano, (h + l - 7 * m + 114) // 31, ((h + l - 7 * m + 114) % 31) + 1)


def feriados(ano: int) -> dict[date, tuple[str, str, str]]:
    p = pascoa(ano)
    return {
        date(ano, 1, 1):        ("Confraternização Universal",   "nacional",    "fechado"),
        p - timedelta(48):      ("Carnaval (segunda)",           "nacional",    "fechado"),
        p - timedelta(47):      ("Carnaval (terça)",             "nacional",    "fechado"),
        p - timedelta(46):      ("Quarta-feira de Cinzas",       "b3",          "abertura às 13h"),
        p - timedelta(2):       ("Sexta-feira Santa",            "nacional",    "fechado"),
        date(ano, 4, 21):       ("Tiradentes",                   "nacional",    "fechado"),
        date(ano, 5, 1):        ("Dia do Trabalho",              "nacional",    "fechado"),
        p + timedelta(60):      ("Corpus Christi",               "nacional",    "fechado"),
        date(ano, 7, 9):        ("Revolução Constitucionalista", "estadual-SP", "fechado"),
        date(ano, 9, 7):        ("Independência",                "nacional",    "fechado"),
        date(ano, 10, 12):      ("N. Sra. Aparecida",            "nacional",    "fechado"),
        date(ano, 11, 2):       ("Finados",                      "nacional",    "fechado"),
        date(ano, 11, 15):      ("Proclamação da República",     "nacional",    "fechado"),
        date(ano, 11, 20):      ("Consciência Negra",            "nacional",    "fechado"),
        date(ano, 12, 24):      ("Véspera de Natal",             "b3",          "fechado"),
        date(ano, 12, 25):      ("Natal",                        "nacional",    "fechado"),
        date(ano, 12, 31):      ("Véspera de Ano-Novo",          "b3",          "fechado"),
    }


def main(ano: int, saida: str) -> None:
    # O D+2 de dezembro cai no ano seguinte: o mapa cobre dois anos.
    fer = feriados(ano) | feriados(ano + 1)
    pregao = lambda d: d.weekday() < 5 and fer.get(d, ("", "", ""))[2] != "fechado"

    def mais_dois(d: date) -> date:
        k, c = d, 0
        while c < 2:
            k += timedelta(1)
            if pregao(k):
                c += 1
        return k

    dias = 0
    with open(saida, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["data", "dia_semana", "pregao", "liquidacao_d2", "feriado", "tipo", "observacao"])
        d = date(ano, 1, 1)
        while d <= date(ano, 12, 31):
            eh = pregao(d)
            dias += eh
            nome, tipo, obs = fer.get(d, ("", "", ""))
            w.writerow([d.strftime("%Y%m%d"), "SEG TER QUA QUI SEX SAB DOM".split()[d.weekday()],
                        int(eh), mais_dois(d).strftime("%Y%m%d") if eh else "", nome, tipo, obs])
            d += timedelta(1)
    print(f"{saida}: {dias} pregões em {ano}")


if __name__ == "__main__":
    ano = int(sys.argv[1]) if len(sys.argv) > 1 else 2026
    main(ano, sys.argv[2] if len(sys.argv) > 2 else f"data/calendario-b3-{ano}.csv")
