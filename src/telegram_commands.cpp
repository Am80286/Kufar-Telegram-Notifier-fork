//
//  telegram_commands.cpp
//  Kufar Telegram Notifier
//
//  Created by Am80286 on 03.11.2026
//

#include <charconv>
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

        bot_.getEvents().onCommand("list", [this](Message::Ptr message) {
            handleList(message);
        });

        bot_.getEvents().onCommand("del", [this](Message::Ptr message) {
            handleDel(message);
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
    
    void CommandsHandler::handleList(TgBot::Message::Ptr message) {     
        if (!queryStore_.queryCount())
            return;

        string replyString;

        int idx = 0;
        for (const auto query : queryStore_.getQueries()) {
            if (query.messageThreadID == message->messageThreadId) {
                idx++;
                replyString += fmt::format("Поиск {}:\n{}\n", idx, query.getDescriptionConcise());
            }
        }

        if (idx)
            reply(message, fmt::runtime(replyString));
    }

    void CommandsHandler::handleDel(TgBot::Message::Ptr message) {
        if (message->chat->id != config_.chatID)
            return;

        const vector<string> args = parseArgs(message);
        if (args.size() != 2) {
            reply(message, "Использование: /del N, где N — номер поиска из /list");
            return;
        }

        size_t requestedIndex = 0;
        const string &indexText = args[1];
        const char *begin = indexText.data();
        const char *end = begin + indexText.size();
        const auto [position, error] = from_chars(begin, end, requestedIndex);
        if (indexText.empty() || error != errc() || position != end || requestedIndex == 0) {
            reply(message, "Номер поиска должен быть положительным целым числом");
            return;
        }

        const vector<QueryConfiguration> storedQueries = queryStore_.getQueries();
        if (storedQueries.empty()) {
            reply(message, "У вас нет добавленных поисков");
            return;
        }

        optional<QueryID> targetID;
        size_t index = 0;
        for (const auto &query : storedQueries) {
            if (query.messageThreadID == message->messageThreadId) {
                index++;
                if (index == requestedIndex) {
                    targetID = query.id;
                    break;
                }
            }
        }

        if (!targetID.has_value()) {
            reply(message, "Поиск №{} не найден в этой теме. Используйте /list", requestedIndex);
            return;
        }

        optional<QueryConfiguration> removedQuery;
        try {
            removedQuery = queryStore_.remove(targetID.value());
        } catch (const exception &exc) {
            spdlog::error("[Telegram]: /del command error: {}", exc.what());
            reply(message, "Произошла ошибка при удалении поиска:\n{}", exc.what());
            return;
        }

        if (!removedQuery.has_value()) {
            reply(message, "Этот поиск уже был удалён. Используйте /list");
            return;
        }

        reply(message, "Поиск №{} удалён", requestedIndex);
        spdlog::info(
            "\n- Удалён запрос (ID:{}):\n{}- Пользователь: {} (ID:{})\n",
            removedQuery->id,
            removedQuery->getDescription(),
            message->from->username.value_or("no username"),
            message->from->id
        );
    }
}
