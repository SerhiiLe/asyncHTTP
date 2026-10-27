#ifndef ASYNC_HTTP_H
#define ASYNC_HTTP_H

#include <Arduino.h>
#include <queue>
#include "AsyncHTTPTypes.h"

#if defined(ESP8266)
    #include <ESP8266WiFi.h>
    #include <WiFiClient.h>
    #include <WiFiClientSecure.h>
    #include <WiFiClientSecureBearSSL.h>
    using SecureClient = BearSSL::WiFiClientSecure;
#elif defined(ESP32)
    #include <WiFi.h>
    #include <WiFiClient.h>
    #include <WiFiClientSecure.h>
    #include <freertos/FreeRTOS.h>
    #include <freertos/task.h>
    using SecureClient = WiFiClientSecure;
#else
    #error "Platform not supported! Only ESP8266 and ESP32 are supported."
#endif

#ifdef LOG_ASYNC_HTTP
#undef LOG_ASYNC_HTTP
#endif

#if defined(DEBUG) || defined(HTTP_DEBUG)
    #define LOG_ASYNC_HTTP(func, ...) Serial.func(__VA_ARGS__)
#else
    #define LOG_ASYNC_HTTP(func, ...) ;
#endif

class AsyncHTTP {
public:
    // Конструктор: на ESP32 параметр useTask=true запускает конечный автомат
    // в отдельной фоновой задаче FreeRTOS (на ESP8266 tick() вызывается в loop)
    AsyncHTTP(bool useTask = false) : _clientHttp(), _clientHttps() {
#if defined(ESP32)
        _useTask = useTask;
#else
        (void)useTask;
#endif
#if defined(ESP8266)
        _clientHttps.setInsecure();
        _clientHttps.setBufferSizes(_rxBufferSize, _txBufferSize);
#elif defined(ESP32)
        _clientHttps.setInsecure();
#endif
    }

    ~AsyncHTTP() {
        stop();
    }

    // Инициализация и запуск библиотеки (и фоновой задачи FreeRTOS на ESP32)
#if defined(ESP32)
    bool begin(BaseType_t core = 1, uint32_t stackSize = 8192, UBaseType_t priority = 1) {
        _started = true;
        _state = STATE_IDLE;
        _lastError = HTTP_OK;

        if (_useTask) {
            if (_taskHandle != nullptr) return true; // Уже запущена
            _running = true;
            BaseType_t res = xTaskCreatePinnedToCore(
                _taskWorker,
                "AsyncHttpTask",
                stackSize,
                this,
                priority,
                &_taskHandle,
                core
            );
            if (res != pdPASS) {
                _running = false;
                _taskHandle = nullptr;
                return false;
            }
        }
        return true;
    }
#else
    bool begin(int core = 1, uint32_t stackSize = 8192, int priority = 1) {
        (void)core; (void)stackSize; (void)priority;
        _started = true;
        _state = STATE_IDLE;
        _lastError = HTTP_OK;
        return true;
    }
#endif

    // Остановка всех сетевых операций, очистка очереди и закрытие соединений
    void stop() {
        _started = false;

#if defined(ESP32)
        if (_taskHandle != nullptr) {
            _running = false;
            if (xTaskGetCurrentTaskHandle() != _taskHandle) {
                uint32_t waitTimeout = millis() + 1000;
                while (_taskHandle != nullptr && millis() < waitTimeout) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                if (_taskHandle != nullptr) {
                    vTaskDelete(_taskHandle);
                    _taskHandle = nullptr;
                }
            }
        }
#endif

        if (_activeClient && _activeClient->connected()) {
            _activeClient->stop();
        }
        _activeClient = nullptr;
        _connectedHost = "";
        _connectedPort = 0;
        _state = STATE_IDLE;
        _clearQueue();
    }

