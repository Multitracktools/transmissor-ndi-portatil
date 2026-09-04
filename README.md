# Transmissor NDI Portátil

Protótipo de um transmissor portátil de tela via NDI para Windows. O objetivo é
permitir que uma pessoa compartilhe um monitor na rede local sem instalar o
pacote completo do NDI Tools na máquina de origem.

## Estado atual

Versão inicial para validação técnica:

- Windows 10 ou 11, 64 bits;
- captura de um monitor completo;
- transmissão NDI High Bandwidth em 30 FPS;
- nome configurável para a fonte;
- cursor do mouse opcional;
- execução portátil com a biblioteca redistribuível ao lado do executável;
- log de diagnóstico salvo em `transmissor-ndi.log`.

Ainda não inclui áudio, captura de janela, ajuste de resolução ou versões para
macOS e Linux.

## Compilação automática

O workflow **Compilar para Windows** gera um ZIP em **Actions → Artifacts**. A
compilação usa as interfaces públicas do NDI SDK e obtém o redistribuível pelo
endereço oficial durante a execução do workflow.

O uso das interfaces e do runtime do NDI está sujeito à licença do NDI SDK. O
projeto não inclui o instalador do SDK nem o pacote NDI Tools.

## Teste

Consulte `README-TESTE.txt` antes de executar o protótipo.

## Desenvolvimento local

Requisitos:

- Visual Studio 2022 com o workload **Desktop development with C++**;
- CMake 3.24 ou superior;
- `Processing.NDI.Lib.x64.dll` obtida do redistribuível oficial do NDI e
  colocada ao lado do executável para rodar.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

## Licença

Nenhuma licença foi definida para o código do projeto nesta fase de protótipo.
Os avisos aplicáveis aos componentes de terceiros estão em
`THIRD_PARTY_NOTICES.md`.

