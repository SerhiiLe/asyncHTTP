/**
 * asyncHTTP - QuickStart Example
 * 
 * Demonstrates basic non-blocking GET requests using asyncHTTP.
 * Works seamlessly on ESP8266 and ESP32 with zero external dependencies.
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

// Enter your Wi-Fi credentials
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// Create client instance.
// On ESP32 passing 'true' runs the FSM in a separate FreeRTOS task.
// On ESP8266 the cooperative FSM runs via tick() in loop().
#if defined(ESP32)
AsyncHTTP httpClient(true);
#else
AsyncHTTP httpClient;
#endif

// Performance monitor variables to prove loop() never freezes
uint32_t lastPrintTime = 0;
uint32_t loopIterations = 0;

void requestQuote() {
    Serial.println(F("[QuickStart] Sending asynchronous GET request..."));

    // Fetch random quote from DummyJSON (HTTPS)
    httpClient.get("https://dummyjson.com/quotes/random", [](AsyncHttpResponse& res) {
        if (res.isSuccess()) {
            Serial.println(F("\n--- Response Received ---"));
            Serial.printf("Status Code : %d\n", res.code);

            // Fast, zero-dependency field extraction with automatic Unicode unescaping:
            String quote = res.extract("quote", true);
            String author = res.extract("author", true);

            Serial.printf("Quote       : \"%s\"\n", quote.c_str());
            Serial.printf("Author      : %s\n", author.c_str());
            Serial.println(F("-------------------------\n"));
        } else {
            Serial.printf("[QuickStart] Request failed: %d (%s)\n", 
                          res.code, res.errorString().c_str());
        }
    });
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println(F("\n=== asyncHTTP QuickStart ==="));

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    Serial.print(F("Connecting to WiFi"));
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print('.');
    }
    Serial.println(F("\nConnected to WiFi!"));
    Serial.print(F("IP Address: "));
    Serial.println(WiFi.localIP());

    // Initialize asyncHTTP client
    httpClient.begin();
    httpClient.setTimeout(10000);  // 10-second timeout
    httpClient.setKeepAlive(10);   // Keep connection open for 10 seconds

    // Dispatch our first request
    requestQuote();
}

void loop() {
    // Required on ESP8266 (cooperative non-blocking state machine).
    // On ESP32 with useTask=true, this is optional but safe to call.
    httpClient.tick();

    loopIterations++;

    // Check how many times loop() runs per second without blocking
    if (millis() - lastPrintTime >= 2000) {
        lastPrintTime = millis();
        Serial.printf("[LOOP MONITOR] %u iterations/sec | Free Heap: %u bytes | Client: %s\n",
                      loopIterations / 2,
                      (unsigned)ESP.getFreeHeap(),
                      httpClient.isBusy() ? "BUSY" : "IDLE");
        loopIterations = 0;
    }
}