    // Неблокирующий шаг конечного автомата.
    // На ESP8266 обязательно вызывается в loop().
    // На ESP32 при useTask=true вызывается автоматически воркером FreeRTOS.
    void tick() {
        if (!_started) return;

        if (WiFi.status() != WL_CONNECTED) {
            if (_state != STATE_IDLE) {
                _setError(HTTP_ERR_NO_WIFI);
                if (_activeClient && _activeClient->connected()) {
                    _activeClient->stop();
                }
                _finishRequest();
            }
            return;
        }

        switch (_state) {
            case STATE_IDLE:
                _handleIdleState();
                break;

            case STATE_CONNECTING:
                _handleConnecting();
                break;

            case STATE_SENDING:
                _handleSending();
                break;

            case STATE_WAIT_HEADERS:
                _handleWaitHeaders();
                break;

            case STATE_READ_HEADERS:
                _handleReadHeaders();
                break;

            case STATE_READ_BODY:
                if (_isChunked) {
                    _handleReadBodyChunked();
                } else if (_contentLength >= 0) {
                    _handleReadBodyContentLength();
                } else {
                    _handleReadBodyEof();
                }
                break;
        }
    }

    // --- Методы отправки HTTP-запросов ---

    // Асинхронный GET запрос (String)
    bool get(const String& url, AsyncHttpCallback cb = nullptr) {
        AsyncHttpRequest req;
        req.method = HTTP_GET;
        req.url = url;
        req.callback = cb;
        return _enqueueRequest(req);
    }

    // Асинхронный GET запрос (const char*)
    bool get(const char* url, AsyncHttpCallback cb = nullptr) {
        return get(String(url), cb);
    }

    // Асинхронный POST запрос (String)
    bool post(const String& url, const String& payload, 
              const char* contentType = "application/x-www-form-urlencoded", 
              AsyncHttpCallback cb = nullptr) {
        AsyncHttpRequest req;
        req.method = HTTP_POST;
        req.url = url;
        req.payload = payload;
        req.contentType = contentType;
        req.callback = cb;
        return _enqueueRequest(req);
    }

    // Асинхронный POST запрос (const char*)
    bool post(const char* url, const char* payload, 
              const char* contentType = "application/x-www-form-urlencoded", 
              AsyncHttpCallback cb = nullptr) {
        return post(String(url), String(payload), contentType, cb);
    }

    // Универсальный асинхронный запрос с расширенными параметрами и заголовками
    bool send(const AsyncHttpRequest& req) {
        return _enqueueRequest(req);
    }

    // --- Настройки и конфигурация ---

    // Установка таймаута ожидания и чтения (в миллисекундах, по умолчанию 10000)
    void setTimeout(uint32_t timeoutMs) {
        _defaultTimeoutMs = timeoutMs;
    }

    // Настройка постоянного соединения HTTP Keep-Alive.
    // 0 (или false) — выключено (Connection: close, сокет закрывается сразу).
    // > 0 (или true) — включено, задаёт таймаут ожидания повторных запросов в секундах (максимум 10 сек).
    void setKeepAlive(uint8_t seconds = 10) {
        if (seconds == 0) {
            _keepAliveTimeoutSec = 0;
            if (_activeClient && _activeClient->connected()) {
                _activeClient->stop();
                _activeClient = nullptr;
                _connectedHost = "";
                _connectedPort = 0;
            }
        } else if (seconds == 1) {
            // Поддержка вызова setKeepAlive(true) -> 10 секунд по умолчанию
            _keepAliveTimeoutSec = 10;
        } else {
            // Ограничиваем максимум 10 секундами
            _keepAliveTimeoutSec = (seconds > 10) ? 10 : seconds;
        }
    }

    // Настройка автоматического перехода по перенаправлениям (301, 302, 303, 307, 308)
    void setFollowRedirects(bool enable, uint8_t maxRedirects = 3) {
        _followRedirects = enable;
        _maxRedirects = maxRedirects;
    }

    // Максимальный размер тела ответа для защиты кучи от переполнения (по умолчанию 16 КБ)
    void setMaxBodySize(size_t maxBytes) {
        _maxBodySize = maxBytes;
    }

