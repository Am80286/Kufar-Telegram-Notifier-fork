//
//  telegram.hpp
//  Kufar Telegram Notifier
//
//  Created by Macintosh on 04.06.2022.
//

#ifndef telegram_hpp
#define telegram_hpp

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "kufar.hpp"

namespace TgBot {
    class Bot;
    class CurlHttpClient;
    class TgLongPoll;
}

namespace Telegram {
    struct TelegramConfiguration {
        std::string botToken;
        // Supergroup/chat ID. Negative for groups and channels.
        int64_t chatID = 0;
    };

    class Client {
    public:
        explicit Client(const TelegramConfiguration &);
        ~Client();

        Client(const Client &) = delete;
        Client &operator=(const Client &) = delete;

        void sendAdvert(const Kufar::Ad &, std::optional<int> messageThreadID) const;

        void startPolling();

        void stopPolling();

    private:
        void registerHandlers();
        void pollingLoop();

        TelegramConfiguration configuration_;
        // httpClient_ должен быть объявлен раньше bot_: Bot хранит ссылку на него.
        std::unique_ptr<TgBot::CurlHttpClient> httpClient_;
        std::unique_ptr<TgBot::Bot> bot_;
        std::unique_ptr<TgBot::TgLongPoll> longPoll_;
        std::jthread pollingThread_;
    };
};
#endif /* telegram_hpp */
