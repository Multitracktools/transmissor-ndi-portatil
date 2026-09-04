# Transmissor NDI® Portátil

Aplicativo da [Zosma Labs](https://zosma.com.br) para transmissão portátil de
tela via NDI no Windows. O objetivo é
permitir que uma pessoa compartilhe um monitor na rede local sem instalar o
pacote completo do NDI Tools na máquina de origem.

## Estado atual

Versão para validação técnica:

- Windows 10 ou 11, 64 bits;
- captura de monitor completo ou janela específica;
- transmissão NDI High Bandwidth em 30 FPS;
- nome configurável para a fonte;
- cursor do mouse opcional;
- modos rápido e protegido;
- tela de espera com liberação manual do primeiro receptor;
- contagem de receptores e bloqueio geral no modo protegido quando houver uma
  conexão adicional;
- proteção automática para WhatsApp, WhatsApp Business, WhatsApp Web, Telegram
  Desktop e Telegram Web;
- permissões temporárias que podem ser alteradas durante a transmissão;
- intensidade do Wi-Fi, tráfego total de saída da máquina e FPS medido;
- preferências básicas salvas no perfil do Windows;
- janela Como usar e link para `zosma.com.br`;
- execução portátil com a biblioteca redistribuível ao lado do executável;
- log de diagnóstico salvo em `transmissor-ndi.log`.

Ainda não inclui áudio, ajuste de resolução ou versões para macOS e Linux.

## Modos de transmissão

- **Protegido:** a fonte começa com uma tela de espera. A imagem real só é
  liberada após a confirmação do usuário. Se mais de um receptor for detectado,
  a imagem é bloqueada para todos e exige nova confirmação.
- **Rápido:** a imagem é enviada imediatamente. Conexões adicionais são
  contabilizadas, mas não bloqueiam o conteúdo.

O SDK NDI padrão informa a quantidade de conexões, mas não identifica de forma
confiável cada receptor. Por isso, o modo protegido bloqueia todos quando há uma
conexão adicional.

## Modo Privacidade

As permissões de WhatsApp e Telegram sempre começam desativadas. A detecção é
feita por janela/processo e, nos navegadores, pelo título da aba ativa. As
notificações do Windows não são ocultadas; recomenda-se ativar **Não incomodar**.

O valor em Mbps mostrado pelo aplicativo representa o tráfego total de saída
das interfaces de rede do computador, não apenas o NDI.

## Compilação automática

O workflow **Compilar para Windows** gera um artifact em **Actions → Artifacts**. A
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

Nenhuma licença foi definida para o código do projeto nesta fase de testes.
Os avisos aplicáveis aos componentes de terceiros estão em
`THIRD_PARTY_NOTICES.md`.
