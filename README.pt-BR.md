# eth_w5100: WIZnet W5100 Ethernet para ESP-IDF 6.x

*Read in English: [README.md](README.md)*

Driver `esp_eth` (MAC/PHY) para o **WIZnet W5100**, o chip do Arduino Ethernet Shield clássico, no
**ESP-IDF 6.x**. Ele se conecta ao `esp_netif`, então tudo que roda sobre o lwIP funciona pelo cabo sem
mudanças: esp-mqtt, esp_http_client, SNTP, DNS e sockets.

> **Status: 0.1.0.** Compila no ESP-IDF 6.1, conferido pelo CI a cada push. **Ainda não foi testado em
> hardware.** O primeiro teste num Arduino Ethernet Shield real está pendente. Quando ele passar, a primeira
> versão validada será a 0.2.0, com o resultado registrado no [CHANGELOG.md](CHANGELOG.md).

## Por que um driver para o W5100?

A Espressif mantém drivers para W5500, DM9051, KSZ8851SNL e outros no
[esp-eth-drivers](https://github.com/espressif/esp-eth-drivers). Eles saíram do ESP-IDF na versão 6.0. Não existe
driver para o W5100, que continua sendo o shield mais barato e mais comum.

O W5100 tem uma pilha TCP/IP em hardware, mas **este driver não a usa**. O socket 0 é aberto no modo **MACRAW**:
o chip só troca quadros Ethernet brutos, e quem faz todo o TCP/IP é o lwIP no ESP32. É a mesma abordagem do
driver oficial do W5500. O resultado é uma interface de rede comum, sem o limite de 4 sockets e sem uma segunda
pilha TCP/IP para manter em sincronia.

## Origem

Este repositório é um port do [KaeLL/w5100_esp32](https://github.com/KaeLL/w5100_esp32), escrito para o IDF 5.2.1
e liberado em domínio público. O histórico git completo do original foi mantido aqui.
O [docs/porting-notes.md](docs/porting-notes.md) lista as mudanças para o IDF 6.x e as correções encontradas no
caminho. Ele também confere cada registrador, bit e tempo contra o datasheet do W5100 v1.2.8 e contra o
esquemático do Arduino Ethernet Shield.

## Uso rápido

1. Adicione o componente em `main/idf_component.yml` do seu projeto:

   ```yaml
   dependencies:
     eth_w5100:
       git: https://github.com/MarciovsRocha/eth_w5100.git
       version: "main"   # a tag v0.2.0 sai depois da primeira validação em hardware
   ```

2. Siga o exemplo de código do [README.md](README.md#quick-start), ou o programa completo em
   [examples/ethernet_basic](examples/ethernet_basic).

A rede se configura sozinha: por padrão a interface obtém IP, gateway e servidor DNS por DHCP assim que o cabo é
ligado numa rede com servidor DHCP. Um IP fixo pode ser definido em `ifcfg.sip`.

O exemplo é um autoteste em seis etapas, cada uma com PASS, FAIL ou SKIP e um resumo no final:

| Etapa | O que prova |
|---|---|
| SPI | O W5100 responde no barramento |
| DHCP | IP, gateway e DNS foram obtidos da rede |
| PING | O gateway responde ao ICMP, então TX e RX funcionam |
| DNS | Um nome público é resolvido (padrão `example.com`) |
| TCP | Um `HEAD` HTTP para esse host é respondido pela internet |
| MQTT | Uma mensagem publicada no broker volta pela própria assinatura (padrão `mqtt://test.mosquitto.org`) |

## Ligação (Arduino Ethernet Shield → ESP32)

| Sinal | Shield | ESP32 |
|---|---|---|
| SCLK | ICSP pino 3 | GPIO18 |
| MISO | ICSP pino 1 | GPIO19 |
| MOSI | ICSP pino 4 | GPIO23 |
| CS do W5100 | D10 | GPIO17 |
| CS do cartão SD | D4 | 3V3 (mantém o SD fora do barramento) |
| 5 V | 5V | VIN / 5V |
| GND | GND | GND |
| RESET | — | **não ligar** (é uma entrada de 5 V do supervisor, veja abaixo) |

Nenhum desses pinos do ESP32 tem função de *strapping* no boot. A explicação completa está em
[docs/wiring.md](docs/wiring.md): as tensões, por que o RESET fica desligado e quais pinos evitar.

## Limitações

- **Sem filtro de multicast.** O filtro de MAC do W5100 só deixa passar o próprio MAC e broadcast. mDNS e IPv6
  não funcionam, a não ser em modo promíscuo. IPv4 unicast (TCP, UDP, DHCP, DNS, ICMP) não é afetado.
- **O link é sempre informado como ativo.** O W5100 não tem registrador de status do PHY, então um cabo
  desconectado não é detectado pelo driver. Use timeouts na aplicação, como o keepalive do MQTT.
- **Polling, sem interrupção.** O pino INT não é usado.
- **Vazão.** O protocolo SPI do W5100 transfere 1 byte por quadro de 32 bits. A 4 MHz isso dá uma estimativa de
  algumas dezenas de kB/s: serve para telemetria e MQTT, não para transferências grandes. Ainda não foi medido.
- Um W5100 por firmware; alvo testado: `esp32`.

## Licença

Domínio público ([The Unlicense](LICENSE)), nos mesmos termos do projeto original.
