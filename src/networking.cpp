//
//  networking.cpp
//  Kufar Telegram Notifier
//
//  Created by Macintosh on 04.06.2022.
//

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>

#include <curl/curl.h>
#include "networking.hpp"
#include "request_gate.hpp"
#include "helperfunctions.hpp"

namespace Networking {
    using std::string;

    namespace {
        std::atomic<RequestGate *> gRequestGate{nullptr};

        struct ResponseData {
            string body;
            size_t maxSize = 0;
            bool sizeExceeded = false;
        };

        struct ResponseHeaders {
            std::optional<std::chrono::seconds> retryAfter;
        };

        static size_t writeFunction(void *ptr, size_t size, size_t nmemb, ResponseData *data) {
            const size_t bytes = size * nmemb;
            if (data->maxSize > 0 &&
                (data->body.size() >= data->maxSize || bytes > data->maxSize - data->body.size())) {
                data->sizeExceeded = true;
                return 0;
            }

            data->body.append(static_cast<char *>(ptr), bytes);
            return bytes;
        }

        static size_t headerFunction(char *buffer, size_t size, size_t nitems, ResponseHeaders *headers) {
            const size_t bytes = size * nitems;
            if (headers == nullptr) {
                return bytes;
            }

            std::string_view line(buffer, bytes);
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
                line.remove_suffix(1);
            }

            const size_t separator = line.find(':');
            if (separator == std::string_view::npos) {
                return bytes;
            }

            string name(line.substr(0, separator));
            std::transform(
                name.begin(),
                name.end(),
                name.begin(),
                [](unsigned char character) { return static_cast<char>(std::tolower(character)); }
            );

            if (name != "retry-after") {
                return bytes;
            }

            std::string_view value = line.substr(separator + 1);
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
                value.remove_prefix(1);
            }
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
                value.remove_suffix(1);
            }

            try {
                const int seconds = std::stoi(string(value));
                if (seconds >= 0) {
                    headers->retryAfter = std::chrono::seconds(seconds);
                }
            } catch (const std::exception &) {
                // Retry-After may be an HTTP date; ignore unsupported forms.
            }

            return bytes;
        }

        void initializeCurl() {
            static const CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
            if (result != CURLE_OK) {
                throw std::runtime_error("Unable to initialize libcurl.");
            }
        }

        // One easy handle per thread. Reusing it keeps the connection cache,
        // DNS cache and TLS session cache alive between polling requests
        struct ThreadCurlHandle {
            CURL *handle = nullptr;

            ThreadCurlHandle() : handle(curl_easy_init()) {
                if (handle == nullptr) {
                    throw std::runtime_error("Unable to initialize HTTP request.");
                }
            }

            ~ThreadCurlHandle() {
                if (handle != nullptr) {
                    curl_easy_cleanup(handle);
                }
            }

            ThreadCurlHandle(const ThreadCurlHandle &) = delete;
            ThreadCurlHandle &operator=(const ThreadCurlHandle &) = delete;
        };

        CURL *acquireCurlHandle() {
            thread_local ThreadCurlHandle holder;
            return holder.handle;
        }

        /**
          Hides the bot token in a URL before logging it,
          so it never ends up in the log file in plain text.
         */
        static string redactBotToken(const string &url) {
            const string marker = "/bot";
            const size_t markerPosition = url.find(marker);
            if (markerPosition == string::npos) {
                return url;
            }

            const size_t tokenStart = markerPosition + marker.size();
            const size_t tokenEnd = url.find('/', tokenStart);
            if (tokenEnd == string::npos) {
                return url;
            }

            return url.substr(0, tokenStart) + "***" + url.substr(tokenEnd);
        }
    }

    void setRequestGate(RequestGate *gate) {
        gRequestGate.store(gate);
    }

    RequestGate *requestGate() {
        return gRequestGate.load();
    }

    HttpError::HttpError(
        int statusCode,
        std::optional<std::chrono::seconds> retryAfter,
        const string &message
    ) : std::runtime_error(message),
        statusCode_(statusCode),
        retryAfter_(retryAfter) {
    }

    int HttpError::statusCode() const {
        return statusCode_;
    }

    const std::optional<std::chrono::seconds> &HttpError::retryAfter() const {
        return retryAfter_;
    }

    string urlEncode(const string &text) {
        CURL *curl = curl_easy_init();
        char *encoded = curl_easy_escape(curl, text.c_str(), 0);
        string tempVariable = encoded;
        curl_free(encoded);
        curl_easy_cleanup(curl);
        return tempVariable;
    }

    string getTextFromURL(const string &url, size_t maxResponseSize) {
        DEBUG_MSG("[URL: {}]", redactBotToken(url));
        initializeCurl();

        RequestGate *gate = requestGate();
        if (gate != nullptr && !gate->acquire()) {
            throw RequestCancelled("HTTP request cancelled during shutdown.");
        }

        CURL *curl = acquireCurlHandle();
        curl_easy_reset(curl);

        ResponseData response;
        response.maxSize = maxResponseSize;
        ResponseHeaders headers;
        char errorBuffer[CURL_ERROR_SIZE] = {};

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 0L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Kufar-Telegram-Notifier");
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerFunction);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFunction);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        const CURLcode result = curl_easy_perform(curl);
        if (result != CURLE_OK) {
            if (response.sizeExceeded) {
                throw std::runtime_error("HTTP response exceeds the configured size limit.");
            }

            if (gate != nullptr) {
                gate->reportFailure(0, std::nullopt);
            }

            const string details = errorBuffer[0] != '\0' ? errorBuffer : curl_easy_strerror(result);
            throw HttpError(0, std::nullopt, "HTTP request failed: " + details);
        }

        long statusCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);
        if (statusCode < 200 || statusCode >= 300) {
            if (gate != nullptr) {
                gate->reportFailure(static_cast<int>(statusCode), headers.retryAfter);
            }

            throw HttpError(
                static_cast<int>(statusCode),
                headers.retryAfter,
                "HTTP request returned status " + std::to_string(statusCode) + "."
            );
        }

        if (gate != nullptr) {
            gate->reportSuccess();
        }

        return std::move(response.body);
    }

    string getJSONFromURL(const string &url) {
        return getTextFromURL(url);
    }

}
