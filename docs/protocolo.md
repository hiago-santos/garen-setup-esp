# Protocolo da porta SETUP

A central Garen fala **TuyaMCU**, o protocolo serial que os módulos Wi-Fi da Tuya usam para conversar com o microcontrolador do aparelho. Nessa conversa:

- a **central** é o "MCU" (responde com versão `0x03` no quadro);
- quem se conecta na porta SETUP (o Garen Setup, o Garen Config ou o seu ESP32) é o "**módulo**" (envia com versão `0x00`).

Referência do protocolo: documentação pública da Tuya, "Serial communication protocol" do MCU SDK.

## Camada física

| Parâmetro | Valor |
|---|---|
| Velocidade | **9600 baud** |
| Formato | 8N1 |
| Nível lógico | 3,3V (o MCU da central é um STM32) |
| Quem começa | O módulo. A central fica calada até receber algo |

## Formato do quadro

```
55 AA | ver | cmd | len_hi len_lo | dados... | checksum
```

- `ver`: `00` do módulo para a central, `03` da central para o módulo.
- `checksum`: soma de todos os bytes anteriores, módulo 256.

## Comandos observados

| Cmd | Nome | Módulo envia | Central responde |
|---|---|---|---|
| `00` | Heartbeat | `55 AA 00 00 00 00 FF` | `55 AA 03 00 00 01 00 03` no primeiro após ligar, depois `... 01 01 04` |
| `01` | Info do produto | `55 AA 00 01 00 00 00` | JSON `{"p":"<PID>","v":"1.0.3","m":0}`. O PID depende do modo (ver abaixo) |
| `02` | Modo de trabalho | `55 AA 00 02 00 00 01` | `55 AA 03 02 00 00 04` (vazio: o módulo cuida do LED e do botão) |
| `03` | Status de rede | `55 AA 00 03 00 01 <st> <cs>` | `55 AA 03 03 00 00 05` (confirmação) |
| `06` | Comando de DP | ver abaixo | Relatório `07` com o novo valor (~50 ms) |
| `07` | Relatório de DP | | Enviado pela central quando algo muda, e no "despejo" de status |
| `08` | Consultar status | `55 AA 00 08 00 00 07` | **Ignorado** (ver abaixo) |

Status de rede (`<st>` no comando `03`): `02` = configurado sem conexão, `03` = conectado ao roteador, `04` = conectado à nuvem.

### Peculiaridade: como ler o status

No TuyaMCU padrão, o comando `08` faz o MCU enviar todos os datapoints. **A Fit Connect ignora o `08`**: testamos várias vezes e ela nunca respondeu.

O que funciona: **informar uma "reconexão" à nuvem**. Quando o status de rede passa de outro valor para `04`, a central envia todos os DPs em menos de 100 ms:

```
TX 55 AA 00 03 00 01 03 06      ← "conectado só ao roteador"
TX 55 AA 00 03 00 01 04 07      ← "conectado à nuvem"
RX 55 AA 03 07 00 08 71 02 00 04 00 00 00 68 F0   DP 113 = 104
RX 55 AA 03 07 00 05 03 01 00 01 01 14            DP 3   = true
RX ... (todos os outros DPs)
```

Enviar `04` repetido, sem passar por outro valor antes, **não** provoca o envio. Repetimos o par `03`→`04` a cada 1 s por mais de uma hora, somando os testes, sem nenhum efeito colateral visível.

A central também envia por conta própria os DPs de **estado** (1, 3, 101, 120) assim que eles mudam, sem precisar de consulta.

## Modo Usuário × Instalador

A Fit Connect tem dois modos, trocados pela placa: **feche os jumpers FORÇA e RAMPA AB e segure TX (−) por 5 s**. O LED pisca **1x = Instalador** ou **2x = Usuário**. Cada vez que você faz isso, o modo alterna. Abra os jumpers depois: com eles fechados, o motor não funciona.

| | Usuário | Instalador |
|---|---|---|
| PID informado no comando `01` | `l9era1stzldt1ybu` | `cpk9ndmiftjvwhwz` |
| Nome no app G.Smart 4.0 | "Automatizador Garen Usuário" | "Automatizador Do Instalador" (tem a tela SETUP e TX/CURSO) |
| Ler estado e parâmetros | ✅ | ✅ |
| Acionar (DP 1) | ✅ | não testado pela serial (o controle funciona) |
| Gravar controle (DP 119) | menu abre; confirmação não testada | ✅ |
| Gravar curso (DP 120) | ✅ | não testado pela serial (pela placa funciona) |
| Escrever parâmetros | ❌ a central responde com o valor **atual** (recusa) | ✅ |

O modo **não aparece em nenhum DP**, só no PID. Ele fica gravado: continua o mesmo depois de desligar e religar a central.

## Datapoints (Fit Connect)

