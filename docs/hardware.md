# Hardware

> ⚠️ A central tem **tensão de rede (127/220V)** na placa. Faça as ligações com ela desligada da tomada.

## Central Garen Fit Connect

- **MCU:** STMicroelectronics **STM32C031K6T6** (Cortex-M0+, 48 MHz, 32 KB de flash, LQFP32, 3,3V).
- A porta SETUP está ligada na **USART2** do MCU.

### Conector SETUP

A placa tem um **USB-A fêmea** e, em algumas versões, uma **barra de 5 pinos**. A serigrafia da barra é:

```
 –   –   RX   TX   5V
GND GND
```

| Pino | Função | MCU | Pino USB-A equivalente | Cor típica no cabo |
|---|---|---|---|---|
| – | GND | | 4 (GND) | preto |
| – | GND | | carcaça | preto/cinza |
| **TX** | **A central transmite** | **PA2** (USART2_TX, pino 9 do LQFP32) | 2 (D−) | branco |
| **RX** | **A central recebe** | **PA3** (USART2_RX, pino 10 do LQFP32) | 3 (D+) | verde |
| 5V | Alimentação para o módulo | | 1 (VBUS) | vermelho |

- A correspondência entre os pinos e o PA2/PA3 foi confirmada com **teste de continuidade** até o MCU.
- A correspondência entre a barra e os pinos do USB-A foi deduzida pelas cores do cabo original entre a barra e o conector, e não foi medida. Confira com um multímetro antes de confiar nela.
- **Não ligue nada de USB de verdade nessa porta** (PC, pendrive). Não é USB, é uma UART.

### Ligação

| Seu dispositivo | Central |
|---|---|
| RX | **TX** (PA2) |
| TX | **RX** (PA3) |
| GND | – |
| (alimentação, opcional) | 5V |

Se você não recebe **nenhum byte**, o motivo mais provável é **TX e RX trocados**. Foi exatamente o que aconteceu com a gente: com os fios invertidos, não houve resposta nenhuma em nenhuma velocidade.

### Gravação de curso (firmware 1.0.3): automática

Os manuais da Fit Connect (Rev 00, Rev 01/A01867, Rev 03 e A01586) descrevem a gravação com o **controle**: CURSO (+), LED pisca **5x**, o controle aciona fechar → abrir → fechar, e o LED pisca 3x no fim. **No firmware 1.0.3 não é assim:**

1. Aperte **CURSO (+)**. O LED pisca **3x**.
2. **Aperte e segure CURSO (+)** de novo.
3. A central move o portão **sozinha** até um fim de curso, depois até o outro, e volta ao primeiro. **Não use o controle.**

O tempo de curso serve para as **rampas** (desaceleração). Sem fim de curso, o motor para em **~20 s** de qualquer jeito, e esse limite não muda com o curso gravado. Seguindo o procedimento dos manuais (com o controle), a gravação nunca terminou.

### LEDs da placa

| LED | Onde | Significado |
|---|---|---|
| **LED STATUS** | perto do conector SETUP | Conexão com o módulo: apagado = desconectado, piscando = tentando, aceso = conectado (acende quando o módulo informa status de rede `04`) |
| **LED de programação** | perto do MCU e dos botões | Programação. Reage às escritas pela serial como aos botões: pisca a cada ajuste, fica aceso no limite do parâmetro, pisca 2x ao salvar (DP 123), fica aceso no menu TX e pisca quando um controle é detectado |
| LEDs FCA / FCF | perto dos conectores de fim de curso | Fim de curso de abertura (verde) / fechamento (vermelho) acionado |

### Alimentação pela porta SETUP

A Garen não publica a corrente disponível no pino de 5V. Pelo que o conector já alimenta, dá para estimar:

- o **Garen Setup** (módulo Wi-Fi Tuya WBR3), com picos de ~250–300 mA ao transmitir;
- o **Garen Config** (teclado com LCD 8x2 com luz de fundo).

Um **ESP32-C3** fica na mesma faixa do Setup com o Wi-Fi ligado (picos de ~300 mA na potência máxima), e em ~20–30 mA com o rádio desligado. Recomendações:

- Ligue o 5V no **5V/VIN** da placa do ESP, nunca no 3V3.
- Coloque um capacitor de **100–470 µF** entre 5V e GND, perto do ESP.
- Reduza a potência do Wi-Fi se puder (o firmware usa 11 dBm).
- Se for possível, confira qual regulador de 5V a central usa e meça o 5V com o ESP funcionando.
- Muitas placas ESP32-C3 (ex.: SuperMini) **não têm diodo** entre o USB e o 5V. Para gravar pelo USB, **desconecte o fio de 5V da central** e mantenha GND, TX e RX.

### Jumpers e entradas (manual da Fit Connect)

| Jumper/entrada | Aparece na serial? |
|---|---|
| PAUSA, FORÇA, FREIO, RAMPA AB, RAMPA FC (ajustados com os botões CURSO + / TX −) | ✅ DPs 108, 110, 118, 117, 111 |
| FORÇA + RAMPA AB fechados + TX por 5 s (modo Usuário/Instalador) | Só no PID (comando `01`) |
| T. FREIO (freio DC ou por reversão) | ❌ |
| BV/DZ (basculante ou deslizante) | ❌ |
| Botoeira | Só o efeito: o motor liga, DP 1. Não tem DP próprio |
| Fim de curso de fechamento (FCF) | ✅ DP 3 = false |
| Fim de curso de abertura (FCA) | ❌ (só para o motor) |

## Garen Setup (módulo original)

- **Módulo Wi-Fi/BLE:** etiqueta "…WPR3", quase certamente o **Tuya WBR3** (Realtek RTL8720CF) com a letra borrada.
- Tem um segundo chip, em encapsulamento parecido com TSSOP-20, que não conseguimos identificar. Como a central já fala TuyaMCU direto, ele deve ser conversor de nível ou outro componente de apoio, e não um tradutor de protocolo.
- Encaixa como um pendrive no USB-A da central.

## Garen Config (teclado programador)

Pelo manual dele, o Config:
- liga no **mesmo conector SETUP** (cabo USB macho-macho, ou adaptador para a barra de 5 pinos);
- é alimentado pela central;
- reconhece o modelo da central sozinho e ajusta **só os parâmetros que o app também ajusta**.

Tudo indica que ele também fala TuyaMCU. Escutar a conversa dele com a central seria o jeito mais fácil de descobrir os DPs que só aceitam escrita (SALVAR, gravar/apagar controle, iniciar gravação de curso).
