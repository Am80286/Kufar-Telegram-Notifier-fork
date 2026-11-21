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
#include "helperfunctions.hpp"


namespace Telegram {
    using namespace std;
    using namespace TgBot;
    using namespace nlohmann;
    using namespace Telegram;
    using namespace queries;
    using namespace Kufar;

    namespace {
        string groupDigits(long long value) {
            string digits = to_string(value);
            for (ptrdiff_t position = static_cast<ptrdiff_t>(digits.size()) - 3;
                 position > 0;
                 position -= 3) {
                digits.insert(static_cast<size_t>(position), " ");
            }
            return digits;
        }

        string formatPriceRange(const PriceRange &priceRange) {
            const optional<int> &min = priceRange.priceMin;
            const optional<int> &max = priceRange.priceMax;

            if (min.has_value() && max.has_value()) {
                return groupDigits(min.value()) + "–" + groupDigits(max.value()) + " BYN";
            }
            if (min.has_value()) {
                return "от " + groupDigits(min.value()) + " BYN";
            }
            if (max.has_value()) {
                return "до " + groupDigits(max.value()) + " BYN";
            }

            return "";
        }

        string formatTag(const string &tag) {
            if (tag.empty() || tag.find_first_of(" \t\n") != string::npos) {
                return tag;
            }
            return "#" + tag;
        }

        string formatLocation(const KufarConfiguration &configuration) {
            string location;

            if (configuration.region.has_value()) {
                location = escapeHTML(EnumString::region(configuration.region.value()));
            }

            if (configuration.areas.has_value() && !configuration.areas->empty()) {
                string areas;
                for (const int area : configuration.areas.value()) {
                    const string name = EnumString::area(area);
                    if (name.empty() || name.front() == '[') {
                        continue;
                    }
                    if (!areas.empty()) {
                        areas += ", ";
                    }
                    areas += escapeHTML(name);
                }

                if (!areas.empty()) {
                    location = location.empty() ? areas : location + ", " + areas;
                }
            }

            return location;
        }

        string formatQuerySummary(size_t index, const QueryConfiguration &query) {
            const string displayName = query.getDisplayName();
            string text = fmt::format("<b>{}. {}</b>\n", index, escapeHTML(displayName));

            const bool hasLabel = query.label.has_value() && !query.label->empty();
            if (hasLabel && query.kufar.tag.has_value() && !query.kufar.tag->empty()) {
                text += "🔎 " + escapeHTML(formatTag(query.kufar.tag.value())) + "\n";
            }

            const string category = query.getCategoryName();
            if (!category.empty() && category != displayName) {
                text += "📦 " + escapeHTML(category) + "\n";
            }

            if (const string price = formatPriceRange(query.kufar.priceRange);
                !price.empty() && price != displayName) {
                text += "💰 " + price + "\n";
            }

            if (const string location = formatLocation(query.kufar);
                !location.empty() && location != displayName) {
                text += "📍 " + location + "\n";
            }

            if (query.pollIntervalSeconds.has_value()) {
                text += "🕒 каждые " + to_string(query.pollIntervalSeconds.value()) + " с\n";
            }

            if (query.kufar.condition.has_value()) {
                text += "🔹 Состояние: " +
                        escapeHTML(EnumString::itemCondition(query.kufar.condition.value())) + "\n";
            }

            return text;
        }
    }

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
        replyArgs.parseMode = "HTML";

