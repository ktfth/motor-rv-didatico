# 10 — Dividendo e JCP com IRRF: o motor **verifica**, não recalcula

Exerce I6 e a regra geral "dinheiro decidido por terceiro entra como campo de evento".

## Contexto

Posição: 143 ações (fim do cenário 09).

## Dividendo — isento

    valor por ação: R$ 0,03714286   (proventos na B3 vêm com 8 casas)
    bruto = 143 × 0,03714286 = R$ 5,31143... 

A depositária credita em **centavos**: R$ 5,31. O evento carrega o número creditado:

    ProventoPago{ evento_id=910, tipo=DIVIDENDO, conta, instrumento=1,
                  valor_por_acao=3714286 (1e-8), qty_base=14300000000,
                  bruto=53100 (1e-4), irrf=0, liquido=53100 }

Efeito: `proventos_a_receber += 53100`. Quantidade e preço médio: **inalterados**.

## JCP — IRRF de 15 % na fonte

    valor por ação: R$ 0,08123456
    bruto  declarado: R$ 11,62     → 116200
    irrf   declarado: R$  1,74     →  17400     (15 % de 11,62 = 1,743 → centavo)
    liquido declarado: R$ 9,88     →  98800

Efeito: `proventos_a_receber += 98800`; o IRRF fica registrado no movimento para a apuração anual.

## O que o motor faz com esses números

Ele **não os recalcula**. Ele verifica três coisas e, na divergência, manda para a fila de exceção
sem alterar o ledger:

1. `bruto − irrf == liquido` — aritmética interna do próprio evento. Aqui: 116200 − 17400 = 98800 ✔
2. `|bruto − qty_base × valor_por_acao|` cabe em **um centavo** — o arredondamento que o pagador
   tem direito de fazer. Aqui: 143 × 0,08123456 = R$ 11,61654208; |11,62 − 11,6165| = R$ 0,0035 ✔
3. `qty_base` é a posição da conta na **data-com**, e não a de hoje.

**Por que não recalcular**: o valor creditado é decisão do emissor e da depositária, com regra de
arredondamento própria que muda por deliberação. Se o motor recalculasse, ele e o extrato do
investidor discordariam em centavos — todo mês, em milhões de contas — e a diferença apareceria
como divergência de reconciliação sem que ninguém tivesse errado. Verificar dentro de uma
tolerância declarada acha o erro de verdade (um provento no papel errado, uma posição-base errada)
e ignora o ruído que não é erro.

**Por que verificar mesmo assim**: sem a checagem 2, um provento com `qty_base` de outra conta
passaria direto. Ela é barata e é o único cruzamento que liga o dinheiro à posição.

## O que I4 diz aqui

Nem dividendo nem JCP tocam `preco_medio`. JCP **não** é devolução de capital: reduz o patrimônio
da companhia, não o custo da posição do investidor. O teste afirma `preco_medio` byte a byte igual
antes e depois dos dois eventos — é a forma mais direta de fixar essa regra contra o palpite de
que "JCP é como um retorno de capital".
