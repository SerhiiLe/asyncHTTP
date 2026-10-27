# asyncHTTP 🚀

Лёгкая, неблокирующая асинхронная библиотека HTTP/HTTPS клиента для микроконтроллеров **ESP8266** и **ESP32** с поддержкой **JSON**, **XML**, **Chunked Transfer Encoding**, **HTTP Keep-Alive** и фонового режима **FreeRTOS** на ESP32.

Zero external dependencies — библиотека работает на базе стабильных системных сокетов (`WiFiClient` для HTTP, `BearSSL::WiFiClientSecure` на ESP8266 и `WiFiClientSecure` на ESP32) и управляется быстрым неблокирующим конечным автоматом (FSM).

---

## ⚡ Почему asyncHTTP?

В стандартных библиотеках (например, `ESP8266HTTPClient` или `HTTPClient`):
- Вызов `http.GET()` или `http.POST()` является **полностью синхронным и блокирующим**.
- На ESP8266 процесс разрешения DNS, TLS-рукопожатия (BearSSL), ожидания ответа сервера и скачивания тела занимает от **1 до 4 секунд**.
- На это время **главный цикл `loop()` замирает**: динамическая индикация, опрос кнопок, плавные анимации бегущей строки, секундные точки и часы полностью зависают.

**asyncHTTP решает эту проблему фундаментально:**
1. **Zero-blocking FSM (конечный автомат):** Шаг `httpClient.tick()` выполняется за доли микросекунды. Заголовки и тело ответа считываются по мере их поступления в сокет, не задерживая основной цикл.
2. **HTTP Keep-Alive:** Соединение с сервером остаётся открытым между последовательными запросами (например, погода + прогноз к `api.open-meteo.com`). Это исключает повторные тяжелые TLS-рукопожатия.
3. **Chunked Transfer Encoding:** Полная поддержка потоковой передачи чанками (`Transfer-Encoding: chunked`), которую используют большинство современных сервисов (Open-Meteo и др.). Чанки декодируются на лету без сырых hex-разделителей.
4. **Встроенный парсинг JSON и XML:**
   - Полная интеграция с `ArduinoJson 7` через `res.parseJson(doc)`.
   - Встроенный легковесный экстрактор `res.extract("field", isJson)` для мгновенного извлечения значений без расхода оперативной памяти.
5. **Очередь запросов (Request FIFO Queue):** Если запущено несколько запросов одновременно, они выстраиваются в очередь и выполняются последовательно с вызовом соответствующих callback-функций.
6. **ESP32 FreeRTOS Task Mode:** На ESP32 библиотека может работать в полностью изолированной фоновой задаче FreeRTOS на соседнем ядре (`AsyncHTTP httpClient(true);`). В этом случае даже не нужно вызывать `tick()` в `loop()`.
7. **Автоматический редирект (301, 302, 307, 308):** Автоматический переход по `Location` с поддержкой абсолютных и относительных путей.

---

## 📋 Архитектура конечного автомата (FSM)

```
[ IDLE ] ──(Запрос из очереди)──> [ CONNECTING ] (DNS/TLS)
   ▲                                      │
   │                                      ▼
[ FINISH / CALLBACK ]                 [ SENDING ] (HTTP Headers + Body)
   ▲                                      │
   │                                      ▼
   ├── [ READ_BODY ] <── [ READ_HEADERS ] <── [ WAIT_HEADERS ]
       (Chunked / Len / EOF)
```

---

## 🚀 Быстрый старт

### 1. Подключение библиотеки
```cpp
#include <AsyncHTTP.h>
#include <ArduinoJson.h> // опционально для JSON

// На ESP8266:
AsyncHTTP httpClient;

// На ESP32 (с фоновой задачей FreeRTOS):
// AsyncHTTP httpClient(true);
```

### 2. Инициализация в setup()
```cpp
void setup() {
    Serial.begin(115200);
    WiFi.begin("SSID", "PASSWORD");
    while (WiFi.status() != WL_CONNECTED) delay(500);

    httpClient.begin();
    httpClient.setTimeout(10000); // 10 секунд
    httpClient.setKeepAlive(true);
}
```

