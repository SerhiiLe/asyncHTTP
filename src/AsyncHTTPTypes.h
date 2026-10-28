#ifndef ASYNC_HTTP_TYPES_H
#define ASYNC_HTTP_TYPES_H

#include <Arduino.h>
#include <functional>
#include <vector>

#if __has_include(<ArduinoJson.h>)
    #include <ArduinoJson.h>
#endif

// HTTP-методы запросов
enum AsyncHttpMethod {
    ASYNC_HTTP_GET,
    ASYNC_HTTP_POST,
    ASYNC_HTTP_PUT,
    ASYNC_HTTP_DELETE,
    ASYNC_HTTP_HEAD,
    ASYNC_HTTP_PATCH
};

// Коды ошибок для getLastError() и возврата в response.code
enum AsyncHttpError {
    ASYNC_HTTP_OK = 0,                    // Запрос успешно выполнен
    ASYNC_HTTP_ERR_NO_WIFI = -1,          // Нет подключения к Wi-Fi
    ASYNC_HTTP_ERR_INVALID_URL = -2,      // Некорректный или неподдерживаемый URL
    ASYNC_HTTP_ERR_CONNECT = -3,          // Ошибка установки TCP/TLS соединения
    ASYNC_HTTP_ERR_SEND = -4,             // Ошибка отправки HTTP-пакета
    ASYNC_HTTP_ERR_TIMEOUT = -5,          // Таймаут ожидания ответа сервера
    ASYNC_HTTP_ERR_READ_TIMEOUT = -6,     // Таймаут при чтении данных тела или заголовков
    ASYNC_HTTP_ERR_TOO_MANY_REDIRECTS = -7,// Превышен лимит перенаправлений (301/302)
    ASYNC_HTTP_ERR_TOO_LARGE = -8,        // Размер тела превышает maxBodySize
    ASYNC_HTTP_ERR_QUEUE_FULL = -9,       // Очередь запросов переполнена
    ASYNC_HTTP_ERR_CANCELLED = -10,       // Запрос отменен пользователем
    ASYNC_HTTP_ERR_PARSE = -11            // Ошибка парсинга данных (например, chunked)
};

// Заголовок HTTP (ключ-значение)
struct AsyncHttpHeader {
    String name;
    String value;
};

// Парсер URL
struct ParsedUrl {
    bool isHttps = false;
    String host;
    uint16_t port = 80;
    String path;
    bool valid = false;

    static ParsedUrl parse(const String& url) {
        ParsedUrl res;
        String u = url;
        u.trim();
        if (u.length() == 0) return res;

        int schemeEnd = u.indexOf(F("://"));
        if (schemeEnd > 0) {
            String scheme = u.substring(0, schemeEnd);
            scheme.toLowerCase();
            if (scheme == F("https")) {
                res.isHttps = true;
                res.port = 443;
            } else if (scheme == F("http")) {
                res.isHttps = false;
                res.port = 80;
            } else {
                return res; // Неподдерживаемая схема
            }
            u = u.substring(schemeEnd + 3);
        } else {
            // По умолчанию HTTP (порт 80)
            res.isHttps = false;
            res.port = 80;
        }

        // Выделение хоста, порта и пути: host[:port][/path]
        int slashIdx = u.indexOf('/');
        String hostPort;
        if (slashIdx >= 0) {
            hostPort = u.substring(0, slashIdx);
            res.path = u.substring(slashIdx);
        } else {
            hostPort = u;
            res.path = F("/");
        }

        int colonIdx = hostPort.indexOf(':');
        if (colonIdx >= 0) {
            res.host = hostPort.substring(0, colonIdx);
            res.port = (uint16_t)hostPort.substring(colonIdx + 1).toInt();
        } else {
            res.host = hostPort;
        }

        res.valid = (res.host.length() > 0 && res.port > 0);
        return res;
    }
};

// Функции декодирования спецсимволов JSON и XML
namespace AsyncHTTPCore {

