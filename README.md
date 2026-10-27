# asyncHTTP 🚀

Лёгкая, неблокирующая асинхронная библиотека HTTP/HTTPS клиента для микроконтроллеров **ESP8266** и **ESP32** с поддержкой **JSON**, **XML**, **Chunked Transfer Encoding**, **HTTP Keep-Alive** и фонового режима **FreeRTOS** на ESP32.

**Zero external dependencies** — библиотека работает на базе стабильных системных сокетов (`WiFiClient` для HTTP, `BearSSL::WiFiClientSecure` на ESP8266 и `WiFiClientSecure` на ESP32) и управляется быстрым неблокирующим конечным автоматом (FSM).

📖 Подробное описание внутреннего устройства и спецификация доступны в **[ARCHITECTURE.md](ARCHITECTURE.md)**.

---

## 📑 Оглавление

1. [Ключевые возможности](#-ключевые-возможности)
2. [Установка](#-установка)
3. [Быстрый старт](#-быстрый-старт)
4. [Примеры использования](#-примеры-использования)
   - [1. GET запрос (Погода JSON с ArduinoJson)](#1-get-запрос-погода-json-с-arduinojson)
   - [2. GET запрос с быстрым извлечением (без библиотек)](#2-get-запрос-с-быстрым-извлечением-без-библиотек)
   - [3. POST запрос параметров формы (Form-Urlencoded)](#3-post-запрос-параметров-формы-form-urlencoded)
   - [4. POST запрос с JSON телом](#4-post-запрос-с-json-телом)
   - [5. Универсальный send() с кастомными заголовками и Bearer токеном](#5-универсальный-send-с-кастомными-заголовками-и-bearer-токеном)
   - [6. Конвейер запросов с Keep-Alive](#6-конвейер-запросов-с-keep-alive)
5. [API Справочник](#-api-справочник)
   - [Методы класса AsyncHTTP](#методы-класса-asynchttp)
   - [Структура ответа AsyncHttpResponse](#структура-ответа-asynchttpresponse)
   - [Коды ошибок HttpError](#коды-ошибок-httperror)
6. [Сравнение со стандартным HTTPClient](#-сравнение-со-стандартным-httpclient)
7. [Лицензия](#-лицензия)

---

## ⚡ Ключевые возможности

* **Zero-blocking FSM (конечный автомат):** Шаг `httpClient.tick()` выполняется за доли микросекунды. Заголовки и тело ответа считываются по мере их поступления в сокет, не задерживая основной цикл `loop()`.
* **Zero External Dependencies:** Никаких сторонних сетевых библиотек (`AsyncTCP`, `ESPAsyncTCP`). Только официальные стабильные сокеты ядра.
* **HTTP Keep-Alive с автотаймаутом:** Соединение удерживается открытым между последовательными запросами (например, погода + прогноз к `api.open-meteo.com`), исключая повторные тяжёлые TLS-рукопожатия (ускорение с ~1000 мс до ~20–30 мс). При бездействии сокет автоматически закрывается и память освобождается.
* **Chunked Transfer Encoding:** Полноценный потоковый декодер чанков на лету (поддержка Open-Meteo и других современных API).
* **Встроенный экстрактор и поддержка ArduinoJson:**
  - Встроенный метод `res.extract("field", isJson)` для мгновенного извлечения значений из JSON/XML с автоматическим декодированием Unicode (`\u0430` -> UTF-8) и XML-сущностей (`&quot;`, `&amp;`).
  - Прозрачная интеграция с `ArduinoJson 7` через `res.parseJson(doc)`.
* **Очередь запросов (Request FIFO Queue):** Запросы безопасно выстраиваются в очередь и выполняются последовательно с индивидуальными callback-функциями.
* **ESP32 FreeRTOS Task Mode:** Опциональная работа в полностью изолированной фоновой задаче FreeRTOS на соседнем ядре (`AsyncHTTP httpClient(true);`).
* **Автоматические редиректы (301, 302, 303, 307, 308):** Автоматический переход по `Location` с поддержкой абсолютных и относительных путей.

---

## 📦 Установка

### PlatformIO
Добавьте библиотеку в секцию `lib_deps` вашего `platformio.ini`:
```ini
lib_deps =
    https://github.com/SerhiiLe/asyncHTTP.git
    bblanchon/ArduinoJson @ ^7.0.0  ; опционально, для работы с JSON
```

### Arduino IDE
1. Скачайте репозиторий в виде ZIP-архива.
2. В Arduino IDE выберите: **Скетч → Подключить библиотеку → Добавить .ZIP библиотеку...**
3. Либо клонируйте репозиторий в папку библиотек:
   ```bash
   cd ~/Arduino/libraries
   git clone https://github.com/SerhiiLe/asyncHTTP.git
   ```

---

## 🚀 Быстрый старт

```cpp
#include <Arduino.h>
#if defined(ESP8266)
    #include <ESP8266WiFi.h>
#elif defined(ESP32)
    #include <WiFi.h>
#endif
#include <AsyncHTTP.h>

// Создание объекта клиента
#if defined(ESP32)
AsyncHTTP httpClient(true); // на ESP32 работает в фоновой задаче FreeRTOS
#else
AsyncHTTP httpClient;       // на ESP8266 неблокирующий FSM в loop()
#endif

void setup() {
    Serial.begin(115200);
    WiFi.begin("SSID", "PASSWORD");
    while (WiFi.status() != WL_CONNECTED) delay(500);

    httpClient.begin();
    httpClient.setKeepAlive(10); // Удержание Keep-Alive 10 секунд

    // Отправка запроса
    httpClient.get("https://dummyjson.com/quotes/random", [](AsyncHttpResponse& res) {
        if (res.isSuccess()) {
            Serial.printf("Цитата: %s\n", res.extract("quote").c_str());
        }
    });
}

void loop() {
    // На ESP8266 обязательно вызывается в loop():
    httpClient.tick();

    // Пользовательский код часов, кнопок и дисплея работает без задержек:
}
```

---

## 💡 Примеры использования

### 1. GET запрос (Погода JSON с ArduinoJson)
```cpp
const char* url = "https://api.open-meteo.com/v1/forecast?latitude=46.48&longitude=30.74&current=temperature_2m,relative_humidity_2m";

httpClient.get(url, [](AsyncHttpResponse& res) {
    if (res.isSuccess()) {
        JsonDocument doc;
        DeserializationError err = res.parseJson(doc);
        if (!err) {
            float temp = doc["current"]["temperature_2m"];
            int hum = doc["current"]["relative_humidity_2m"];
            Serial.printf("Температура: %.1f °C, Влажность: %d %%\n", temp, hum);
        }
    } else {
        Serial.printf("Ошибка: %d (%s)\n", res.code, res.errorString().c_str());
    }
});
```

### 2. GET запрос с быстрым извлечением (без библиотек)
Извлечение строковых и числовых полей из JSON или XML без аллокации дерева документов:
```cpp
// JSON ответ
httpClient.get("https://dummyjson.com/quotes/random", [](AsyncHttpResponse& res) {
    if (res.isSuccess()) {
        String quote = res.extract("quote", true);   // true = JSON
        String author = res.extract("author", true);
        Serial.printf("\"%s\" — %s\n", quote.c_str(), author.c_str());
    }
});

// XML ответ
httpClient.get("https://ultragenerator.com/citaty/handler.php", [](AsyncHttpResponse& res) {
    if (res.isSuccess()) {
        String quote = res.extract("quote", false);  // false = XML
        Serial.printf("XML Цитата: %s\n", quote.c_str());
    }
});
```

### 3. POST запрос параметров формы (Form-Urlencoded)
Тип контента `application/x-www-form-urlencoded` установлен по умолчанию:
```cpp
String url = "https://httpbin.org/post";
String params = "method=getQuote&format=json&lang=ru";

httpClient.post(url, params, [](AsyncHttpResponse& res) {
    if (res.isSuccess()) {
        Serial.println("POST запрос успешно выполнен!");
    }
});
```

### 4. POST запрос с JSON телом
```cpp
String url = "https://httpbin.org/post";
String jsonPayload = "{\"device\":\"Clock_Mini\",\"temp\":22.4}";

httpClient.post(url, jsonPayload, "application/json", [](AsyncHttpResponse& res) {
    if (res.isSuccess()) {
        Serial.println("JSON успешно доставлен на сервер!");
    }
});
```

### 5. Универсальный send() с кастомными заголовками и Bearer токеном
```cpp
AsyncHttpRequest req;
req.method = HTTP_POST;
req.url = "https://api.myserver.com/v1/telemetry";
req.payload = "{\"status\":\"active\"}";
req.contentType = "application/json";

// Добавление заголовков авторизации и кастомных метаданных:
req.headers.push_back({"Authorization", "Bearer my_secret_token_123"});
req.headers.push_back({"X-Device-Id", "ESP8266_Clock"});

// Индивидуальный таймаут именно для этого запроса:
req.customTimeout = 5000; // 5 секунд

req.callback = [](AsyncHttpResponse& res) {
    if (res.isSuccess()) {
        Serial.printf("Сервер ответил HTTP %d\n", res.code);
    } else {
        Serial.printf("Ошибка авторизации/сети: %d\n", res.code);
    }
};

httpClient.send(req);
```

### 6. Конвейер запросов с Keep-Alive
При постановке в очередь двух запросов к одному серверу соединение устанавливается один раз, а второй запрос отрабатывает практически мгновенно:
```cpp
// Запрос 1: Текущая погода
httpClient.get("https://api.open-meteo.com/v1/forecast?latitude=46.48&longitude=30.74&current=temperature_2m", [](AsyncHttpResponse& res) {
    Serial.println("Текущая погода получена (полный TLS-хэндшейк ~400 мс)");
});

// Запрос 2: Прогноз (встаёт в очередь)
httpClient.get("https://api.open-meteo.com/v1/forecast?latitude=46.48&longitude=30.74&daily=temperature_2m_max&forecast_days=3", [](AsyncHttpResponse& res) {
    Serial.println("Прогноз получен мгновенно через тот же сокет (~20 мс)!");
});
```

---

## 🛠️ API Справочник

### Методы класса AsyncHTTP

#### Управление жизненным циклом
* **`AsyncHTTP(bool useTask = false)`**  
  Конструктор. На ESP32 при `useTask = true` автомат работает в отдельной задаче FreeRTOS.
* **`bool begin(core = 1, stackSize = 8192, priority = 1)`**  
  Инициализация клиента и запуск фонового воркера (на ESP32).
* **`void stop()`**  
  Остановка работы, закрытие открытых сокетов и очистка очереди запросов.
* **`void tick()`**  
  Шаг конечного автомата. На ESP8266 обязательно вызывается в `loop()`.

#### Отправка запросов
* **`bool get(const String& url, AsyncHttpCallback cb = nullptr)`**  
  **`bool get(const char* url, AsyncHttpCallback cb = nullptr)`**  
  Постановка в очередь асинхронного GET-запроса.
* **`bool post(const String& url, const String& payload, const char* contentType = "application/x-www-form-urlencoded", AsyncHttpCallback cb = nullptr)`**  
  **`bool post(const char* url, const char* payload, const char* contentType = "application/x-www-form-urlencoded", AsyncHttpCallback cb = nullptr)`**  
  Постановка в очередь асинхронного POST-запроса с телом.
* **`bool send(const AsyncHttpRequest& req)`**  
  Универсальная отправка запроса с кастомным методом (`HTTP_GET`, `HTTP_POST`, `HTTP_PUT`, `HTTP_DELETE`, `HTTP_HEAD`, `HTTP_PATCH`) и произвольными заголовками.

#### Диагностика и статус
* **`bool isBusy() const`**  
  Возвращает `true`, если в данный момент выполняется запрос или очередь не пуста.
* **`bool isIdle() const`**  
  Возвращает `true`, если клиент свободен для выполнения новых запросов.
* **`size_t getQueueSize() const`**  
  Текущее количество запросов в очереди ожидания.
* **`HttpError getLastError() const`**  
  Код последней возникшей ошибки.
* **`String getLastErrorString() const`**  
  Текстовое описание последней ошибки.

#### Настройки и конфигурация
* **`void setKeepAlive(uint8_t seconds = 10)`**  
  Настройка соединения Keep-Alive:
  - `0` (или `false`) — выключено (`Connection: close`, сокет закрывается сразу).
  - `> 0` (или `true`) — включено, задаёт таймаут простоя в секундах до авто-закрытия (аппаратный лимит: максимум 10 сек).
* **`void setTimeout(uint32_t timeoutMs)`**  
  Таймаут ожидания подключения и приёма данных (по умолчанию 10 000 мс).
* **`void setFollowRedirects(bool enable, uint8_t maxRedirects = 3)`**  
  Автоматическое следование за HTTP перенаправлениями (301, 302, 303, 307, 308).
* **`void setMaxBodySize(size_t maxBytes)`**  
  Максимально допустимый размер тела ответа (по умолчанию 16 КБ) для защиты кучи от переполнения.
* **`void setMaxQueueSize(size_t sz)`**  
  Максимальная глубина очереди запросов (по умолчанию 16).
* **`void setBufferSizes(uint16_t rx, uint16_t tx)`**  
  Настройка буферов BearSSL для ESP8266 (по умолчанию 2048 RX, 512 TX).

---

### Структура ответа AsyncHttpResponse

В callback передаётся ссылка на объект `AsyncHttpResponse res`:

| Поле / Метод | Тип | Описание |
| :--- | :--- | :--- |
| `res.code` | `int` | Код ответа сервера (200, 404 и др.) либо код ошибки `< 0` |
| `res.body` | `String` | Тело ответа сервера |
| `res.contentType` | `String` | Значение заголовка `Content-Type` |
| `res.contentLength` | `int` | Длина тела из заголовка (-1 если chunked) |
| `res.headers` | `vector` | Список всех полученных HTTP-заголовков |
| `res.isSuccess()` | `bool` | Возвращает `true`, если код 2xx (успех) |
| `res.isError()` | `bool` | Возвращает `true`, если произошла сетевая ошибка (`code < 0`) |
| `res.errorString()` | `String` | Человекочитаемое описание статуса или ошибки |
| `res.getHeader("Name")` | `String` | Значение указанного заголовка ответа |
| `res.extract("key", isJson)` | `String` | Быстрое извлечение значения поля из JSON или XML |
| `res.parseJson(doc)` | `Error` | Парсинг тела в `JsonDocument` (ArduinoJson 7) |

---

### Коды ошибок HttpError

Если сетевой запрос завершился неудачно, `res.code` содержит отрицательное число:

| Код | Константа | Описание |
| :---: | :--- | :--- |
| `0` | `HTTP_OK` | Успешно |
| `-1` | `HTTP_ERR_NO_WIFI` | Нет подключения к точке доступа Wi-Fi |
| `-2` | `HTTP_ERR_INVALID_URL` | Некорректный или неподдерживаемый URL |
| `-3` | `HTTP_ERR_CONNECT` | Ошибка установки TCP/TLS соединения |
| `-4` | `HTTP_ERR_SEND` | Ошибка отправки пакета в сокет |
| `-5` | `HTTP_ERR_TIMEOUT` | Таймаут ожидания ответа сервера |
| `-6` | `HTTP_ERR_READ_TIMEOUT` | Обрыв связи во время чтения тела |
| `-7` | `HTTP_ERR_TOO_MANY_REDIRECTS` | Превышен лимит редиректов (301/302) |
| `-8` | `HTTP_ERR_TOO_LARGE` | Размер тела превысил лимит `maxBodySize` |
| `-9` | `HTTP_ERR_QUEUE_FULL` | Очередь запросов переполнена |
| `-10` | `HTTP_ERR_CANCELLED` | Запрос отменён вызовом `stop()` |
| `-11` | `HTTP_ERR_PARSE` | Ошибка парсинга служебных данных протокола |

---

## 📊 Сравнение со стандартным HTTPClient

| Параметр | Стандартный HTTPClient | asyncHTTP |
| :--- | :---: | :---: |
| Блокировка `loop()` | **Да (1–4 секунды)** | **Нет (0 мс)** |
| Замирание/пропуск событий | **Да** | **Полностью устранено** |
| Поддержка Chunked Encoding | Ограниченная | **Полная потоковая** |
| Повторное использование TLS (Keep-Alive) | Нет / Ручное | **Автоматическое с тайм-аутом** |
| Защита кучи от переполнения (OOM) | Нет | **Да (`maxBodySize`)** |
| Очередь запросов | Нет | **Да (FIFO Queue)** |
| Фоновый режим FreeRTOS (ESP32) | Нет | **Да (`AsyncHTTP(true)`)** |

---

## 📄 Лицензия

Распространяется под лицензией **MIT**. Автор: **Serhii Le**.
