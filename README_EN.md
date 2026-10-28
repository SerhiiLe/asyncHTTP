[English](README_EN.md) | [Русский](README.md)

# asyncHTTP 🚀

A lightweight, non-blocking asynchronous HTTP/HTTPS client library for **ESP8266** and **ESP32** with support for **JSON**, **XML**, **Chunked Transfer Encoding**, **HTTP Keep-Alive**, and a background **FreeRTOS task mode** on ESP32.

**Zero external dependencies** — built directly on official core sockets (`WiFiClient` for HTTP, `BearSSL::WiFiClientSecure` on ESP8266, and `WiFiClientSecure` on ESP32), driven by a fast, non-blocking cooperative Finite State Machine (FSM).

📖 Detailed internal design and FSM specifications are available in **[ARCHITECTURE_EN.md](ARCHITECTURE_EN.md)**.

---

## 📑 Table of Contents

1. [Key Features](#-key-features)
2. [Installation](#-installation)
3. [Quick Start](#-quick-start)
4. [Usage Examples](#-usage-examples)
   - [1. GET Request (Weather JSON with ArduinoJson)](#1-get-request-weather-json-with-arduinojson)
   - [2. GET Request with Fast Zero-Dependency Extraction](#2-get-request-with-fast-zero-dependency-extraction)
   - [3. POST Request (Form-Urlencoded and JSON)](#3-post-request-form-urlencoded-and-json)
   - [4. PUT and PATCH Requests](#4-put-and-patch-requests)
   - [5. DELETE and HEAD Requests](#5-delete-and-head-requests)
   - [6. Universal send() — The Gem of the Implementation](#6-universal-send--the-gem-of-the-implementation)
   - [7. Keep-Alive Request Pipeline](#7-keep-alive-request-pipeline)
5. [API Reference](#-api-reference)
   - [AsyncHTTP Class Methods](#asynchttp-class-methods)
   - [AsyncHttpRequest Request Structure](#asynchttprequest-request-structure)
   - [AsyncHttpResponse Response Structure](#asynchttpresponse-response-structure)
   - [AsyncHttpError Error Codes](#asynchttperror-error-codes)
6. [Comparison with Standard HTTPClient](#-comparison-with-standard-httpclient)
7. [License](#-license)

---

## ⚡ Key Features

* **Zero-Blocking FSM:** Each `httpClient.tick()` invocation runs in fractions of a microsecond. Headers and body bytes are consumed as they arrive in the socket buffer without delaying the main `loop()`.
* **Zero External Dependencies:** No third-party network libraries (`AsyncTCP`, `ESPAsyncTCP`). Only official, stable core sockets.
* **HTTP Keep-Alive with Auto-Timeout:** Holds the connection open across consecutive requests (e.g., weather + forecast to `api.open-meteo.com`), eliminating repeated heavy TLS handshakes (accelerating requests from ~1000 ms down to ~20–30 ms). On idle timeout, the socket automatically closes and memory is freed back to heap.
* **Chunked Transfer Encoding:** Full on-the-fly streaming chunk decoder (compatible with Open-Meteo, HTTPBin, and modern APIs).
* **Built-in Extractor & ArduinoJson Integration:**
  - Built-in `res.extract("field", isJson)` for instant zero-dependency key extraction from JSON/XML with automatic Unicode unescaping (`\u0430` -> UTF-8) and XML entities (`&quot;`, `&amp;`).
  - Seamless integration with `ArduinoJson 7` via `res.parseJson(doc)`.
* **Request FIFO Queue:** Requests are safely enqueued and processed sequentially with individual callback functions.
* **ESP32 FreeRTOS Task Mode:** Optional background worker on a dedicated core (`AsyncHTTP httpClient(true);`).
* **Automatic Redirects (301, 302, 303, 307, 308):** Follows `Location` headers with absolute and relative URL support.

---

## 📦 Installation

### PlatformIO
Add the library to the `lib_deps` section of your `platformio.ini`:
```ini
lib_deps =
    https://github.com/SerhiiLe/asyncHTTP.git
    bblanchon/ArduinoJson @ ^7.0.0  ; optional, for JSON parsing
```

### Arduino IDE
1. Download this repository as a `.ZIP` archive.
2. In Arduino IDE, navigate to: **Sketch → Include Library → Add .ZIP Library...**
3. Or clone directly into your libraries directory:
   ```bash
   cd ~/Arduino/libraries
   git clone https://github.com/SerhiiLe/asyncHTTP.git
   ```

---

## 🚀 Quick Start

```cpp
#include <Arduino.h>
#if defined(ESP8266)
    #include <ESP8266WiFi.h>
#elif defined(ESP32)
    #include <WiFi.h>
#endif
#include <AsyncHTTP.h>

// Client initialization
#if defined(ESP32)
AsyncHTTP httpClient(true); // runs in FreeRTOS worker task on ESP32
#else
AsyncHTTP httpClient;       // cooperative non-blocking FSM in loop() on ESP8266
#endif

void setup() {
    Serial.begin(115200);
    WiFi.begin("SSID", "PASSWORD");
    while (WiFi.status() != WL_CONNECTED) delay(500);

    httpClient.begin();
    httpClient.setKeepAlive(10); // Keep connection open for 10 seconds

    // Enqueue request
    httpClient.get("https://dummyjson.com/quotes/random", [](AsyncHttpResponse& res) {
        if (res.isSuccess()) {
            Serial.printf("Quote: %s\n", res.extract("quote").c_str());
        }
    });
}

void loop() {
    // Required on ESP8266 inside loop():
    httpClient.tick();

    // User code for displays, buttons, and animations runs without any freezes!
}
```

---

## 💡 Usage Examples

### 1. GET Request (Weather JSON with ArduinoJson)
```cpp
const char* url = "https://api.open-meteo.com/v1/forecast?latitude=46.48&longitude=30.74&current=temperature_2m,relative_humidity_2m";

httpClient.get(url, [](AsyncHttpResponse& res) {
    if (res.isSuccess()) {
        JsonDocument doc;
        DeserializationError err = res.parseJson(doc);
        if (!err) {
            float temp = doc["current"]["temperature_2m"];
            int hum = doc["current"]["relative_humidity_2m"];
            Serial.printf("Temperature: %.1f °C, Humidity: %d %%\n", temp, hum);
        }
    } else {
        Serial.printf("Error: %d (%s)\n", res.code, res.errorString().c_str());
    }
});
```

### 2. GET Request with Fast Zero-Dependency Extraction
Extract string or numeric values directly from JSON or XML without allocating a document tree:
```cpp
// JSON response
httpClient.get("https://dummyjson.com/quotes/random", [](AsyncHttpResponse& res) {
    if (res.isSuccess()) {
        String quote = res.extract("quote", true);   // true = JSON
        String author = res.extract("author", true);
        Serial.printf("\"%s\" — %s\n", quote.c_str(), author.c_str());
    }
});

// XML response
httpClient.get("https://example.com/feed.xml", [](AsyncHttpResponse& res) {
    if (res.isSuccess()) {
        String title = res.extract("title", false);  // false = XML
        Serial.printf("XML Title: %s\n", title.c_str());
    }
});
```

### 3. POST Request (Form-Urlencoded and JSON)
```cpp
// 1. Form-urlencoded parameters (default Content-Type)
httpClient.post("https://httpbin.org/post", "method=getQuote&lang=en", [](AsyncHttpResponse& res) {
    if (res.isSuccess()) {
        Serial.println("POST form successfully sent!");
    }
});

// 2. JSON Payload
String jsonPayload = "{\"device\":\"Clock_Mini\",\"temp\":22.4}";
httpClient.post("https://httpbin.org/post", jsonPayload, "application/json", [](AsyncHttpResponse& res) {
    if (res.isSuccess()) {
        Serial.println("JSON delivered successfully!");
    }
});
```

### 4. PUT and PATCH Requests
The `put()` and `patch()` methods work just like `post()`:
```cpp
// Update resource state (PUT)
httpClient.put("https://api.myserver.com/sensors/1", "{\"state\":\"ON\"}", "application/json", [](AsyncHttpResponse& res) {
    Serial.printf("PUT status: %d\n", res.code);
});

// Partial update (PATCH)
httpClient.patch("https://api.myserver.com/settings", "{\"brightness\":80}", "application/json", [](AsyncHttpResponse& res) {
    Serial.printf("PATCH status: %d\n", res.code);
});
```

### 5. DELETE and HEAD Requests
```cpp
// DELETE record
httpClient.del("https://api.myserver.com/records/42", [](AsyncHttpResponse& res) {
    if (res.isSuccess()) Serial.println("Record deleted");
});

// Query resource metadata without body download (HEAD)
// Per RFC 7231/9110, the message body is never transferred or waited for
httpClient.head("https://api.myserver.com/large-firmware.bin", [](AsyncHttpResponse& res) {
    if (res.isSuccess()) {
        Serial.printf("File size: %d bytes\n", res.contentLength);
        Serial.printf("Content-Type: %s\n", res.contentType.c_str());
    }
});
```

### 6. Universal send() — The Gem of the Implementation
The `send(req)` method is the centerpiece of the library. It provides exhaustive declarative control: custom HTTP headers, per-request custom timeouts, and any HTTP method.

```cpp
AsyncHttpRequest req;
req.method = ASYNC_HTTP_POST;
req.url = "https://api.myserver.com/v1/telemetry";
req.payload = "{\"status\":\"active\"}";
req.contentType = "application/json";

// Fluent header chaining
req.addHeader("Authorization", "Bearer secret_jwt_token_123")
   .addHeader("X-Device-Id", "ESP8266_Clock");

// Custom per-request timeout (overrides global httpClient timeout)
req.customTimeout = 5000; // 5 seconds

req.callback = [](AsyncHttpResponse& res) {
    if (res.isSuccess()) {
        Serial.printf("Server replied HTTP %d: %s\n", res.code, res.body.c_str());
    } else {
        Serial.printf("Error (%d): %s\n", res.code, res.errorString().c_str());
    }
};

httpClient.send(req);
```

### 7. Keep-Alive Request Pipeline
When enqueuing multiple requests targeting the same host, the TLS connection is established once and subsequent requests execute almost instantly:
```cpp
// Request 1: Current weather (TLS handshake ~400 ms)
httpClient.get("https://api.open-meteo.com/v1/forecast?latitude=46.48&longitude=30.74&current=temperature_2m", [](AsyncHttpResponse& res) {
    Serial.println("Current weather received");
});

// Request 2: Daily forecast (sent over the existing open socket in ~20 ms!)
httpClient.get("https://api.open-meteo.com/v1/forecast?latitude=46.48&longitude=30.74&daily=temperature_2m_max&forecast_days=3", [](AsyncHttpResponse& res) {
    Serial.println("Forecast received over reused socket!");
});
```

---

## 🛠️ API Reference

### AsyncHTTP Class Methods

#### Lifecycle Management
* **`AsyncHTTP(bool useTask = false)`**  
  Constructor. On ESP32, passing `useTask = true` spawns a dedicated FreeRTOS worker task.
* **`bool begin(int core = 1, uint32_t stackSize = 8192, int priority = 1)`**  
  Initializes the client and background task (on ESP32).
* **`void stop()`**  
  Stops operations, closes open sockets, and clears the FIFO queue.
* **`void tick()`**  
  FSM step execution. Must be called in `loop()` on ESP8266.

#### Dispatching Requests (Shortcuts and send)
All shortcut methods appear in editor autocomplete and forward to `send()`:

* **`bool get(url, cb = nullptr)`**  
  Asynchronous GET request (`String` or `const char*`).
* **`bool post(url, payload, contentType = "application/x-www-form-urlencoded", cb = nullptr)`**  
  Asynchronous POST request (`String` or `const char*`).
* **`bool put(url, payload, contentType = "application/x-www-form-urlencoded", cb = nullptr)`**  
  Asynchronous PUT request (`String` or `const char*`).
* **`bool patch(url, payload, contentType = "application/x-www-form-urlencoded", cb = nullptr)`**  
  Asynchronous PATCH request (`String` or `const char*`).
* **`bool del(url, cb = nullptr)`**  
  **`bool del(url, payload, contentType = "application/x-www-form-urlencoded", cb = nullptr)`**  
  Asynchronous DELETE request (with optional payload).
* **`bool head(url, cb = nullptr)`**  
  Asynchronous HEAD request for metadata inspection without body download.
* **`bool send(const AsyncHttpRequest& req)`**  
  Universal request dispatch with full declarative options.

#### Diagnostics and Status
* **`bool isBusy() const`**  
  Returns `true` if a request is actively processing or the queue is non-empty.
* **`bool isIdle() const`**  
  Returns `true` if the client is ready for new requests.
* **`size_t getQueueSize() const`**  
  Current count of pending requests in the FIFO queue.
* **`AsyncHttpError getLastError() const`**  
  Error code of the most recent failure.
* **`String getLastErrorString() const`**  
  Human-readable description of the last error.

#### Settings and Configuration
* **`void setKeepAlive(uint8_t seconds = 10)`**  
  Configures HTTP Keep-Alive:
  - `0` (or `false`) — disabled (`Connection: close`, socket terminates immediately).
  - `> 0` (or `true`) — enabled, sets idle timeout in seconds (maximum 10s hardware limit).
* **`void setTimeout(uint32_t timeoutMs)`**  
  Default connect and read timeout in milliseconds (default: 10,000 ms).
* **`void setFollowRedirects(bool enable, uint8_t maxRedirects = 3)`**  
  Enables automatic redirect following (301, 302, 303, 307, 308).
* **`void setMaxBodySize(size_t maxBytes)`**  
  Maximum response body size in bytes (default: 16 KB) to prevent heap exhaustion.
* **`void setMaxQueueSize(size_t sz)`**  
  Maximum FIFO queue depth (default: 16).
* **`void setBufferSizes(uint16_t rx, uint16_t tx)`**  
  Configures BearSSL buffer sizes on ESP8266 (default: 2048 RX, 512 TX).

---

### AsyncHttpRequest Request Structure

The **`send(const AsyncHttpRequest& req)`** method provides complete declarative control. All shortcut methods (`get`, `post`, `put`, `patch`, `del`, `head`) populate `AsyncHttpRequest` under the hood:

| Field / Method | Type | Default | Description |
| :--- | :--- | :---: | :--- |
| `req.method` | `AsyncHttpMethod` | `ASYNC_HTTP_GET` | HTTP method: `ASYNC_HTTP_GET`, `ASYNC_HTTP_POST`, `ASYNC_HTTP_PUT`, `ASYNC_HTTP_DELETE`, `ASYNC_HTTP_HEAD`, `ASYNC_HTTP_PATCH` |
| `req.url` | `String` | `""` | Full target URL (`http://` or `https://`, host, port, path, query parameters) |
| `req.payload` | `String` | `""` | Request body string (JSON, XML, or form-data for POST/PUT/PATCH/DELETE) |
| `req.contentType` | `String` | `"application/x-www-form-urlencoded"` | `Content-Type` header value (e.g. `"application/json"`, `"text/plain"`) |
| `req.headers` | `std::vector<AsyncHttpHeader>` | `{}` | List of custom HTTP headers (pairs `{name, value}`) |
| `req.addHeader(name, value)` | `AsyncHttpRequest&` | — | Helper to add a custom header (supports fluent chaining) |
| `req.clearHeaders()` | `void` | — | Clears all added headers |
| `req.callback` | `AsyncHttpCallback` | `nullptr` | Completion callback: `[](AsyncHttpResponse& res) { ... }` |
| `req.customTimeout` | `uint32_t` | `0` | Per-request timeout in milliseconds (`0` = use client default `httpClient.setTimeout()`) |
| `req.redirectCount` | `uint8_t` | `0` | Internal redirect hop counter (protects against infinite loops) |

---

### AsyncHttpResponse Response Structure

The callback receives a reference to `AsyncHttpResponse res`:

| Field / Method | Type | Description |
| :--- | :--- | :--- |
| `res.code` | `int` | HTTP status code (200, 404, etc.) or negative `AsyncHttpError` code |
| `res.body` | `String` | Response body payload |
| `res.contentType` | `String` | Value of `Content-Type` response header |
| `res.contentLength` | `int` | Payload length from header (-1 if chunked or unspecified) |
| `res.headers` | `vector` | List of all parsed response headers |
| `res.isSuccess()` | `bool` | Returns `true` if status code is 2xx |
| `res.isError()` | `bool` | Returns `true` on network failure (`code < 0`) |
| `res.errorString()` | `String` | Human-readable status or error description |
| `res.getHeader("Name")` | `String` | Returns value of specified response header |
| `res.extract("key", isJson)` | `String` | Fast zero-dependency extraction of field values from JSON or XML |
| `res.parseJson(doc)` | `Error` | Deserializes body into `JsonDocument` (ArduinoJson 7) |

---

### AsyncHttpError Error Codes

When a network operation fails, `res.code` contains a negative integer:

| Code | Constant | Description |
| :---: | :--- | :--- |
| `0` | `ASYNC_HTTP_OK` | Request succeeded |
| `-1` | `ASYNC_HTTP_ERR_NO_WIFI` | No active Wi-Fi connection |
| `-2` | `ASYNC_HTTP_ERR_INVALID_URL` | Malformed or unsupported URL |
| `-3` | `ASYNC_HTTP_ERR_CONNECT` | TCP/TLS connection failed |
| `-4` | `ASYNC_HTTP_ERR_SEND` | Failed to write packet to socket |
| `-5` | `ASYNC_HTTP_ERR_TIMEOUT` | Server response wait timeout |
| `-6` | `ASYNC_HTTP_ERR_READ_TIMEOUT` | Connection dropped during body read |
| `-7` | `ASYNC_HTTP_ERR_TOO_MANY_REDIRECTS` | Exceeded redirect limit (301/302) |
| `-8` | `ASYNC_HTTP_ERR_TOO_LARGE` | Response body exceeded `maxBodySize` |
| `-9` | `ASYNC_HTTP_ERR_QUEUE_FULL` | Request queue is full |
| `-10` | `ASYNC_HTTP_ERR_CANCELLED` | Request cancelled via `stop()` |
| `-11` | `ASYNC_HTTP_ERR_PARSE` | Protocol data parsing error |

---

## 📊 Comparison with Standard HTTPClient

| Feature | Standard HTTPClient | asyncHTTP |
| :--- | :---: | :---: |
| Blocks `loop()` | **Yes (1–4 seconds)** | **No (0 ms)** |
| UI Freezes & Dropped Events | **Yes** | **Completely Eliminated** |
| Chunked Transfer Encoding | Partial / Fragile | **Full Streaming Decoder** |
| TLS Session Reuse (Keep-Alive) | No / Manual | **Automatic with Idle Timeout** |
| Heap Overflow Protection (OOM) | No | **Yes (`maxBodySize`)** |
| Outbound Request FIFO Queue | No | **Yes** |
| FreeRTOS Background Task Mode | No | **Yes (`AsyncHTTP(true)`)** |

---

## 📄 License

Distributed under the **LGPL-3.0** License. You are free to use it in open-source and commercial microcontroller projects.
