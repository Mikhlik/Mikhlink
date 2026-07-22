# Moblink в Mikhlink

Этот документ фиксирует протокол и архитектурные границы, чтобы дальнейшая разработка не строилась на догадках.

## Зафиксированные upstream-источники

- Moblin iOS: [`eerimoq/moblin@3438dd3`](https://github.com/eerimoq/moblin/tree/3438dd39385aede79ed4e3cc64ae49d35f1a201a), состояние от 2026-07-22.
- Moblink Android: [`eerimoq/Moblink@7ff4e3e`](https://github.com/eerimoq/Moblink/tree/7ff4e3ea6f86383a27bd5b0377ef24044bcfa43e), состояние от 2026-07-21.
- moblink-rust: [`datagutt/moblink-rust@dbf4476`](https://github.com/datagutt/moblink-rust/tree/dbf44769144f46541e7f47ed29c4b4b037f50cbd), состояние от 2026-05-26.

Главными источниками wire protocol являются `MoblinkProtocol.swift`, `MoblinkStreamer.swift`, `MoblinkRelay.swift` и Android `Protocol.kt`/`Relay.kt`. `moblink-rust` использован как дополнительная сверка, но его TUN-архитектура не переносится в Windows-плагин.

## Control plane

- Транспорт: WebSocket поверх TCP.
- Порт по умолчанию: `7777`.
- Ручной URL: `ws://LAN_IP_стримера:7777`.
- DNS-SD service type для будущего автообнаружения: `_moblink._tcp.local`.
- Версия API, отправляемая Mikhlink: `0.1`.

После WebSocket handshake streamer отправляет:

```json
{"hello":{"apiVersion":"0.1","authentication":{"challenge":"...","salt":"..."}}}
```

Телефон вычисляет authentication:

1. `hash1 = SHA256(password + salt)`;
2. `base64_1 = Base64(hash1)`;
3. `hash2 = SHA256(base64_1 + challenge)`;
4. `authentication = Base64(hash2)`.

Затем телефон отправляет стабильный UUID и имя:

```json
{"identify":{"id":"UUID","name":"Phone","authentication":"BASE64"}}
```

При успехе streamer отвечает `{"identified":{"result":{"ok":{}}}}`. Пароль сравнивается без раннего выхода; сам пароль не пишется в relay-файл и не передаётся `srtla_send.exe`.

## Data plane

Для каждого авторизованного телефона Mikhlink отправляет:

```json
{"request":{"id":1,"data":{"startTunnel":{"address":"SRTLA_HOST","port":5000}}}}
```

Телефон создаёт UDP listener со стороны локальной сети и отдельный UDP socket, привязанный к выбранной мобильной сети. Ответ содержит локальный порт телефона:

```json
{"response":{"id":1,"result":{"ok":{}} ,"data":{"startTunnel":{"port":54321}}}}
```

Mikhlink публикует для sender только готовый descriptor:

```json
{
  "version": 1,
  "relays": [
    {
      "id": "UUID",
      "name": "Phone",
      "bind_ip": "192.168.1.10",
      "remote_ip": "192.168.1.20",
      "remote_port": 54321
    }
  ]
}
```

`srtla_send` создаёт обычный UDP socket, привязанный к `bind_ip` ПК, но подключает его к `remote_ip:remote_port` телефона. Телефон пересылает SRTLA datagram на BELABOX через cellular и возвращает ответы обратно. Поэтому телефон является удалённым SRTLA endpoint, а не виртуальной сетевой картой Windows.

Поздно подключённый телефон получает текущий SRTLA group id через REG2. При изменении endpoint старый socket удаляется, новый регистрируется, а локальный SRT-вход OBS не перезапускается.

## Идентичность и приоритет

- Физический канал sender идентифицируется своим source IP.
- Телефон идентифицируется как `moblink:<UUID>`.
- UUID, а не IP телефона и не IP адаптера ПК, является ключом сохранённых режима и приоритета.
- `Bonding` использует процентный вес.
- `Backup` остаётся зарегистрированным с минимальным весом и может принять поток при отказе основных каналов.
- `Disabled` имеет вес `0`, но управляющее соединение и телефонная телеметрия остаются активными.

## Телеметрия и liveness

Streamer раз в секунду запрашивает:

```json
{"request":{"id":2,"data":{"status":{}}}}
```

Ответ может содержать:

```json
{"response":{"id":2,"result":{"ok":{}},"data":{"status":{"batteryPercentage":83,"thermalState":"yellow"}}}}
```

`thermalState` имеет значения `white`, `yellow`, `red`; отдельные платформы вправе вернуть `null`. WebSocket ping отправляется раз в 10 секунд. Отсутствие pong закрывает соединение, удаляет relay descriptor и затем телефонный SRTLA link.

## Осознанно отложено

- DNS-SD/mDNS advertisement для автоматического списка серверов.
- Метрики телефона, которых нет в текущем upstream-протоколе. Они должны добавляться только после расширения wire protocol и проверки обеих мобильных реализаций.
- Стримовый overlay. Dock-панель остаётся локальной и не попадает в эфир.
