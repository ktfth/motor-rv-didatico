# ADR-0019 — Servidor HTTP próprio sobre io_uring/epoll, no lugar de Asio/Beast

Status: aceito (02/09/2026). Ajusta ADR-0008 no ponto do Asio; mantém OpenSSL e io_uring.

## Contexto
ADR-0008 escolheu Asio/Beast para não escrever rede à mão. Asio não existe na máquina de referência,
e o perfil FAPI-BR já nos obriga a controlar o socket em detalhe: mTLS com verificação de cadeia
ICP-Brasil, *session resumption* e renegociação **desabilitadas**, cipher suites fixos, thumbprint
do certificado da conexão comparado ao `cnf.x5t#S256` do token (R3, R6). Isso é feito no `SSL*` do
OpenSSL de qualquer jeito; o Beast estaria por baixo do que realmente decide.

## Decisão
`src/edge/http` implementa um servidor HTTP/1.1 mínimo — o subconjunto que os seis endpoints da API
RV usam — sobre io_uring (com fallback epoll), com o TLS em OpenSSL 3 dirigido por BIO de memória.
Sem parsing genérico: método, alvo, versão e um conjunto fechado de cabeçalhos, tudo com limite de
tamanho aplicado antes de copiar qualquer byte.

## Alternativas consideradas
- Vendorizar Asio standalone: possível, mas o valor entregue seria o loop de eventos, que o
  io_uring da persistência já nos dá, e o Beast traria um parser HTTP grande demais para fuzzar.
- Terminar TLS em proxy externo: quebraria R3, que exige o certificado da conexão no mesmo processo
  que valida o token.

## Consequências
Escrevemos e mantemos um parser HTTP — logo ele **precisa** de harness de fuzz, e o limite de tempo
mínimo de fuzz fica definido em `tests/edge/README.md` antes do fim da fase 4. Em troca, o processo
que recebe tráfego externo é pequeno, auditável e não tem caminho de código que não usamos.