Tipos Tuya: `1` = bool (1 byte), `2` = value (inteiro de 4 bytes, big-endian), `4` = enum (1 byte).

| DP | Tipo | Significado | Faixa / passo | Valor de fábrica | Como confirmamos |
|---|---|---|---|---|---|
| **1** | bool | **Escrita:** `true` = acionar (igual a apertar o controle: liga se parado, para se em movimento). **Leitura:** motor ligado | | | Comandos pelo PC + observação do relé |
| **3** | bool | Portão **não** está fechado (`false` = fechado) | | | Fim de curso simulado (FCF) |
| **101** | bool | Liga junto com o motor (luz de cortesia ou relé auxiliar) | | | Sempre acompanhou o DP 1 |
| **108** | value | **PAUSA** (s) antes do fechamento automático. `0` = desligado | 0–120 pela placa (0–105 pela serial), passo 5 | 0 | Botões da placa + escrita |
| **110** | value | **FORÇA** (%) | 5–100, passo 5 (níveis 0–20 no manual) | 100 | Botões da placa + escrita |
| **111** | value | **RAMPA FC** (fechamento) | 0–15, passo 1 | 7 | Botões da placa + escrita |
| **113** | value | Desconhecido | | (104 na nossa) | Não mudou em nenhum teste. **Não escreva** |
| **117** | value | **RAMPA AB** (abertura) | 0–15, passo 1 | 7 | Botões da placa + escrita |
| **118** | value | **FREIO** (%) | 0–100, passo 5 (níveis 0–20 no manual; 0 = desligado) | 20 (= nível 5) | Botões da placa + escrita |
| **119** | enum | **Menu TX** (gravar controle). Ver abaixo | | | Gravação de controles pela serial |
| **120** | enum | **Gravação de curso.** Escrever `1` inicia a gravação **automática**. A central reporta `1` durante a gravação e `0` ao terminar | | | Gravação pela placa e pela serial; só é enviado quando muda |
| **123** | bool | **SALVAR**: `true` grava os parâmetros na memória | | | Escrita + desligar e religar |
| **151** | value | Número de controles remotos cadastrados | | | Mudou ao gravar e ao apagar controles |

O manual da Fit Connect mostra força e freio em **níveis de 0 a 20**. Os DPs usam **porcentagem** (nível × 5): o freio de fábrica, nível 5, aparece como **20**.

### Acionar o portão

```
55 AA 00 06 00 05 | 01 01 00 01 01 | 0E
                    │  │  └──┘  └ valor true
                    │  │   len
                    │  └ tipo bool
                    └ DP 1
```

A central responde em ~50 ms:

```
RX 55 AA 03 07 00 05 65 01 00 01 01 76   DP 101 = true
RX 55 AA 03 07 00 05 01 01 00 01 01 12   DP 1   = true  (motor ligou)
```

Enviar `DP 1 = true` de novo com o motor ligado **para** o portão. **Não testado:** o que acontece ao enviar `DP 1 = false`.

### Escrever um parâmetro

**Só funciona no modo Instalador.** O parâmetro é um DP do tipo value (4 bytes). Exemplo, pausa = 30 s:

```
55 AA 00 06 00 08 | 6C 02 00 04 00 00 00 1E | 9D
```

A central **não copia o valor recebido**. Ela se comporta como os botões + e − da placa: anda em passos (5 para pausa, força e freio; 1 para as rampas) **na direção do valor enviado**:

- **Descendo**, ela para no valor enviado (ou no passo logo abaixo dele).
- **Subindo**, ela para no **primeiro passo acima** do valor enviado. Por isso, um múltiplo exato do passo passa um passo além: enviar 30 com a pausa em 0 resulta em **35**.
- **Valores acima de 100 são ignorados em silêncio.** Por isso a pausa não passa de **105** pela serial, mesmo que pela placa vá até 120.

O app original manda **valor atual ± 1** a cada toque, o que vira exatamente um passo. Para ir direto a um valor:

| Situação | Enviar |
|---|---|
| Alvo **maior** que o atual, passo 5 | **alvo − 1** (ex.: 29 para chegar em 30) |
| Alvo **menor** que o atual | **alvo** |
| Rampas (passo 1) | **alvo** |

Confira o eco e repita se não bater. Em dois casos isolados, o primeiro "−1" de força/freio não teve efeito, e a partir de 120 a pausa desceu um passo a menos. Uma segunda escrita corrigiu os dois.

A cada escrita, o **LED de programação** reage como nos botões: pisca, e **fica aceso** se o parâmetro chegou ao mínimo ou ao máximo.

### Salvar (DP 123)

A escrita vale **na hora**, mas **se perde ao desligar** a central. Para gravar:

```
55 AA 00 06 00 05 | 7B 01 00 01 01 | 88        DP 123 = true
RX  DP 151 = <nº de controles>                 (a central responde reenviando o DP 151)
```

