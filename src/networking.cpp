//
//  networking.cpp
//  Kufar Telegram Notifier
//
//  Created by Macintosh on 04.06.2022.
//

#include <iostream>
#include <memory>
#include <stdexcept>

#include <curl/curl.h>
#include "networking.hpp"
#include "helperfunctions.hpp"

namespace Networking {
    using std::string;

    namespace {
        struct ResponseData {
            string body;
            size_t maxSize = 0;
            bool sizeExceeded = false;
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

        void initializeCurl() {
            static const CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
            if (result != CURLE_OK) {
                throw std::runtime_error("Unable to initialize libcurl.");
            }
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

        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
        if (!curl) {
            throw std::runtime_error("Unable to initialize HTTP request.");
        }

        ResponseData response;
        response.maxSize = maxResponseSize;
        char errorBuffer[CURL_ERROR_SIZE] = {};

        curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl.get(), CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, errorBuffer);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writeFunction);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);

        const CURLcode result = curl_easy_perform(curl.get());
        if (result != CURLE_OK) {
            if (response.sizeExceeded) {
                throw std::runtime_error("HTTP response exceeds the configured size limit.");
            }

            const string details = errorBuffer[0] != '\0' ? errorBuffer : curl_easy_strerror(result);
            throw std::runtime_error("HTTP request failed: " + details);
        }

        long statusCode = 0;
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &statusCode);
        if (statusCode < 200 || statusCode >= 300) {
            throw std::runtime_error(
                "HTTP request returned status " + std::to_string(statusCode) + "."
            );
        }

        return std::move(response.body);
    }

    string getJSONFromURL(const string &url) {
        return getTextFromURL(url);
    }

}
