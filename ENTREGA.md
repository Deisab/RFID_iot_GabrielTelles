# Leitor RFID com MQTT — Entrega

**Aluno:** SEU NOME
**Disciplina:** IoT — Prof. Leonam V. Hemann
**Link do projeto no Wokwi:** COLE-O-LINK-AQUI

## Como funciona

- Ao ligar, o ESP32 conecta no Wi-Fi `Wokwi-GUEST`, acerta a hora por NTP (UTC-3) e conecta no broker `test.mosquitto.org:1883`.
- Ao conectar, publica em `sis1a/SEU-NOME/rfid/mac`, **com retain**, o MAC, o IP e a hora.
- A central manda comandos em `sis1a/SEU-NOME/rfid/cmd`:
  - `CADASTRO`: entra no modo cadastro por 15 s com o LED piscando. O próximo cartão é cadastrado (`cadastrada`, `ja_cadastrada`, `tempo_esgotado` ou `lista_cheia`, com no máximo 10 cartões).
  - `REMOVER A3 4F 12 9C`: remove o cartão (`removida` ou `nao_encontrada`).
  - `REMOVER` (sem UID): entra no modo remoção por 15 s com o LED piscando; o próximo cartão passado é removido (`removida`, `nao_encontrada` ou `tempo_esgotado`).
  - `LISTAR`: publica em `.../lista` todos os UIDs cadastrados.
- Fora do modo cadastro, cada cartão publica em `.../acesso`: `liberado` (LED aceso 2 s) ou `negado` (LED pisca 3 vezes rápido).
- Os UIDs saem em hexadecimal separados por espaço e funcionam com 4 e 7 bytes (NFC Tag). Se o mesmo cartão for lido de novo em menos de 2 s, a leitura é ignorada.
- Se o Wi-Fi ou o MQTT cair, a reconexão é automática (nova tentativa a cada 3 s).

UIDs dos cartões no Wokwi: Blue `01 02 03 04`, Green `11 22 33 44`, Yellow `55 66 77 88`, Red `AA BB CC DD`, NFC Tag `04 11 22 33 44 55 66`, Key Fob `C0 FF EE 99`.

## Comandos de teste

```
mosquitto_sub -h test.mosquitto.org -t "sis1a/SEU-NOME/rfid/#" -v
mosquitto_pub -h test.mosquitto.org -t "sis1a/SEU-NOME/rfid/cmd" -m "CADASTRO"
mosquitto_pub -h test.mosquitto.org -t "sis1a/SEU-NOME/rfid/cmd" -m "REMOVER 11 22 33 44"
mosquitto_pub -h test.mosquitto.org -t "sis1a/SEU-NOME/rfid/cmd" -m "LISTAR"
```

## Prints

### 1. Mensagem retida em .../mac
![mac](prints/01-mac.png)

### 2. Cadastro (cadastrada)
![cadastro](prints/02-cadastro.png)

### 3. Cadastro repetido (ja_cadastrada)
![repetido](prints/03-repetido.png)

### 4. Tempo esgotado (tempo_esgotado)
![tempo](prints/04-tempo-esgotado.png)

### 5. Acesso liberado
![liberado](prints/05-liberado.png)

### 6. Acesso negado
![negado](prints/06-negado.png)

### 7. REMOVER (removida)
![remover](prints/07-remover.png)

### 8. LISTAR
![listar](prints/08-listar.png)