    inline String jsonUnescape(const String& in) {
        String out;
        out.reserve(in.length());
        for (size_t i = 0; i < in.length(); i++) {
            char c = in[i];
            if (c == '\\' && i + 1 < in.length()) {
                i++;
                char next = in[i];
                switch (next) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        if (i + 4 < in.length()) {
                            char hex[5] = { in[i+1], in[i+2], in[i+3], in[i+4], 0 };
                            uint32_t codepoint = strtoul(hex, nullptr, 16);
                            i += 4;
                            if (codepoint <= 0x7F) {
                                out += (char)codepoint;
                            } else if (codepoint <= 0x7FF) {
                                out += (char)(0xC0 | ((codepoint >> 6) & 0x1F));
                                out += (char)(0x80 | (codepoint & 0x3F));
                            } else {
                                out += (char)(0xE0 | ((codepoint >> 12) & 0x0F));
                                out += (char)(0x80 | ((codepoint >> 6) & 0x3F));
                                out += (char)(0x80 | (codepoint & 0x3F));
                            }
                        }
                        break;
                    }
                    default:
                        out += '\\';
                        out += next;
                        break;
                }
            } else {
                out += c;
            }
        }
        return out;
    }

    inline String xmlUnescape(const String& in) {
        String out = in;
        out.replace(F("&quot;"), F("\""));
        out.replace(F("&apos;"), F("'"));
        out.replace(F("&amp;"), F("&"));
        out.replace(F("&lt;"), F("<"));
        out.replace(F("&gt;"), F(">"));
        return out;
    }

    // Быстрое извлечение поля из JSON или XML без аллокации дерева документов
    inline String extractField(const String& str, const char* search, bool isJson = true) {
        if (!search || strlen(search) == 0 || str.length() == 0) return "";

        if (isJson) {
            String needle = F("\"");
            needle += search;
            needle += F("\"");
            int pos = str.indexOf(needle);
            if (pos < 0) return "";

            int colon = str.indexOf(':', pos + needle.length());
            if (colon < 0) return "";

            size_t idx = colon + 1;
            while (idx < str.length() && (str[idx] == ' ' || str[idx] == '\t' || str[idx] == '\r' || str[idx] == '\n')) {
                idx++;
            }
            if (idx >= str.length()) return "";

            if (str[idx] == '"') {
                size_t start = idx + 1;
                size_t end = start;
                while (end < str.length()) {
                    if (str[end] == '"' && str[end - 1] != '\\') break;
                    end++;
                }
                return jsonUnescape(str.substring(start, end));
            } else {
                size_t start = idx;
                size_t end = start;
                while (end < str.length() && str[end] != ',' && str[end] != '}' && str[end] != ']' && str[end] != '\r' && str[end] != '\n') {
                    end++;
                }
                String val = str.substring(start, end);
                val.trim();
                return val;
            }
        } else {
            String openTag = F("<");
            openTag += search;
            openTag += F(">");
            int pos = str.indexOf(openTag);
            if (pos < 0) {
                String openTagAttr = F("<");
                openTagAttr += search;
                openTagAttr += F(" ");
                pos = str.indexOf(openTagAttr);
                if (pos >= 0) {
                    int closingAngle = str.indexOf('>', pos);
                    if (closingAngle > 0) {
                        pos = closingAngle - openTag.length() + 1;
                    } else {
                        pos = -1;
                    }
                }
            }
            if (pos >= 0) {
                size_t start = pos + openTag.length();
                String closeTag = F("</");
                closeTag += search;
                closeTag += F(">");
                int end = str.indexOf(closeTag, start);
                if (end < 0) {
                    end = str.indexOf(F("</"), start);
                }
                if (end > (int)start) {
                    return xmlUnescape(str.substring(start, end));
                }
            }
        }
        return "";
    }
}

// Структура ответа HTTP
struct AsyncHttpResponse {
    int code = 0;                         // HTTP статус код (200, 404 и т.д.) или код ошибки AsyncHttpError (< 0)
    String body;                          // Тело ответа
    String contentType;                   // Тип контента
    int contentLength = -1;               // Content-Length (-1 если chunked или не указан)
    std::vector<AsyncHttpHeader> headers; // Полученные заголовки

    bool isSuccess() const { return code >= 200 && code < 300; }
    bool isError() const { return code < 0; }

    String errorString() const {
        switch (code) {
            case ASYNC_HTTP_OK: return F("OK");
            case ASYNC_HTTP_ERR_NO_WIFI: return F("No Wi-Fi connection");
            case ASYNC_HTTP_ERR_INVALID_URL: return F("Invalid URL");
            case ASYNC_HTTP_ERR_CONNECT: return F("Connection failed");
            case ASYNC_HTTP_ERR_SEND: return F("Send failed");
            case ASYNC_HTTP_ERR_TIMEOUT: return F("Response timeout");
            case ASYNC_HTTP_ERR_READ_TIMEOUT: return F("Read timeout");
            case ASYNC_HTTP_ERR_TOO_MANY_REDIRECTS: return F("Too many redirects");
            case ASYNC_HTTP_ERR_TOO_LARGE: return F("Response body too large");
            case ASYNC_HTTP_ERR_QUEUE_FULL: return F("Queue full");
            case ASYNC_HTTP_ERR_CANCELLED: return F("Request cancelled");
            case ASYNC_HTTP_ERR_PARSE: return F("Parse error");
            default:
                if (code > 0) {
                    return String("HTTP ") + code;
                }
                return F("Unknown error");
        }
    }

    String getHeader(const String& name) const {
        for (const auto& h : headers) {
            if (h.name.equalsIgnoreCase(name)) return h.value;
        }
        return "";
    }

    // Быстрое извлечение поля из JSON или XML без библиотек парсинга
    String extract(const char* key, bool isJson = true) const {
        return AsyncHTTPCore::extractField(body, key, isJson);
    }

#if defined(ARDUINOJSON_VERSION) || __has_include(<ArduinoJson.h>)
    template <typename TDoc>
    DeserializationError parseJson(TDoc& doc) const {
        return deserializeJson(doc, body);
    }
#endif
};

// Тип функции обратного вызова
using AsyncHttpCallback = std::function<void(AsyncHttpResponse&)>;

struct AsyncHttpRequest {
    AsyncHttpMethod method = ASYNC_HTTP_GET;
    String url;
    String payload;
    String contentType = "application/x-www-form-urlencoded";
    std::vector<AsyncHttpHeader> headers;
    AsyncHttpCallback callback = nullptr;
    uint32_t customTimeout = 0; // 0 = использовать тайм-аут по умолчанию
    uint8_t redirectCount = 0;

    // Удобные методы добавления пользовательских заголовков (поддерживают чейнинг)
    AsyncHttpRequest& addHeader(const String& name, const String& value) {
        headers.push_back({name, value});
        return *this;
    }

    AsyncHttpRequest& addHeader(const char* name, const char* value) {
        headers.push_back({String(name), String(value)});
        return *this;
    }

    // Очистить список заголовков
    void clearHeaders() {
        headers.clear();
    }
};

#endif // ASYNC_HTTP_TYPES_H
