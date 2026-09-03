# 14 — Reconciliação com divergência: marca o snapshot, não bloqueia o EOD

Exerce I1 e a regra de `docs/dominio.md`: "divergência vai para fila de exceção e marca o snapshot;
não bloqueia o EOD".

## Contexto

Fim do dia 20260910. A partição calcula, para a conta A e o instrumento 1:

    I1 = disponivel + Σ a_liquidar_venda + bloqueado + sobras = 187 + 0 + 0 + 0 = 187 ações

O arquivo de posição da depositária diz **185**.

## O que acontece

    ReconciliacaoDepositaria{ data=20260910,
                              divergencias=[ {conta=A, instrumento=1, motor=187, depositaria=185} ] }

1. O evento entra no log como qualquer outro — a divergência é fato, e fato vai para o log.
2. O ledger **não é ajustado**. O motor não "corrige" a posição para 185. Corrigir apagaria a
   evidência e faria o replay reproduzir a correção como se fosse verdade original.
3. A divergência entra na fila de exceção da partição, que é estado e vai para o snapshot.
4. O cabeçalho do snapshot de exposição recebe a **flag de divergência** (`docs/wal.md`, layout do
   cabeçalho de 4 KiB).
5. O EOD segue: `EodMark{20260910}`, commit forçado, snapshot em `eod_lsn`.

## Por que não bloquear o EOD

Bloquear o fechamento por uma divergência de duas ações em uma conta pararia a exposição Open
Finance de **todas** as contas da partição. O regulador exige dado de D-1 (R14); ficar sem snapshot
é indisponibilidade medida (R18) e violação de SLA (R17). A divergência é um problema de duas
ações; parar o EOD é um problema de todo mundo.

A escolha oposta — bloquear — seria defensável se o motor fosse a fonte de verdade contábil da
custódia. Ele não é: a depositária é. O motor é a fonte de verdade do **que aconteceu no motor**, e
é isso que ele publica, marcado.

## O que a borda faz com a flag

A flag do cabeçalho não muda o corpo das respostas — os números publicados são os do motor. Ela
alimenta a trilha de auditoria (R19) e a métrica operacional. A decisão de suspender a exposição de
uma conta específica é operacional, tomada fora do motor, e entra como evento se for tomada.

## Teste

| Afirmação |
|---|
| depois da reconciliação com divergência, `disponivel` continua 187 (o ledger não se move) |
| a fila de exceção contém exatamente uma entrada, com os dois números |
| `EodMark` e o snapshot acontecem mesmo assim |
| o cabeçalho do snapshot tem a flag ligada |
| recuperar do snapshot reproduz a fila de exceção idêntica (I11) |
| reconciliação **sem** divergência não liga a flag e não cria entrada |

A última linha é a que impede o alarme de virar ruído: um motor que marcasse todo snapshot com a
flag "por segurança" seria indistinguível de um motor sem a flag.
