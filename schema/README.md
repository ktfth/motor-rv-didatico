# schema/ — SBE

Donos: `dominio-pos-negociacao` (semântica dos campos) e `nucleo` (layout, versionamento).
Um arquivo `*.xml` por família de mensagem; codecs C++ gerados por `sbe-tool` no build,
nunca versionados nem editados à mão. Convenções: quantidades e preços `int64` escala 1e-8;
BRL `int64` escala 1e-4; datas como `uint32` AAAAMMDD; identificadores internados `uint32`.
Toda mudança incompatível é `schemaId`/`version` novo e teste de leitura da versão anterior.
