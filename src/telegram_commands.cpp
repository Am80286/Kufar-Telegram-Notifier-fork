//
//  telegram_commands.cpp
//  Kufar Telegram Notifier
//
//  Created by Am80286 on 03.11.2026
//

#include <exception>

#include <spdlog/spdlog.h>
#include <spdlog/fmt/fmt.h>
#include <boost/asio.hpp>

#include "telegram_commands.hpp"
#include "telegram.hpp"
#include "kufar_url.hpp"
#include "queries.hpp"


namespace Telegram {
    using namespace std;
    using namespace TgBot;
    using namespace nlohmann;
    using namespace Telegram;
    using namespace queries;
    using namespace Kufar;

    CommandsHandler::CommandsHandler(Bot &bot, const TelegramConfiguration &config, QueryStore &queryStore)
        : bot_(bot),
          config_(config),
          queryStore_(queryStore) { 
    }

    void CommandsHandler::registerHandlers() {
        bot_.getEvents().onCommand("start", [this](Message::Ptr message) {
            handleStart(message);
        });

        bot_.getEvents().onCommand("add", [this](Message::Ptr message) {
            handleAdd(message);
        });
    }
    
    vector<string> CommandsHandler::parseArgs(Message::Ptr message) {
        vector<std::string> argv;
        string cur;
        bool inQuotes = false, hasToken = false;
        string text = message -> text.value_or("");

        for (size_t i = 0; i < text.size(); ++i) {
            char c = text[i];
            if (c == '\\' && i + 1 < text.size() && inQuotes) {
                cur += text[++i];
                hasToken = true;
            } else if (c == '"') {
                inQuotes = !inQuotes;
                hasToken = true;
            } else if (isspace(static_cast<unsigned char>(c)) && !inQuotes) {
                if (hasToken) {
                    argv.push_back(cur);
                    cur.clear();
                    hasToken = false;
                }
            } else {
                cur += c;
                hasToken = true;
            }
        }
        if (hasToken) argv.push_back(cur);

        if (!argv.empty()) {
            string& cmd = argv[0];
            if (!cmd.empty() && cmd[0] == '/') cmd.erase(0, 1);
            auto at = cmd.find('@');
            if (at != string::npos) cmd.erase(at);
        }

        return argv;
    }

    void CommandsHandler::replyImpl(const Message::Ptr &message, const string &text) {
        SendMessageArgs replyArgs;

        replyArgs.chatId = message->chat->id;
        if (message->messageThreadId && message->messageThreadId.value())
            replyArgs.messageThreadId = message->messageThreadId.value();

        replyArgs.text = text;

        bot_.getApi().sendMessage(replyArgs);
    }
    
    void CommandsHandler::handleAdd(Message::Ptr message) {
        if (message->chat->id != config_.chatID)
            return;
        
        vector<string> args = parseArgs(message);
        if (args.size() < 2)
        {
            reply(message, "Вы не указали ссылку на поиск");
            return;
        }

        string url = args[1];

        boost::asio::post(thread_pool_, [this, message, url = std::move(url)] {
            try {
                json newQuery = importSearchQuery(url);
                if (message->messageThreadId)
                    newQuery["message-thread-id"] = message->messageThreadId;

                queryStore_.add(newQuery);
                reply(message, "Поиск добавлен");
                spdlog::info(
                    "\n- Добавлен запрос:\n{}- Пользователь: {} (ID:{})\n", 
                    parseQuery(newQuery).getDescription(),
                    message->from->username.value_or("no username"),
                    message->from->id
                );
            } catch (const std::exception& exc) {
                spdlog::error("[Telegram]: /add command error: {}", exc.what());
                reply(message, "Произошла ошибка:\n{}\nВозможно вы ввели неправильную ссылку", exc.what());
            }
        });
    }

    void CommandsHandler::handleStart(Message::Ptr message) {
        if (message -> chat -> id != config_.chatID) {
            reply(message, "Это приватный бот");
        } else {
            reply(message, "Бот включен и работает");
        }
    }
}