        bot_.getApi().sendMessage(replyArgs);
    }
    
    void CommandsHandler::handleAdd(Message::Ptr message) {
        if (message->chat->id != config_.chatID)
            return;
        
        vector<string> args = parseArgs(message);
        if (args.size() < 2)
        {
            reply(message, "❌ <b>Не указана ссылка на поиск</b>\n\n"
                           "Использование: <code>/add ссылка [метка]</code>");
            return;
        }

        string url = args[1];
        string label;
        for (size_t i = 2; i < args.size(); i++) {
            if (!label.empty()) {
                label += " ";
            }
            label += args[i];
        }

        boost::asio::post(thread_pool_, [this, message, url = std::move(url), label = std::move(label)] {
            try {
                json newQuery = importSearchQuery(url);
                if (message->messageThreadId.has_value())
                    newQuery["message-thread-id"] = message->messageThreadId.value();
                if (!label.empty())
                    newQuery["label"] = label;

                queryStore_.add(newQuery);

                const QueryConfiguration addedQuery = parseQuery(newQuery);
                reply(message, "✅ <b>Поиск добавлен</b>\n🏷 {}", escapeHTML(addedQuery.getDisplayName()));

                spdlog::info(
                    "\n- Добавлен запрос:\n{}- Пользователь: {} (ID:{})\n", 
                    addedQuery.getDescription(),
                    message->from->username.value_or("no username"),
                    message->from->id
                );
            } catch (const std::exception& exc) {
                spdlog::error("[Telegram]: /add command error: {}", exc.what());
                reply(message, "❌ <b>Не удалось добавить поиск</b>\n\n"
                               "<code>{}</code>\n\n"
                               "Проверьте ссылку и попробуйте снова.",
                               escapeHTML(exc.what()));
            }
        });
    }

    void CommandsHandler::handleStart(Message::Ptr message) {
        if (message -> chat -> id != config_.chatID) {
            reply(message, "🔒 <b>Это приватный бот</b>");
        } else {
            reply(message, "✅ <b>Бот включён и работает</b>");
        }
    }
    
    void CommandsHandler::handleList(TgBot::Message::Ptr message) {     
        string replyString;
        size_t idx = 0;

        for (const auto &query : queryStore_.getQueries()) {
            if (query.messageThreadID == message->messageThreadId) {
                idx++;
                if (idx > 1) {
                    replyString += "\n";
                }
                replyString += formatQuerySummary(idx, query);
            }
        }

        if (idx == 0) {
            reply(message, "🔎 <b>В этой теме пока нет поисков</b>\n\n"
                           "Добавьте поиск командой <code>/add ссылка</code>");
            return;
        }

        // replyImpl avoids treating user-provided text as a format string.
        replyImpl(message, "🔎 <b>Ваши поиски</b>\n\n" + replyString);
    }

    void CommandsHandler::handleDel(TgBot::Message::Ptr message) {
        if (message->chat->id != config_.chatID)
            return;

        const vector<string> args = parseArgs(message);
        if (args.size() != 2) {
            reply(message, "❌ <b>Неверный формат команды</b>\n\n"
                           "Использование: <code>/del N</code>, где N — номер поиска из /list");
            return;
        }

        size_t requestedIndex = 0;
        const string &indexText = args[1];
        const char *begin = indexText.data();
        const char *end = begin + indexText.size();
        const auto [position, error] = from_chars(begin, end, requestedIndex);
        if (indexText.empty() || error != errc() || position != end || requestedIndex == 0) {
            reply(message, "❌ <b>Номер поиска должен быть положительным целым числом</b>");
            return;
        }

        const vector<QueryConfiguration> storedQueries = queryStore_.getQueries();
        if (storedQueries.empty()) {
            reply(message, "🔎 <b>У вас нет добавленных поисков</b>");
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
            reply(message, "❌ Поиск №{} не найден в этой теме\n\n"
                           "Используйте /list, чтобы увидеть номера поисков.", requestedIndex);
            return;
        }

        optional<QueryConfiguration> removedQuery;
        try {
            removedQuery = queryStore_.remove(targetID.value());
        } catch (const exception &exc) {
            spdlog::error("[Telegram]: /del command error: {}", exc.what());
            reply(message, "❌ <b>Не удалось удалить поиск</b>\n\n<code>{}</code>",
                           escapeHTML(exc.what()));
            return;
        }

        if (!removedQuery.has_value()) {
            reply(message, "❌ <b>Этот поиск уже был удалён</b>\n\n"
                           "Используйте /list, чтобы увидеть актуальный список.");
            return;
        }

        reply(message, "🗑 <b>Поиск №{} удалён</b>", requestedIndex);
        spdlog::info(
            "\n- Удалён запрос (ID:{}):\n{}- Пользователь: {} (ID:{})\n",
            removedQuery->id,
            removedQuery->getDescription(),
            message->from->username.value_or("no username"),
            message->from->id
        );
    }
}