    // Максимальный размер очереди исходящих запросов
    void setMaxQueueSize(size_t sz) {
        _maxQueueSize = sz;
    }

    // Настройка буферов BearSSL для ESP8266 (rx, tx). По умолчанию 2048, 512.
    void setBufferSizes(uint16_t rx, uint16_t tx) {
        _rxBufferSize = rx;
        _txBufferSize = tx;
#if defined(ESP8266)
        _clientHttps.setBufferSizes(_rxBufferSize, _txBufferSize);
#endif
    }

    // --- Статус и диагностика ---

    // Занят ли клиент выполнением запроса
    bool isBusy() const {
        return (_state != STATE_IDLE || !_isQueueEmpty());
    }

    // Свободен ли клиент для новых запросов
    bool isIdle() const {
        return (_state == STATE_IDLE && _isQueueEmpty());
    }

    // Текущее количество запросов в очереди
    size_t getQueueSize() const {
        _lock();
        size_t sz = _queue.size();
        _unlock();
        return sz;
    }

    // Код последней ошибки
    HttpError getLastError() const {
        return _lastError;
    }

    // Текстовое описание последней ошибки
    String getLastErrorString() const {
        AsyncHttpResponse r;
        r.code = _lastError;
        return r.errorString();
    }

private:
    enum State {
        STATE_IDLE,
        STATE_CONNECTING,
        STATE_SENDING,
        STATE_WAIT_HEADERS,
        STATE_READ_HEADERS,
        STATE_READ_BODY
    };

    enum ChunkState {
        CHUNK_STATE_SIZE,
        CHUNK_STATE_DATA,
        CHUNK_STATE_CRLF,
        CHUNK_STATE_TRAILERS
    };

    WiFiClient _clientHttp;
    SecureClient _clientHttps;
    Client* _activeClient = nullptr;

    std::queue<AsyncHttpRequest> _queue;
    size_t _maxQueueSize = 16;
    AsyncHttpRequest _currentReq;
    ParsedUrl _currentUrl;
    AsyncHttpResponse _response;

    State _state = STATE_IDLE;
    ChunkState _chunkState = CHUNK_STATE_SIZE;
    size_t _chunkRemaining = 0;
    String _chunkLine;

    String _headerLine;
    int _httpStatusCode = 0;
    int _contentLength = -1;
    bool _isChunked = false;
    bool _connectionClose = false;
    String _location;

    uint32_t _stateStartTime = 0;
    uint32_t _defaultTimeoutMs = 10000;
    uint32_t _timeoutMs = 10000;
    size_t _maxBodySize = 16384;

    uint8_t _keepAliveTimeoutSec = 10;
    uint32_t _idleStartTime = 0;
    bool _followRedirects = true;
    uint8_t _maxRedirects = 3;

    String _connectedHost;
    uint16_t _connectedPort = 0;
    bool _connectedIsHttps = false;

    bool _started = false;
    HttpError _lastError = HTTP_OK;

    uint16_t _rxBufferSize = 2048;
    uint16_t _txBufferSize = 512;

#if defined(ESP32)
    bool _useTask = false;
    volatile bool _running = false;
    TaskHandle_t _taskHandle = nullptr;
    mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

    void _lock() const { portENTER_CRITICAL(&_mux); }
    void _unlock() const { portEXIT_CRITICAL(&_mux); }

