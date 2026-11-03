//
//  telegram_commands.hpp
//  Kufar Telegram Notifier
//
//  Created by Am80286 on 03.11.2026
//

#ifndef telegram_commands_hpp
#define telegram_commands_hpp 

#include <tgbot/tgbot.h>

namespace queries {
    class QueryStore;
}

namespace Telegram {
    class TelegramConfiguration;

    class CommandsHandler {
    public:
        CommandsHandler(TgBot::Bot &bot, const Telegram::TelegramConfiguration &config, queries::QueryStore &queryStore);

    void registerHandlers();

    private:
        void reply(const TgBot::Message::Ptr message, const std::string &reply);

        void handleStart(TgBot::Message::Ptr message);

        TgBot::Bot &bot_;
        queries::QueryStore &queryStore_;
        const Telegram::TelegramConfiguration &config_;
    };
}

#endif
