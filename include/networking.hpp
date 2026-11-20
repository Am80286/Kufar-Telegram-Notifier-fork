//
//  networking.hpp
//  Kufar Telegram Notifier
//
//  Created by Macintosh on 04.06.2022.
//

#ifndef networking_hpp
#define networking_hpp

#include <chrono>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>

namespace Networking {
    class RequestGate;

    void setRequestGate(RequestGate *);
    RequestGate *requestGate();

    // The server returned a non-2xx HTTP status.
    class HttpError : public std::runtime_error {
    public:
        HttpError(
            int statusCode,
            std::optional<std::chrono::seconds> retryAfter,
            const std::string &message
        );

        int statusCode() const;
        const std::optional<std::chrono::seconds> &retryAfter() const;

    private:
        int statusCode_;
        std::optional<std::chrono::seconds> retryAfter_;
    };

    // The request was abandoned because shutdown was requested while waiting
    // for the shared request gate.
    class RequestCancelled : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    std::string urlEncode(const std::string &);

    // A zero maxResponseSize disables the response size limit.
    std::string getTextFromURL(const std::string &, std::size_t maxResponseSize = 0);
    std::string getJSONFromURL(const std::string &);
};

#endif /* networking_hpp */