    static void _taskWorker(void* param) {
        AsyncHTTP* self = static_cast<AsyncHTTP*>(param);
        while (self->_running) {
            self->tick();
            if (self->isIdle()) {
                vTaskDelay(pdMS_TO_TICKS(10));
            } else {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }
        self->_taskHandle = nullptr;
        vTaskDelete(NULL);
    }
#else
    void _lock() const {}
    void _unlock() const {}
#endif

    bool _isQueueEmpty() const {
        _lock();
        bool empty = _queue.empty();
        _unlock();
        return empty;
    }

    void _clearQueue() {
        _lock();
        while (!_queue.empty()) {
            AsyncHttpRequest req = _queue.front();
            _queue.pop();
            if (req.callback) {
                AsyncHttpResponse r;
                r.code = HTTP_ERR_CANCELLED;
                req.callback(r);
            }
        }
        _unlock();
    }

    void _setError(HttpError err) {
        _lastError = err;
        _response.code = err;
    }

    bool _enqueueRequest(const AsyncHttpRequest& req) {
        if (!_started) {
            begin();
        }
        _lock();
        if (_queue.size() >= _maxQueueSize) {
            _unlock();
            LOG_ASYNC_HTTP(println, F("[AsyncHTTP] Queue full! Dropping request."));
            if (req.callback) {
                AsyncHttpResponse errRes;
                errRes.code = HTTP_ERR_QUEUE_FULL;
                req.callback(errRes);
            }
            return false;
        }
        _queue.push(req);
        _unlock();
        return true;
    }

    void _handleIdleState() {
        if (_keepAliveTimeoutSec > 0 && _activeClient) {
            if (!_activeClient->connected()) {
                _activeClient->stop();
                _activeClient = nullptr;
                _connectedHost = "";
                _connectedPort = 0;
            } else if (millis() - _idleStartTime >= (uint32_t)_keepAliveTimeoutSec * 1000UL) {
                LOG_ASYNC_HTTP(printf_P, PSTR("[AsyncHTTP] Keep-Alive idle timeout (%u s). Closing socket.\n"),
                               (unsigned)_keepAliveTimeoutSec);
                _activeClient->stop();
                _activeClient = nullptr;
                _connectedHost = "";
                _connectedPort = 0;
            }
        }
        _processQueue();
    }

    void _processQueue() {
        if (_state != STATE_IDLE) return;

        _lock();
        if (_queue.empty()) {
            _unlock();
            return;
        }
        _currentReq = _queue.front();
        _queue.pop();
        _unlock();

        _startRequest();
    }

    void _startRequest() {
        _currentUrl = ParsedUrl::parse(_currentReq.url);
        if (!_currentUrl.valid) {
            LOG_ASYNC_HTTP(printf_P, PSTR("[AsyncHTTP] Invalid URL: %s\n"), _currentReq.url.c_str());
            _setError(HTTP_ERR_INVALID_URL);
            _finishRequest();
            return;
        }

        if (_currentUrl.isHttps) {
            _activeClient = &_clientHttps;
        } else {
            _activeClient = &_clientHttp;
        }

        _timeoutMs = (_currentReq.customTimeout > 0) ? _currentReq.customTimeout : _defaultTimeoutMs;
        _activeClient->setTimeout(_timeoutMs);

        // Проверяем, открыто ли уже соединение по Keep-Alive к этому же серверу
        if (_keepAliveTimeoutSec > 0 && _activeClient && _activeClient->connected() &&
            _connectedHost == _currentUrl.host &&
            _connectedPort == _currentUrl.port &&
            _connectedIsHttps == _currentUrl.isHttps) {
            LOG_ASYNC_HTTP(printf_P, PSTR("[AsyncHTTP] Reusing Keep-Alive connection to %s:%u\n"),
                           _currentUrl.host.c_str(), _currentUrl.port);
            _state = STATE_SENDING;
            _stateStartTime = millis();
            _handleSending();
        } else {
            if (_activeClient && _activeClient->connected()) {
                _activeClient->stop();
            }
            _connectedHost = "";
            _connectedPort = 0;
            _state = STATE_CONNECTING;
            _stateStartTime = millis();
            _handleConnecting();
        }
    }

    void _handleConnecting() {
        LOG_ASYNC_HTTP(printf_P, PSTR("[AsyncHTTP] Connecting to %s:%u (%s)...\n"),
                       _currentUrl.host.c_str(), _currentUrl.port, _currentUrl.isHttps ? "HTTPS" : "HTTP");

        bool ok = _activeClient->connect(_currentUrl.host.c_str(), _currentUrl.port);
        if (!ok) {
            LOG_ASYNC_HTTP(println, F("[AsyncHTTP] Connection failed!"));
            _setError(HTTP_ERR_CONNECT);
            _finishRequest();
            return;
        }

        LOG_ASYNC_HTTP(printf_P, PSTR("[AsyncHTTP] Connected! Free Heap: %u\n"), ESP.getFreeHeap());
        _connectedHost = _currentUrl.host;
        _connectedPort = _currentUrl.port;
        _connectedIsHttps = _currentUrl.isHttps;

        _state = STATE_SENDING;
        _stateStartTime = millis();
        _handleSending();
    }

    void _handleSending() {
        const char* methodStr = "GET";
        switch (_currentReq.method) {
            case HTTP_GET: methodStr = "GET"; break;
            case HTTP_POST: methodStr = "POST"; break;
            case HTTP_PUT: methodStr = "PUT"; break;
            case HTTP_DELETE: methodStr = "DELETE"; break;
            case HTTP_HEAD: methodStr = "HEAD"; break;
            case HTTP_PATCH: methodStr = "PATCH"; break;
        }

        String req;
        req.reserve(256 + _currentReq.payload.length());
        req += methodStr;
        req += ' ';
        req += _currentUrl.path;
        req += F(" HTTP/1.1\r\nHost: ");
        req += _currentUrl.host;
        if ((!_currentUrl.isHttps && _currentUrl.port != 80) || 
            (_currentUrl.isHttps && _currentUrl.port != 443)) {
            req += ':';
            req += String(_currentUrl.port);
        }
        req += F("\r\nUser-Agent: AsyncHTTP/1.0\r\nAccept: */*\r\n");

        if (_keepAliveTimeoutSec > 0) {
            req += F("Connection: keep-alive\r\n");
        } else {
            req += F("Connection: close\r\n");
        }

        if (_currentReq.payload.length() > 0 || _currentReq.method == HTTP_POST || _currentReq.method == HTTP_PUT) {
            if (_currentReq.contentType.length() > 0) {
                req += F("Content-Type: ");
                req += _currentReq.contentType;
                req += F("\r\n");
            }
            req += F("Content-Length: ");
            req += String(_currentReq.payload.length());
            req += F("\r\n");
        }

        for (const auto& h : _currentReq.headers) {
            req += h.name;
            req += F(": ");
            req += h.value;
            req += F("\r\n");
        }

        req += F("\r\n");
        if (_currentReq.payload.length() > 0) {
            req += _currentReq.payload;
        }

        LOG_ASYNC_HTTP(printf_P, PSTR("[AsyncHTTP] -> %s %s (body %u bytes)\n"),
                       methodStr, _currentUrl.path.c_str(), (unsigned)_currentReq.payload.length());

        size_t written = _activeClient->print(req);
        _activeClient->flush();

        if (written == 0) {
            LOG_ASYNC_HTTP(println, F("[AsyncHTTP] Send failed!"));
            _setError(HTTP_ERR_SEND);
            _finishRequest();
            return;
        }

        _response = AsyncHttpResponse();
        _headerLine = "";
        _httpStatusCode = 0;
        _contentLength = -1;
        _isChunked = false;
        _connectionClose = false;
        _location = "";

        _state = STATE_WAIT_HEADERS;
        _stateStartTime = millis();
    }

    void _handleWaitHeaders() {
        if (!_activeClient->connected() && !_activeClient->available()) {
            LOG_ASYNC_HTTP(println, F("[AsyncHTTP] Connection dropped waiting for response!"));
            _setError(HTTP_ERR_CONNECT);
            _finishRequest();
            return;
        }

        if (millis() - _stateStartTime > _timeoutMs) {
            LOG_ASYNC_HTTP(println, F("[AsyncHTTP] Response headers timeout!"));
            _setError(HTTP_ERR_TIMEOUT);
            _finishRequest();
            return;
        }

        if (_activeClient->available() > 0) {
            _state = STATE_READ_HEADERS;
            _stateStartTime = millis();
            _handleReadHeaders();
        }
    }

    void _handleReadHeaders() {
        while (_activeClient->available() > 0) {
            char c = (char)_activeClient->read();
            _headerLine += c;
            if (c == '\n') {
                String line = _headerLine;
                line.trim();
                _headerLine = "";

                if (line.length() == 0) {
                    // Пустая строка означает конец блока HTTP-заголовков
                    _onHeadersFinished();
                    return;
                }

                if (line.startsWith(F("HTTP/1."))) {
                    int sp = line.indexOf(' ');
                    if (sp > 0) {
                        _httpStatusCode = line.substring(sp + 1, sp + 4).toInt();
                        _response.code = _httpStatusCode;
                    }
                } else {
                    int colon = line.indexOf(':');
                    if (colon > 0) {
                        String name = line.substring(0, colon);
                        String val = line.substring(colon + 1);
                        name.trim();
                        val.trim();

                        if (name.equalsIgnoreCase(F("Content-Length"))) {
                            _contentLength = val.toInt();
                            _response.contentLength = _contentLength;
                        } else if (name.equalsIgnoreCase(F("Transfer-Encoding"))) {
                            if (val.indexOf(F("chunked")) >= 0) {
                                _isChunked = true;
                            }
                        } else if (name.equalsIgnoreCase(F("Connection"))) {
                            if (val.equalsIgnoreCase(F("close"))) {
                                _connectionClose = true;
                            }
                        } else if (name.equalsIgnoreCase(F("Content-Type"))) {
                            _response.contentType = val;
                        } else if (name.equalsIgnoreCase(F("Location"))) {
                            _location = val;
                        }

                        _response.headers.push_back({name, val});
                    }
                }
            }
        }

        if (millis() - _stateStartTime > _timeoutMs) {
            LOG_ASYNC_HTTP(println, F("[AsyncHTTP] Read headers timeout!"));
            _setError(HTTP_ERR_READ_TIMEOUT);
            _finishRequest();
        }
    }

    void _onHeadersFinished() {
        LOG_ASYNC_HTTP(printf_P, PSTR("[AsyncHTTP] <- HTTP %d, Content-Length: %d, Chunked: %d\n"),
                       _httpStatusCode, _contentLength, _isChunked ? 1 : 0);

        // Обработка автоматического перенаправления (301, 302, 303, 307, 308)
        if ((_httpStatusCode == 301 || _httpStatusCode == 302 || _httpStatusCode == 303 || 
             _httpStatusCode == 307 || _httpStatusCode == 308) && _location.length() > 0) {
            if (_followRedirects && _currentReq.redirectCount < _maxRedirects) {
                _currentReq.redirectCount++;
                if (_location.startsWith(F("http://")) || _location.startsWith(F("https://"))) {
                    _currentReq.url = _location;
                } else if (_location.startsWith(F("/"))) {
                    String scheme = _currentUrl.isHttps ? F("https://") : F("http://");
                    _currentReq.url = scheme + _currentUrl.host;
                    if ((!_currentUrl.isHttps && _currentUrl.port != 80) || 
                        (_currentUrl.isHttps && _currentUrl.port != 443)) {
                        _currentReq.url += ':';
                        _currentReq.url += String(_currentUrl.port);
                    }
                    _currentReq.url += _location;
                }

                LOG_ASYNC_HTTP(printf_P, PSTR("[AsyncHTTP] Following redirect (%d) to: %s\n"),
                               _httpStatusCode, _currentReq.url.c_str());

                ParsedUrl newUrl = ParsedUrl::parse(_currentReq.url);
                if (!newUrl.valid) {
                    _setError(HTTP_ERR_INVALID_URL);
                    _finishRequest();
                    return;
                }

                if (newUrl.host != _currentUrl.host || newUrl.port != _currentUrl.port || 
                    newUrl.isHttps != _currentUrl.isHttps) {
                    if (_activeClient && _activeClient->connected()) {
                        _activeClient->stop();
                    }
                }

                _currentUrl = newUrl;
                if (_currentUrl.isHttps) _activeClient = &_clientHttps;
                else _activeClient = &_clientHttp;

                _state = STATE_CONNECTING;
                _stateStartTime = millis();
                _handleConnecting();
                return;
            } else if (_currentReq.redirectCount >= _maxRedirects) {
                _setError(HTTP_ERR_TOO_MANY_REDIRECTS);
                _finishRequest();
                return;
            }
        }

        if (_isChunked) {
            _chunkState = CHUNK_STATE_SIZE;
            _chunkRemaining = 0;
            _chunkLine = "";
            _state = STATE_READ_BODY;
            _stateStartTime = millis();
            _handleReadBodyChunked();
        } else if (_contentLength == 0) {
            _onBodyFinished();
        } else if (_contentLength > 0) {
            if ((size_t)_contentLength <= _maxBodySize) {
                _response.body.reserve(_contentLength + 1);
            }
            _state = STATE_READ_BODY;
            _stateStartTime = millis();
            _handleReadBodyContentLength();
        } else {
            _state = STATE_READ_BODY;
            _stateStartTime = millis();
            _handleReadBodyEof();
        }
    }

    void _handleReadBodyChunked() {
        while (_activeClient->available() > 0) {
            _stateStartTime = millis();

            switch (_chunkState) {
                case CHUNK_STATE_SIZE: {
                    char c = (char)_activeClient->read();
                    if (c == '\n') {
                        String line = _chunkLine;
                        line.trim();
                        _chunkLine = "";

                        if (line.length() == 0) {
                            break;
                        }

                        int sc = line.indexOf(';');
                        if (sc >= 0) line = line.substring(0, sc);

                        long sz = strtol(line.c_str(), nullptr, 16);
                        if (sz == 0) {
                            _chunkState = CHUNK_STATE_TRAILERS;
                        } else if (sz > 0) {
                            _chunkRemaining = (size_t)sz;
                            _chunkState = CHUNK_STATE_DATA;
                        } else {
                            LOG_ASYNC_HTTP(printf_P, PSTR("[AsyncHTTP] Bad chunk size hex: '%s'\n"), line.c_str());
                            _setError(HTTP_ERR_PARSE);
                            _finishRequest();
                            return;
                        }
                    } else if (c != '\r') {
                        _chunkLine += c;
                    }
                    break;
                }

                case CHUNK_STATE_DATA: {
                    size_t avail = _activeClient->available();
                    size_t toRead = (avail < _chunkRemaining) ? avail : _chunkRemaining;
                    if (toRead > 128) toRead = 128;

                    char buf[128];
                    int n = _activeClient->read((uint8_t*)buf, toRead);
                    if (n > 0) {
                        _chunkRemaining -= n;
                        if (_response.body.length() + n <= _maxBodySize) {
                            _response.body.concat(buf, n);
                        } else {
                            _setError(HTTP_ERR_TOO_LARGE);
                            _finishRequest();
                            return;
                        }
                    }
                    if (_chunkRemaining == 0) {
                        _chunkState = CHUNK_STATE_CRLF;
                    }
                    break;
                }

                case CHUNK_STATE_CRLF: {
                    char c = (char)_activeClient->read();
                    if (c == '\n') {
                        _chunkState = CHUNK_STATE_SIZE;
                    }
                    break;
                }

                case CHUNK_STATE_TRAILERS: {
                    char c = (char)_activeClient->read();
                    if (c == '\n') {
                        String line = _chunkLine;
                        line.trim();
                        _chunkLine = "";
                        if (line.length() == 0) {
                            _onBodyFinished();
                            return;
                        }
                    } else if (c != '\r') {
                        _chunkLine += c;
                    }
                    break;
                }
            }
        }

        if (!_activeClient->connected() && !_activeClient->available()) {
            if (_chunkState == CHUNK_STATE_TRAILERS) {
                _onBodyFinished();
            } else {
                _setError(HTTP_ERR_READ_TIMEOUT);
                _finishRequest();
            }
            return;
        }

        if (millis() - _stateStartTime > _timeoutMs) {
            LOG_ASYNC_HTTP(println, F("[AsyncHTTP] Chunked body read timeout!"));
            _setError(HTTP_ERR_READ_TIMEOUT);
            _finishRequest();
        }
    }

    void _handleReadBodyContentLength() {
        while (_activeClient->available() > 0 && (int)_response.body.length() < _contentLength) {
            _stateStartTime = millis();
            size_t avail = _activeClient->available();
            size_t needed = _contentLength - _response.body.length();
            size_t toRead = (avail < needed) ? avail : needed;
            if (toRead > 128) toRead = 128;

            char buf[128];
            int n = _activeClient->read((uint8_t*)buf, toRead);
            if (n > 0) {
                if (_response.body.length() + n <= _maxBodySize) {
                    _response.body.concat(buf, n);
                } else {
                    _setError(HTTP_ERR_TOO_LARGE);
                    _finishRequest();
                    return;
                }
            }
        }

        if ((int)_response.body.length() >= _contentLength) {
            _onBodyFinished();
            return;
        }

        if (!_activeClient->connected() && !_activeClient->available()) {
            _setError(HTTP_ERR_READ_TIMEOUT);
            _finishRequest();
            return;
        }

        if (millis() - _stateStartTime > _timeoutMs) {
            LOG_ASYNC_HTTP(println, F("[AsyncHTTP] Body read timeout!"));
            _setError(HTTP_ERR_READ_TIMEOUT);
            _finishRequest();
        }
    }

    void _handleReadBodyEof() {
        while (_activeClient->available() > 0) {
            _stateStartTime = millis();
            size_t toRead = _activeClient->available();
            if (toRead > 128) toRead = 128;

            char buf[128];
            int n = _activeClient->read((uint8_t*)buf, toRead);
            if (n > 0) {
                if (_response.body.length() + n <= _maxBodySize) {
                    _response.body.concat(buf, n);
                } else {
                    _setError(HTTP_ERR_TOO_LARGE);
                    _finishRequest();
                    return;
                }
            }
        }

        if (!_activeClient->connected() && !_activeClient->available()) {
            _onBodyFinished();
            return;
        }

        if (millis() - _stateStartTime > _timeoutMs) {
            LOG_ASYNC_HTTP(println, F("[AsyncHTTP] Body read timeout (EOF)!"));
            _setError(HTTP_ERR_READ_TIMEOUT);
            _finishRequest();
        }
    }

    void _onBodyFinished() {
        LOG_ASYNC_HTTP(printf_P, PSTR("[AsyncHTTP] Finished: HTTP %d, body length: %u bytes\n"),
                       _response.code, (unsigned)_response.body.length());

        if (_connectionClose || _keepAliveTimeoutSec == 0) {
            if (_activeClient && _activeClient->connected()) {
                _activeClient->stop();
            }
            _activeClient = nullptr;
            _connectedHost = "";
            _connectedPort = 0;
        } else {
            _idleStartTime = millis();
        }

        _finishRequest();
    }

    void _finishRequest() {
        if (_lastError != HTTP_OK && _response.code >= 0) {
            _response.code = _lastError;
        }

        if (_lastError != HTTP_OK) {
            if (_activeClient && _activeClient->connected()) {
                _activeClient->stop();
            }
            _activeClient = nullptr;
            _connectedHost = "";
            _connectedPort = 0;
        }

        if (_currentReq.callback) {
            _currentReq.callback(_response);
        }

        _state = STATE_IDLE;
        if (_connectionClose || _keepAliveTimeoutSec == 0) {
            _activeClient = nullptr;
        }
        _lastError = HTTP_OK;

        _processQueue();
    }
};

#endif // ASYNC_HTTP_H