### 3. Отправка GET запроса (Погода JSON)
```cpp
httpClient.get("https://api.open-meteo.com/v1/forecast?latitude=46.48&longitude=30.74&current=temperature_2m", [](AsyncHttpResponse& res) {
    if (res.isSuccess()) {
        JsonDocument doc;
        res.parseJson(doc);
        float temp = doc["current"]["temperature_2m"];
        Serial.printf("Temperature: %.1f °C\n", temp);
    } else {
        Serial.printf("Error: %d (%s)\n", res.code, res.errorString().c_str());
    }
});
```

### 4. Извлечение цитаты без ArduinoJson (Легковесный extract)
```cpp
httpClient.get("https://dummyjson.com/quotes/random", [](AsyncHttpResponse& res) {
    if (res.isSuccess()) {
        String quote = res.extract("quote", true);
        String author = res.extract("author", true);
        Serial.printf("\"%s\" — %s\n", quote.c_str(), author.c_str());
    }
});
```

### 5. Отправка POST запроса
```cpp
httpClient.post("https://httpbin.org/post", "param1=value1&param2=value2", "application/x-www-form-urlencoded", [](AsyncHttpResponse& res) {
    if (res.isSuccess()) {
        Serial.println("POST successful!");
    }
});
```

### 6. Шаг автомата в loop()
```cpp
void loop() {
    // На ESP8266 вызывается в loop() обязательно:
    httpClient.tick();

    // Пользовательский код часов, кнопок и дисплея работает без задержек:
    updateDisplay();
}
```

---

## 🛠️ API Справочник

### Методы запросов
| Метод | Описание |
| :--- | :--- |
| `get(url, callback)` | Асинхронный GET запрос |
| `post(url, payload, contentType, callback)` | Асинхронный POST запрос с телом |
| `send(AsyncHttpRequest)` | Универсальный запрос с кастомными заголовками и методом |
| `isBusy()` | `true`, если выполняется запрос или очередь не пуста |
| `isIdle()` | `true`, если библиотека свободна для новых запросов |
| `getQueueSize()` | Количество ожидающих запросов в очереди |
| `stop()` | Остановка клиента, закрытие сокетов и очистка очереди |

### Настройки
| Метод | Описание | По умолчанию |
| :--- | :--- | :--- |
| `setTimeout(ms)` | Таймаут ожидания соединения и данных | `10000` (10 сек) |
| `setKeepAlive(seconds)` | Удержание Keep-Alive (0/false = выкл, >0 = сек ожидания, max 10) | `10` сек |
| `setFollowRedirects(bool, max)` | Следование за HTTP 301/302 перенаправлениями | `true`, max 3 |
| `setMaxBodySize(bytes)` | Защита от исчерпания RAM при слишком большом ответе | `16384` (16 КБ) |
| `setBufferSizes(rx, tx)` | Размеры буферов BearSSL для ESP8266 | `2048, 512` |

### Структура `AsyncHttpResponse`
- `res.code`: HTTP статус-код (200, 404 и т.д.) либо код ошибки `< 0` (`HTTP_ERR_TIMEOUT`, `HTTP_ERR_CONNECT` и др.).
- `res.body`: Строка с полученным телом ответа.
- `res.contentType`: Значение заголовка `Content-Type`.
- `res.contentLength`: Длина контента (-1 если chunked).
- `res.isSuccess()`: Возвращает `true`, если статус 2xx.
- `res.errorString()`: Текстовое описание кода ошибки или статуса.
- `res.getHeader("Name")`: Получение значения указанного заголовка.
- `res.extract("field", isJson)`: Мгновенное извлечение значения из JSON или XML без аллокации дерева документов.
- `res.parseJson(doc)`: Десериализация в `JsonDocument` (ArduinoJson 7).

---

## 📊 Сравнение с HTTPClient

| Параметр | Стандартный HTTPClient | asyncHTTP |
| :--- | :---: | :---: |
| Блокировка `loop()` | **Да (1–4 секунды)** | **Нет (0 мс)** |
| Мерцание/замирание экрана часов | **Да** | **Полностью устранено** |
| Поддержка Chunked Encoding | Ограниченная | **Полная потоковая** |
| Повторное использование TLS (Keep-Alive) | Нет / Ручное | **Автоматическое** |
| Защита кучи от переполнения (OOM) | Нет | **Да (`maxBodySize`)** |
| Очередь запросов | Нет | **Да (FIFO Queue)** |
| Фоновый режим FreeRTOS (ESP32) | Нет | **Да (`AsyncHTTP(true)`)** |

---

## 📄 Лицензия

MIT License © 2026 Serhii Le
