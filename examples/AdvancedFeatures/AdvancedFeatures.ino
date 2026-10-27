/**
 * asyncHTTP - AdvancedFeatures Example
 * 
 * Demonstrates the full capabilities of asyncHTTP:
 * - Keep-Alive connection reuse (fast pipelining without repeated TLS handshakes)
 * - HTTP methods: GET, POST, PUT, PATCH, DELETE, HEAD
 * - Declarative send() method with custom headers, chaining and timeouts
 * - Zero-dependency field extraction (res.extract) vs ArduinoJson 7 integration (res.parseJson)
 * - Automatic redirect following and non-blocking FIFO queue
 * 
 * Licensed under LGPL-3.0
 */

#include <Arduino.h>

#if defined(ESP8266)
    #include <ESP8266WiFi.h>
#elif defined(ESP32)
    #include <WiFi.h>
#endif

#include <AsyncHTTP.h>
#include <ArduinoJson.h>

// Enter your Wi-Fi credentials
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// Client instance
#if defined(ESP32)
AsyncHTTP httpClient(true); // runs in FreeRTOS worker task on ESP32
#else
AsyncHTTP httpClient;       // cooperative non-blocking FSM on ESP8266
#endif

// Performance monitoring
uint32_t lastPrintTime = 0;
uint32_t loopIterations = 0;

// 1. Keep-Alive Pipeline: Requests to the same host reuse the active TLS session
void runKeepAlivePipeline() {
    Serial.println(F("\n[Demo 1] Keep-Alive Pipeline to api.open-meteo.com"));

    // Request A: Current weather (initial TLS handshake)
    const char* urlWeather = "https://api.open-meteo.com/v1/forecast?latitude=46.48&longitude=30.74&current=temperature_2m,relative_humidity_2m";
    httpClient.get(urlWeather, [](AsyncHttpResponse& res) {
        if (res.isSuccess()) {
            JsonDocument doc;
            DeserializationError err = res.parseJson(doc);
            if (!err) {
                float temp = doc["current"]["temperature_2m"];
                int hum = doc["current"]["relative_humidity_2m"];
                Serial.printf("[Keep-Alive 1] Weather -> Temp: %.1f °C, Humidity: %d %%\n", temp, hum);
            }
        } else {
            Serial.printf("[Keep-Alive 1] Failed: %d (%s)\n", res.code, res.errorString().c_str());
        }
    });

    // Request B: Forecast (queued and sent immediately over the same TLS connection!)
    const char* urlForecast = "https://api.open-meteo.com/v1/forecast?latitude=46.48&longitude=30.74&daily=temperature_2m_max&forecast_days=1";
    httpClient.get(urlForecast, [](AsyncHttpResponse& res) {
        if (res.isSuccess()) {
            Serial.printf("[Keep-Alive 2] Forecast fetched over reused socket! Body size: %u bytes\n", 
                          (unsigned)res.body.length());
        } else {
            Serial.printf("[Keep-Alive 2] Failed: %d (%s)\n", res.code, res.errorString().c_str());
        }
    });
}

