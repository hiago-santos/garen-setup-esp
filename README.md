# garen-setup-esp

Engenharia reversa da comunicação entre o **Garen Setup** (módulo Wi-Fi/Bluetooth do app G.Smart 4.0) e as centrais de portão eletrônico Garen. Inclui um firmware para **ESP32-C3** que substitui o Setup original.

O resultado principal: **a porta SETUP das centrais Garen fala TuyaMCU padrão** (UART 9600 8N1, 3,3V, quadros `55 AA`). Com isso, qualquer microcontrolador consegue acionar o portão, ler o estado e ajustar os parâmetros da central, sem o módulo original e sem nuvem.

> Projeto independente, sem nenhuma relação com a Garen Automação S/A. "Garen", "G.Smart" e "Tuya" são marcas de seus donos. Use por sua conta e risco. A central tem tensão de rede na placa.

## Testado em

| Item | Detalhe |
|---|---|
| Central | **Garen Fit Connect** (versão de placa azul) |
| MCU da central | STM32C031K6T6 (LQFP32) |
| Firmware da central | `1.0.3` (informado pela própria central) |
| Product ID Tuya | `l9era1stzldt1ybu` (modo Usuário) / `cpk9ndmiftjvwhwz` (modo Instalador) |
| App | G.Smart 4.0 v1.0.7 (`com.Smart.Garen`) |

Outras centrais Garen "compatíveis com o Setup" provavelmente usam o mesmo protocolo, mas talvez com outro PID e outros datapoints. Se você testar outra, abra uma issue ou um PR.

## Resumo rápido

| O quê | Como |
|---|---|
| Conector | SETUP: USB-A fêmea **ou** barra de 5 pinos `– – RX TX 5V`. O USB-A é só o encaixe físico: os pinos D−/D+ carregam a UART |
| Serial | 9600 8N1, nível 3,3V. TX da central = PA2 (USART2) |
| Protocolo | TuyaMCU padrão. A central é o "MCU"; quem conecta faz o papel de "módulo Wi-Fi" |
| **Acionar o portão** | Enviar `55 AA 00 06 00 05 01 01 00 01 01 0E` (DP 1 = true), equivalente a apertar o controle |
| Ler o estado | A central **ignora** o `0x08`. Envie status de rede `03` com `03` e depois `03` com `04`, e ela devolve todos os DPs |
| Fechado? | DP 3: `false` = fechado, `true` = não fechado |
| Configurar | Só no **modo Instalador**. Escrever o parâmetro (DPs 108, 110, 111, 117, 118) e **salvar** com `55 AA 00 06 00 05 7B 01 00 01 01 88` (DP 123 = true) |
| Gravar controle | DP 119 = 1 (menu), esperar a central reportar 2 (detectado), DP 119 = 3 (confirmar) |
| Apagar controles | DP 119 = 1 (menu), depois DP 119 = 6 |
| Gravar curso | DP 120 = 1. A central percorre os fins de curso sozinha e reporta DP 120 = 0 ao terminar |

O mapa completo de datapoints está em [docs/protocolo.md](docs/protocolo.md).

## Conteúdo

| Caminho | O que é |
|---|---|
| [docs/protocolo.md](docs/protocolo.md) | Protocolo TuyaMCU da central: quadros, datapoints, peculiaridades |
| [docs/hardware.md](docs/hardware.md) | Conector SETUP, pinagem, MCU, alimentação, o módulo Setup original |
| [docs/engenharia-reversa.md](docs/engenharia-reversa.md) | Como chegamos aqui: método, testes, erros e o que ficou em aberto |
| [firmware/garen_setup_c3](firmware/garen_setup_c3) | Firmware Arduino para ESP32-C3: hotspot Wi-Fi com página para acionar e configurar |
| [tools/tuyamcu_probe.py](tools/tuyamcu_probe.py) | Script Python para escutar, descobrir e comandar a central por um conversor USB-TTL |

## Firmware ESP32-C3 ("Setup caseiro")

O ESP32-C3 conecta na porta SETUP e cria uma rede Wi-Fi própria. Pelo celular, em `http://192.168.4.1`, você:

- aciona o portão;
- vê o estado (fechado, aberto/parado, em movimento), o modo da central e se ela está online;
- no modo Instalador: ajusta pausa, força, rampas e freio, salva na central e grava controles remotos;
- grava o curso do portão (a central faz o percurso sozinha).

Instruções em [firmware/garen_setup_c3/README.md](firmware/garen_setup_c3/README.md).

## Explorar a sua central com um conversor USB-TTL

```bash
pip install pyserial
python tools/tuyamcu_probe.py --list
python tools/tuyamcu_probe.py COM6 probe --baud 9600
python tools/tuyamcu_probe.py COM6 watch --baud 9600 --poll 1
```

`probe` identifica a central e lista os datapoints. `watch` mostra cada mudança de DP em tempo real, só lendo. Ligue **RX do conversor → TX da central**, **TX → RX** e **GND**, com o conversor em **3,3V** e **sem** ligar o VCC dele.

## Status

| Recurso | Situação |
|---|---|
| Acionar (DP 1) | ✅ testado na central (modo Usuário) |
| Ler motor ligado / fechado / parâmetros | ✅ testado na central |
| Mapa dos parâmetros (pausa, força, rampas, freio, controles) | ✅ confirmado mudando cada um pelos botões da placa |
| Escrever parâmetros pela serial | ✅ testado (modo Instalador), com a lógica de passos descrita no protocolo |
| Salvar (DP 123) | ✅ testado: os valores sobrevivem a desligar e religar |
| Gravar controle (DP 119) | ✅ testado: 2 controles gravados pela serial |
| Gravar curso (DP 120 = 1) | ✅ testado: inicia a gravação automática; DP 120 volta a 0 ao terminar |
| Apagar todos os controles (DP 119: 1 → 6) | ✅ testado |
| Firmware ESP32-C3 | Compila. A versão com edição, salvar e gravar controle foi testada só com um simulador da central, **não no ESP com a central** |
| Distinguir "totalmente aberto" de "parado no meio" | ❌ a central não informa |

## Licença

[MIT](LICENSE), para o código e a documentação.
