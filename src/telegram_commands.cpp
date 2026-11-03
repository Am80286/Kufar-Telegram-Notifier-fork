//
//  telegram_commands.cpp
//  Kufar Telegram Notifier
//
//  Created by Am80286 on 03.11.2026
//

#include "telegram_commands.hpp"
#include "telegram.hpp"

namespace Telegram {
    using namespace std;
    using namespace TgBot;
    using namespace Telegram;
    using namespace queries;

    CommandsHandler::CommandsHandler(Bot &bot, const TelegramConfiguration &config, QueryStore &queryStore)
        : bot_(bot),
          config_(config),
          queryStore_(queryStore) { 
    }

    void CommandsHandler::registerHandlers() {
        bot_.getEvents().onCommand("start", [this](Message::Ptr message) {
            handleStart(message);
        });
    }

    void CommandsHandler::reply(const Message::Ptr message, const string &reply) {
        SendMessageArgs replyArgs;

        replyArgs.chatId = message -> chat -> id;
        if (message -> messageThreadId && message -> messageThreadId.value())
            replyArgs.messageThreadId = message -> messageThreadId.value();

        replyArgs.text = reply;

        bot_.getApi().sendMessage(replyArgs); 
    }

    void CommandsHandler::handleStart(Message::Ptr message) {
        if (message -> chat -> id != config_.chatID) {
            reply(message, "Это приватный бот");
        }
    }
}