// 2. POST, PUT, PATCH, DELETE and HEAD shortcuts
void runHttpMethodsDemo() {
    Serial.println(F("\n[Demo 2] Enqueuing POST, PUT, PATCH, DELETE and HEAD requests..."));

    // POST Form-Urlencoded
    httpClient.post("https://httpbin.org/post", "user=serhii&action=login", [](AsyncHttpResponse& res) {
        Serial.printf("[POST Form] Code: %d, Success: %s\n", res.code, res.isSuccess() ? "YES" : "NO");
    });

    // POST JSON
    httpClient.post("https://httpbin.org/post", "{\"status\":\"active\"}", "application/json", [](AsyncHttpResponse& res) {
        Serial.printf("[POST JSON] Code: %d, Response size: %u\n", res.code, (unsigned)res.body.length());
    });

    // PUT
    httpClient.put("https://httpbin.org/put", "{\"sensor\":1,\"val\":25.4}", "application/json", [](AsyncHttpResponse& res) {
        Serial.printf("[PUT] Code: %d\n", res.code);
    });

    // PATCH
    httpClient.patch("https://httpbin.org/patch", "{\"val\":26.0}", "application/json", [](AsyncHttpResponse& res) {
        Serial.printf("[PATCH] Code: %d\n", res.code);
    });

    // DELETE
    httpClient.del("https://httpbin.org/delete", [](AsyncHttpResponse& res) {
        Serial.printf("[DELETE] Code: %d\n", res.code);
    });

    // HEAD - Only fetches headers according to RFC 7231 (never downloads or waits for body)
    httpClient.head("https://httpbin.org/get", [](AsyncHttpResponse& res) {
        if (res.isSuccess()) {
            Serial.printf("[HEAD] Code: %d, Content-Type: %s\n", 
                          res.code, res.contentType.c_str());
        }
    });
}

// 3. Universal send() - The Gem of asyncHTTP: complete declarative control
void runUniversalSendDemo() {
    Serial.println(F("\n[Demo 3] Universal send() with custom headers, chaining & per-request timeout"));

    AsyncHttpRequest req;
    req.method = HTTP_POST;
    req.url = "https://httpbin.org/post";
    req.payload = "{\"device\":\"ESP_Clock\",\"boot_count\":42}";
    req.contentType = "application/json";

    // Fluent header chaining
    req.addHeader("Authorization", "Bearer my_secure_jwt_token_xyz")
       .addHeader("X-Custom-Client", "asyncHTTP-v1.0")
       .addHeader("Accept-Language", "en,uk;q=0.9");

    // Custom per-request timeout in milliseconds (overrides global httpClient timeout)
    req.customTimeout = 7000;

    req.callback = [](AsyncHttpResponse& res) {
        if (res.isSuccess()) {
            Serial.println(F("\n--- [send()] Request Succeeded ---"));
            Serial.printf("Status Code : %d\n", res.code);
            Serial.printf("Server      : %s\n", res.getHeader("Server").c_str());

            // Extract nested field without loading full JSON AST
            String origin = res.extract("origin", true);
            Serial.printf("Client IP   : %s\n", origin.c_str());
            Serial.println(F("----------------------------------\n"));
        } else {
            Serial.printf("[send()] Failed with error %d: %s\n", 
                          res.code, res.errorString().c_str());
        }
    };

    httpClient.send(req);
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println(F("\n=== asyncHTTP AdvancedFeatures Demo ==="));

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    Serial.print(F("Connecting to WiFi"));
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print('.');
    }
    Serial.println(F("\nConnected to WiFi!"));

    // Configure asyncHTTP
    httpClient.begin();
    httpClient.setTimeout(10000); // 10s default timeout
    httpClient.setKeepAlive(10);  // Keep-Alive up to 10 seconds idle timeout
    httpClient.setFollowRedirects(true, 3); // Auto follow redirects up to 3 hops
    httpClient.setMaxBodySize(16384);       // Protect heap from payloads > 16 KB

    // Dispatch all demo batches into the non-blocking FIFO queue
    runKeepAlivePipeline();
    runHttpMethodsDemo();
    runUniversalSendDemo();
}

void loop() {
    // Non-blocking step
    httpClient.tick();

    loopIterations++;

    if (millis() - lastPrintTime >= 2000) {
        lastPrintTime = millis();
        Serial.printf("[LOOP MONITOR] %u iter/sec | Free Heap: %u bytes | Client: %s | Queue: %u\n",
                      loopIterations / 2,
                      (unsigned)ESP.getFreeHeap(),
                      httpClient.isBusy() ? "BUSY" : "IDLE",
                      (unsigned)httpClient.getQueueSize());
        loopIterations = 0;
    }
}