O LED de programação **pisca 2x**. Testado: pausa 45 e rampa AB 9, depois salvar, desligar e religar: os valores se mantiveram. Sem o salvar, a central volta ao último valor gravado. A central **não** salva sozinha (testamos 5 minutos conectada), e o DP 119 não salva.

### Gravar controle remoto (DP 119)

| Valor | Quem manda | Significado |
|---|---|---|
| `1` | módulo | **Abre o menu TX** (LED aceso, "pressione o controle") |
| `2` | central | **Controle detectado**, aguardando confirmação (LED piscando) |
| `3` | módulo | **Confirma a gravação** do controle detectado. O DP 151 sobe |
| `4` | módulo | Aceito (a central ecoa 4 e volta a 1), mas **sem efeito visível**. Desconhecido |
| `5` | central | O controle apertado **já está cadastrado** (LED pisca 2x) |
| `6` | módulo | **Apaga TODOS os controles** (com o menu aberto). O DP 151 vai a 0 ~2,3 s depois |
| `0` | central | Menu fechou (~10 s sem atividade) |

Fluxo seguro, testado gravando 2 controles:

```
TX  55 AA 00 06 00 05 77 04 00 01 01 87     DP 119 = 1   (abrir menu)
    ... o usuário aperta o controle ...
RX  DP 119 = 2                              (detectado)
TX  55 AA 00 06 00 05 77 04 00 01 03 89     DP 119 = 3   (confirmar)
RX  DP 151 = n+1, DP 119 = 3 → 1 → (10 s) → 0
```

> ⚠️ **O `3` grava o último código de rádio que a central recebeu, mesmo que ninguém tenha apertado nada.** Nos testes, ele gravou duas vezes um código desconhecido (provavelmente um controle da vizinhança). **Só mande o `3` depois que a central reportar `2`.**

Enviar `2` não faz nada: é um status da central.

**Apagar todos os controles** (equivale a segurar o TX da placa por 5 s). Testado no modo Usuário:

```
TX  55 AA 00 06 00 05 77 04 00 01 01 87     DP 119 = 1   (abrir menu)
TX  55 AA 00 06 00 05 77 04 00 01 06 8C     DP 119 = 6   (apagar)
RX  DP 119 = 6 … ~2,3 s … DP 151 = 0, DP 119 = 1
```

## O que a central **não** informa

- **Totalmente aberto × parado no meio.** O fim de curso de abertura (FCA) para o motor, mas não altera nenhum DP. O DP 3 fica `true` igual em "totalmente aberto", "parado no meio" e "parou por tempo".
- **Sentido do movimento** (abrindo ou fechando).
- **Origem do acionamento:** controle, botoeira e serial geram o mesmo relatório.
- **Jumpers de hardware:** T. FREIO e BV/DZ não aparecem em nenhum DP.

### Gravar o curso (DP 120)

No firmware 1.0.3, a gravação de curso é **automática** (ver [hardware.md](hardware.md#gravação-de-curso-firmware-103-automática)). Pela serial, um único comando faz a mesma coisa que o CURSO (+) seguido de segurar o botão. Funcionou até no modo Usuário:

```
TX  55 AA 00 06 00 05 78 04 00 01 01 88      DP 120 = 1
RX  DP 120 = 1                               gravação iniciada
    ... a central move o portão sozinha: até um fim de curso,
        até o outro e de volta ao primeiro (DP 1 / DP 3 mudam a cada etapa) ...
RX  DP 151 = <n>, DP 120 = 0                 gravação concluída
```

> ⚠️ Em um portão instalado, **o portão se move sozinho** durante a gravação. Confirme que a área está livre antes de mandar o comando.

## DPs que respondem a comando

Varredura com valores neutros (bool `false` / enum `0`) em **2–160**, nos dois modos: só **1**, **119** e os parâmetros respondem. Varredura com `true` em 102–107, 109, 112, 114–116, 121–123: só o **123** (salvar). Não testamos `true` em 124 em diante, nem em 2–100.

## Outros comportamentos observados

- **Sem fim de curso**, o motor desliga sozinho depois de **~20 s**. Esse tempo não muda com o DP 118 (freio), então parece um limite de segurança fixo.
- **LED STATUS** (perto do conector SETUP): acende quando o módulo se apresenta como conectado (status de rede `04`).
- **LED de programação** (perto do MCU): reage às escritas pela serial como reage aos botões.
- Os parâmetros ajustados pelos **botões da placa** aparecem no DP em menos de 1 s.
- Pedidos que a central poderia fazer ao módulo (`2B` status de rede, `1C` hora local, `24` sinal Wi-Fi) **não apareceram** nos nossos testes. O firmware responde ao `2B` mesmo assim, por precaução.
