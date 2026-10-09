# Firmware ESP32-C3: "Setup caseiro"

Substitui o Garen Setup. O ESP32-C3 conversa com a central pela porta SETUP (TuyaMCU) e cria um **hotspot Wi-Fi** com uma página para o celular. Não precisa de internet nem de nuvem.

## Recursos

- Botão para **acionar** o portão, com resposta em fração de segundo (o ESP espera o eco da central antes de responder).
- Estado em tempo real: **fechado**, **aberto/parado**, **em movimento**, **gravando curso**, central online/offline e **modo** (Usuário/Instalador).
- **Parâmetros:** pausa, força, rampa de abertura, rampa de fechamento e freio.
  - No **modo Instalador**: botões − / + (travados até ligar "editar") e botão **Salvar na central**.
  - No **modo Usuário** ("modo cliente"): a página mostra só o acionamento, o estado e os valores, com um aviso no topo explicando que o modo Instalador libera a configuração. Os controles de edição, controles remotos e curso ficam escondidos.
- **Gravar controle remoto** (modo Instalador): abre o menu TX, espera a central **detectar** o controle e só então confirma. Mostra "gravado", "já cadastrado" ou "tempo esgotado".
- **Apagar todos os controles** (modo Instalador), com confirmação dupla.
- **Gravar curso:** inicia a gravação automática (a central percorre os dois fins de curso sozinha). Pede confirmação, porque o portão se move.
- Número de controles cadastrados.
- Tema claro/escuro automático.

> A versão com edição, salvar e gravar controle foi testada **só com um simulador da central** no navegador. Os comandos que ela usa foram todos testados na central real pelo PC, mas o conjunto ainda não rodou no ESP ligado à central.

## Montagem

| Central (SETUP) | ESP32-C3 |
|---|---|
| 5V | 5V / VIN (**não** no 3V3) |
| – (GND) | GND |
| TX (PA2) | `GAREN_RX_PIN` (padrão GPIO 20) |
| RX (PA3) | `GAREN_TX_PIN` (padrão GPIO 21) |

Os pinos padrão são os da **ESP32-C3 SuperMini**. Ajuste no topo do `.ino` para a sua placa. Veja também os cuidados de alimentação em [docs/hardware.md](../../docs/hardware.md#alimentação-pela-porta-setup).

## Gravar (Arduino IDE)

1. Instale o core **esp32 by Espressif** 3.x pelo Gerenciador de Placas.
2. Abra `garen_setup_c3.ino` e **troque `AP_PASSWORD`**. Quem entrar nessa rede consegue abrir o seu portão e gravar controles. A senha precisa ter pelo menos 8 caracteres, senão o hotspot não inicia.
3. Placa: **ESP32C3 Dev Module**. **USB CDC On Boot: Enabled**.
4. Grave com o **5V da central desconectado**: deixe o USB alimentando o ESP.

Também dá para compilar pela linha de comando:

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc" firmware/garen_setup_c3
```

## Uso

1. Ligue a central. O ESP cria a rede **`Garen-Portao`**.
2. Conecte o celular e abra **http://192.168.4.1**.
3. No Android, se aparecer o aviso de "rede sem internet", escolha manter a conexão. Desligar os dados móveis também ajuda.

Para configurar parâmetros ou gravar controles, ponha a central no **modo Instalador**: feche os jumpers FORÇA e RAMPA AB, segure TX (−) por 5 s até o LED piscar **1 vez** e abra os jumpers. O ESP detecta o modo sozinho em até ~10 s.

Monitor serial a 115200: mostra o endereço do hotspot, ou um erro se a senha for curta demais.

## API

| Método | Rota | O que faz |
|---|---|---|
| GET | `/api/status` | Estado completo em JSON |
| POST | `/api/acionar` | Aciona o portão (DP 1 = true) e responde com o estado novo |
| POST | `/api/param?dp=<dp>&v=<valor>` | Leva um parâmetro até o valor (modo Instalador). Só aceita os DPs da tabela `PARAMS`, dentro da faixa e do passo. Responde `502` se a central não confirmar e `403` no modo Usuário |
| POST | `/api/salvar` | Grava os parâmetros na memória da central (DP 123) |
| POST | `/api/tx` | Abre o menu TX para gravar um controle (modo Instalador). Acompanhe por `tx` no status |
| POST | `/api/tx/apagar` | **Apaga todos os controles** (modo Instalador). Responde `502` se a central não zerar o DP 151 |
| POST | `/api/curso` | Inicia a gravação de curso automática (DP 120 = 1). `curso` no status fica `true` até terminar. **O portão se move sozinho** |

Exemplo de `/api/status`:

```json
{"v":42,"online":true,"modo":"instalador","motor":false,"fechado":true,"aux":false,"curso":false,
 "controles":2,"pendente":false,"tx":{"ativo":false,"fase":0,"resultado":""},
 "params":[{"dp":108,"key":"pausa","label":"Pausa","unit":"s","min":0,"max":105,"step":5,"value":45}, ...]}
```

## Faixas dos parâmetros

Ficam na tabela `PARAMS` no topo do arquivo e seguem o manual da Fit Connect, com uma exceção: a **pausa vai até 105** porque a central ignora valores acima de 100 na serial. Até 120 s só pelos botões da placa.
