# Como foi feita a engenharia reversa

Registro do caminho até o protocolo, incluindo os erros, para quem quiser repetir em outra central.

## Ponto de partida

- **Objetivo:** controlar a central sem o Garen Setup, usando o cabo de rede que chega ao portão (não há Wi-Fi lá).
- **O que se sabia:** o Setup é configurado pelo app G.Smart 4.0. O manual manda procurar o dispositivo como "Dispositivo de abertura de porta de Garagem BLE+Wi-Fi", que é uma categoria padrão da Tuya. Isso sugeria um módulo Tuya falando **TuyaMCU** com a central.
- **Material:** só a central. Nenhum Setup disponível.

## 1. Análise do app (APK)

Ferramenta: [jadx](https://github.com/skylot/jadx).

- O APK (G.Smart 4.0 v1.0.7, pacote `com.Smart.Garen`) é um **app Tuya de marca própria (OEM)**, montado sobre o SDK da Tuya, que foi renomeado para "ThingClips" (`com.thingclips.smart.*`, `libThing*.so`, `META-INF/thing`).
- **Não existe código da Garen.** O pacote `com.Smart.Garen` só tem o `R.java` gerado automaticamente.
- O PID da central e os nomes dos parâmetros **não estão no APK**. A tela de controle de cada produto é baixada da nuvem da Tuya quando o dispositivo é pareado.

**Conclusão:** o APK confirmou a Tuya, mas não traz o protocolo nem os datapoints. Isso reforçou a hipótese do TuyaMCU.

## 2. Hardware

- **Módulo Setup** (só por foto): Tuya **WBR3** e um chip auxiliar.
- **Conector da central:** USB-A, com uma barra de 5 pinos marcada `– – RX TX 5V`. Concluímos que o "USB" é só o encaixe e que os pinos levam uma UART.
- **MCU da central:** STM32C031K6T6. Pelo [banco de pinos oficial da ST](https://github.com/STMicroelectronics/STM32_open_pin_data), procuramos os pinos possíveis de UART e confirmamos com teste de continuidade: a barra vai para os pinos 9 e 10 do LQFP32, ou seja, **PA2/PA3 = USART2**.

## 3. Primeira escuta: silêncio

Ferramenta: conversor USB-TTL (FT232, em 3,3V) e [`tools/tuyamcu_probe.py`](../tools/tuyamcu_probe.py).

- O modo `sniff` (só escuta) em 7 velocidades não recebeu **nenhum byte**.
- O modo `probe` (manda heartbeat TuyaMCU) **também não teve resposta**.
- **Causa: TX e RX estavam invertidos.** O RX do conversor estava escutando o RX da central, onde nada é transmitido. Sem nenhum byte, nem lixo, o sinal era de problema físico, e não de protocolo ou velocidade erradas.
- Outros contratempos: o Monitor Serial do Arduino IDE estava com a porta COM aberta ("acesso negado"), e o conversor desconectou algumas vezes durante os testes.

## 4. A central fala TuyaMCU

Com os fios na posição certa, chegaram em 9600 baud quadros perfeitos, com checksum válido:

```
55 AA 03 07 00 05 65 01 00 01 01 76    DP 101 = true
55 AA 03 07 00 05 01 01 00 01 01 12    DP 1   = true
```

O `probe` completo respondeu com heartbeat, info do produto (`{"p":"l9era1stzldt1ybu","v":"1.0.3","m":0}`), modo de trabalho e uma lista de DPs. **A hipótese inicial estava certa: é TuyaMCU padrão, sem variações** (nada de cabeçalho `AA 55` nem velocidade fora do comum).

## 5. O `0x08` ignorado

O modo `watch` original esperava a central mandar os DPs sozinha e, num teste de 5 minutos, não recebeu nada. Testando comando por comando:

| Enviado | Resultado |
|---|---|
| `08` (consulta de status) | nada, sempre |
| `03` = `04` repetido | nada |
| `03` = `02` ou `03`, depois `03` = `04` | **todos os DPs em menos de 100 ms** |

A partir daí, o `watch` repete esse par a cada 1 s e mostra só o que muda.

## 6. Comando

`DP 1 = true` (`55 AA 00 06 00 05 01 01 00 01 01 0E`) acionou o relé do motor. Um segundo envio parou o motor. Testado duas vezes, com confirmação visual do relé e do LED.

## 7. Mapeamento dos parâmetros

Método: **mudar cada parâmetro pelos botões da própria central** (fechar o jumper do parâmetro e apertar `CURSO (+)` e `TX (−)`) com o `watch` rodando, e ver qual DP muda. Assim **nada é escrito pela serial**, o que elimina o risco de configurar a central errado.

| Ação na placa | DP que mudou |
|---|---|
| Pausa: subir de 0 até o máximo | 108: 0 → 5 → … → 120 |
| Força: − e + | 110: 100 → 95 → 100 |
| Freio: + e − | 118: 20 → 25 → 20 |
| Rampa AB: + e − | 117: 7 → 8 → 7 |
| Rampa FC: + e − | 111: 7 → 8 → 7 |
| Gravar um segundo controle | 151: 1 → 2 |
| Gravação de curso | 120: 1 durante a abertura, depois 0 |
| Jumpers T. FREIO e BV/DZ | nenhum |
| Botoeira | só DP 1/101, como o controle |

**Palpites que estavam errados** (vale lembrar antes de confiar em coincidências):
- Achamos que o DP 118 = 20 era a pausa ou o tempo máximo de motor, porque o motor parou em exatos 20 s sem fim de curso. Era o **freio**. O limite de 20 s é outra coisa.
- Achamos que o DP 113 = 104 era o tempo de curso (10,4 s). Regravamos o curso várias vezes com ~7 s, e ele **não mudou**.

## 8. Fins de curso simulados

Os fins de curso da Fit Connect são contatos secos, fáceis de simular na bancada com um jumper:
- **FCF** (fechamento) → DP 3 = false, e o motor para na hora.
- **FCA** (abertura) → o motor para na hora, mas **nenhum DP muda**.

Por isso a central consegue dizer se o portão está "fechado", mas não se está "totalmente aberto".

## 9. Escrever parâmetros: o caminho até funcionar

A escrita de parâmetros foi a parte mais trabalhosa, porque três coisas diferentes se somavam:

1. **Silêncio total no começo.** A primeira escrita foi pausa = **115** (a pausa estava em 120). A central não respondeu nada, nem com o handshake completo. Só bem depois descobrimos o motivo: **ela ignora valores acima de 100**. Foi azar na escolha do valor de teste.
2. **O modo Usuário/Instalador.** O manual completo da Fit Connect descreve um "modo instalador". Trocando o modo, a central **muda de PID**: é outro produto para a Tuya, com outra tela no app. No modo Usuário, a central responde à escrita com o valor atual (recusa); no Instalador, aceita.
3. **A lógica de passos.** Com um valor ≤ 100 no modo Instalador, a central aceitou, mas gravou valores "estranhos" (20 → 25, 50 → 55). Testando subida, descida e ±1, ficou claro que ela anda em passos na direção do valor, como os botões, e que subindo para num passo além de um múltiplo exato. O app original manda ±1 por toque e nunca percebe isso.

O LED de programação ajudou a entender: ele reage às escritas pela serial como reage aos botões, e fica aceso quando um parâmetro chega ao limite.

## 10. O "salvar"

As escritas **sumiam ao desligar a central**. Testamos e descartamos: DP 119 = 1 e esperar 5 minutos com o "módulo" conectado. Depois de uma varredura com `true` nos DPs vizinhos (102–123), o **DP 123** respondeu com o LED piscando 2x e reenviando o DP 151. Teste final: pausa 45 e rampa AB 9, depois DP 123, desligar e religar: **os valores ficaram**.

Por que a primeira varredura não achou o DP 123: ela mandava `false`, e um botão de "salvar" ignora `false` sem responder.

## 11. Gravar controles

Prints de um vídeo do app no modo Instalador mostraram a tela **TX: STANDBY / MENU TX / APAGAR TX**, que corresponde ao DP 119. Testando:

- `1` abre o menu (LED aceso);
- a central reporta `2` quando detecta um controle (LED piscando) e `5` quando o controle já está cadastrado;
- `3` confirma a gravação.

**Armadilha:** por duas vezes, mandar `3` gravou um código **desconhecido**, porque o `3` grava o último código de rádio recebido, mesmo sem ninguém apertar nada. Foi preciso apagar a memória pela placa e regravar. A regra no firmware: **só confirmar depois do `2`**. Com esse cuidado, os dois controles do dono foram regravados pela serial e funcionaram.

O "APAGAR TX" do app: enviar `2` não faz nada, porque é um status. Depois de mapear 1, 2, 3 e 5, testamos os valores que faltavam, um por vez e com o menu aberto. O `4` é aceito sem efeito visível, e o **`6` apaga todos os controles**. O controle do dono foi regravado pela serial logo em seguida.

## 12. Gravação de curso

Seguindo os manuais, gravando com o controle, a gravação **nunca terminava**: o LED não dava as 3 piscadas finais, e não havia sinal nenhum de conclusão pela serial. Comparamos três revisões do manual, todas com o mesmo procedimento. No fim, testando a placa, o dono descobriu que **no firmware 1.0.3 a gravação é automática**: CURSO (+) (3 piscadas), depois **segurar** CURSO (+), e a central percorre os dois fins de curso sozinha. Detalhes em [hardware.md](hardware.md#gravação-de-curso-firmware-103-automática).

Pela serial, **DP 120 = 1** inicia exatamente esse processo automático, até no modo Usuário. Ao terminar, a central reenvia o DP 151 e manda **DP 120 = 0**. Nas tentativas com o controle, o DP 120 caía para 0 no meio do caminho: era a gravação sendo abortada.

Lição: o manual pode estar desatualizado em relação ao firmware. Quando o comportamento não bate, vale testar variações na própria placa (apertar, segurar, apertar duas vezes).

## Tentativas que não deram certo

- **Explorar o app no emulador:** o G.Smart roda num emulador Android (x86_64 com tradução ARM), mas sem um Setup pareado ele não mostra a tela do produto. O recurso de "dispositivo de experiência virtual" está no SDK, mas a Garen não o ativou.
- **Simular o pareamento com a nuvem:** exige a licença (UUID/AuthKey) gravada de fábrica em cada módulo Tuya. Não tentamos falsificar.

## Em aberto

- **DP 119 = 4:** aceito, mas sem efeito visível.
- **DP 113:** o que é.
- **Varredura com `true` de 124 em diante e de 2 a 100.**
- **Outras centrais Garen:** provavelmente o mesmo protocolo, com outros PIDs e DPs.
- **`DP 1 = false`:** efeito desconhecido.
- **Um módulo Tuya genérico** com firmware para MCU, ligado na porta SETUP, deveria parear com o app usando o PID da central. Com isso daria para escutar o app original e confirmar tudo o que falta.

## Ferramenta

O [`tools/tuyamcu_probe.py`](../tools/tuyamcu_probe.py) tem os modos:

| Modo | O que faz | Transmite? |
|---|---|---|
| `sniff` | Só escuta, em uma ou várias velocidades (`--baud all`) | Não |
| `probe` | Heartbeat, info do produto, modo, status de rede. Lista os DPs | Só consultas |
| `watch` | Pede o status a cada `--poll` s e mostra só o que muda. Reconecta sozinho se a porta cair | Só consultas |
| `shell` | Sessão interativa. Inclui `dp <id> <tipo> <valor>`, que **escreve na central** | Sim |

Tudo o que passa na serial fica gravado em `tuyamcu_log.txt`.
