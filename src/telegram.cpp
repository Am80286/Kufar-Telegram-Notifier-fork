//
//  telegram.cpp
//  Kufar Telegram Notifier
//
//  Created by Macintosh on 04.06.2022.
//

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <exception>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>
#include <tgbot/tgbot.h>

#include "telegram.hpp"
#include "telegram_commands.hpp"
#include "queries.hpp"
#include "helperfunctions.hpp"
#include "tgbot/Types.h"

namespace Telegram {
    using namespace std;
    using namespace queries;

    namespace {
        constexpr size_t MAX_IMAGES_IN_GROUP = 10;
        constexpr size_t TITLE_LIMIT = 200;
        constexpr size_t SELLER_NAME_LIMIT = 100;

        string groupDigits(long long value) {
            string digits = to_string(value);
            for (ptrdiff_t position = static_cast<ptrdiff_t>(digits.size()) - 3;
                 position > 0;
                 position -= 3) {
                digits.insert(static_cast<size_t>(position), " ");
            }
            return digits;
        }

        string formatPrice(int priceInKopecks) {
            if (priceInKopecks <= 0) {
                return "Цена не указана";
            }
            return groupDigits(priceInKopecks / 100) + " BYN";
        }

        string formatMedian(double medianInKopecks) {
            if (medianInKopecks <= 0.0) {
                return "Цена не указана";
            }
            return groupDigits(llround(medianInKopecks / 100.0)) + " BYN";
        }

        string formatDate(time_t date) {
            if (date == 0) {
                return "";
            }

            const tm value = localTime(date);
            const tm today = localTime(time(nullptr));

            char clock[8] = {};
            strftime(clock, sizeof(clock), "%H:%M", &value);

            if (value.tm_year == today.tm_year && value.tm_yday == today.tm_yday) {
                return string("сегодня, ") + clock;
            }
            if (value.tm_year == today.tm_year && value.tm_yday + 1 == today.tm_yday) {
                return string("вчера, ") + clock;
            }

            char buffer[32] = {};
            strftime(buffer, sizeof(buffer), "%d.%m.%Y, %H:%M", &value);
            return buffer;
        }

        string formatTag(const string &tag) {
            if (tag.empty()) {
                return tag;
            }
            if (tag.find_first_of(" \t\n") == string::npos) {
                return "#" + tag;
            }
            return tag;
        }

        string formatCategory(const Kufar::Ad &ad) {
            if (!ad.subCategory.has_value()) {
                return "";
            }

            const string leaf = Kufar::EnumString::subCategory(ad.subCategory.value());
            if (leaf.empty() || leaf.front() == '[') {
                return "";
            }

            if (ad.category.has_value()) {
                const string parent =
                    Kufar::EnumString::category(static_cast<Kufar::Category>(ad.category.value()));
                if (!parent.empty() && parent.front() != '[') {
                    return parent + " › " + leaf;
                }
            }

            return leaf;
        }

        string truncateText(const string &text, size_t limit) {
            if (text.size() <= limit) {
                return text;
            }

            size_t cut = text.rfind(' ', limit);
            if (cut == string::npos || cut < limit / 2) {
                cut = limit;
            }

            // Never split a UTF-8 sequence.
            while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
                cut--;
            }

            return text.substr(0, cut) + "…";
        }

        string formatPriceAssessment(const Market::PriceAssessment &assessment) {
            ostringstream deviationStream;
            deviationStream << fixed << setprecision(1) << fabs(assessment.deviationPercent);

            string line;
            if (assessment.deviationPercent < 0.0) {
                line = "📉 На " + deviationStream.str() + "% ниже медианы";
            } else if (assessment.deviationPercent > 0.0) {
                line = "📈 На " + deviationStream.str() + "% выше медианы";
            } else {
                line = "➖ На уровне медианы";
            }

            if (assessment.belowTypicalRange) {
                line += " · ниже типичного диапазона";
            } else if (assessment.aboveTypicalRange) {
                line += " · выше типичного диапазона";
            }

            return "📊 Медиана рынка: " + formatMedian(assessment.baselineMedian) + "\n" +
                    line + "\n";
        }

        string formatAdvert(const Kufar::Ad &ad, const optional<Market::PriceAssessment> &priceAssessment) {
            string text;

            if (ad.tag.has_value() && !ad.tag->empty()) {
                text += "🏷 " + escapeHTML(formatTag(ad.tag.value())) + "\n\n";
            }

            text += "🛍 <b>" + escapeHTML(truncateText(ad.title, TITLE_LIMIT)) + "</b>\n";
            text += "────────────\n";
            text += "💰 <b>" + formatPrice(ad.price) + "</b>\n";

            if (const string category = formatCategory(ad); !category.empty()) {
                text += "📦 " + escapeHTML(category) + "\n";
            }

            if (const string date = formatDate(ad.date); !date.empty()) {
                text += "🕒 " + date + "\n";
            }

            if (priceAssessment.has_value()) {
                text += "────────────\n";
                text += formatPriceAssessment(priceAssessment.value());
            }

            text += "────────────\n";

            const string sellerName = ad.sellerName.empty()
                ? string("продавец")
                : escapeHTML(truncateText(ad.sellerName, SELLER_NAME_LIMIT));
            text += "👤 " + sellerName + "\n";
            text += string("📞 ") +
                    (ad.phoneNumberIsVisible ? "Контакты открыты" : "Контакты скрыты") + "\n";

            text += "\n🔗 <a href=\"" + escapeHTML(ad.link) + "\">Открыть на Kufar</a>";

            return text;
        }

        int32_t messageThreadID(const optional<int> &threadID) {
            return threadID.value_or(0);
        }
    }

    Client::Client(const TelegramConfiguration &configuration, QueryStore &queryStore)
        : configuration_(configuration),
          httpClient_(make_unique<TgBot::CurlHttpClient>()),
          bot_(make_unique<TgBot::Bot>(configuration_.botToken, *httpClient_)), 
          commandsHandler_(make_unique<CommandsHandler>(*bot_, configuration_, queryStore)) {
        commandsHandler_->registerHandlers(); 
    }

    Client::~Client() {
        stopPolling();
    }

    void Client::editAdvertAssessment(
        const TgBot::Message::Ptr &message,
        const Kufar::Ad &ad,
        const Market::PriceAssessment &assessment
    ) const {
        if (message == nullptr)
            return;

        const TgBot::Api &api = bot_->getApi();
        const string newAdvertText = formatAdvert(ad, assessment);

        if (ad.images.empty()) {
            TgBot::EditMessageTextArgs args;
            args.chatId = configuration_.chatID;
            args.messageId = message->messageId;
            args.text = newAdvertText;
            args.parseMode = "HTML";
            api.editMessageText(args);
        } else {
            TgBot::EditMessageCaptionArgs args;
            args.chatId = configuration_.chatID;
            args.messageId = message->messageId;
            args.caption = newAdvertText;
            args.parseMode = "HTML";
            api.editMessageCaption(args);
        }
    }

    TgBot::Message::Ptr Client::sendAdvert(
        const Kufar::Ad &ad,
        optional<int> threadID,
        const optional<Market::PriceAssessment> &priceAssessment,
        bool allowMedia
    ) const {
        const string text = formatAdvert(ad, priceAssessment);
        const int32_t threadIDValue = messageThreadID(threadID);
        const TgBot::Api &api = bot_->getApi();

        if (!allowMedia || ad.images.empty()) {
            // sendMediaGroup requires 2-10 images, and sendPhoto with
            // a nonexistent placeholder URL always failed.
            TgBot::SendMessageArgs args;
            args.chatId = configuration_.chatID;
            args.text = text;
            args.parseMode = "HTML";
            args.messageThreadId = threadIDValue;

            // The "Открыть на Kufar" link is already in the text; a large
            // preview card would only duplicate it.
            auto linkPreview = make_shared<TgBot::LinkPreviewOptions>();
            linkPreview->isDisabled = true;
            args.linkPreviewOptions = linkPreview;

            return api.sendMessage(args);
        }

        if (ad.images.size() == 1) {
            TgBot::SendPhotoArgs args;
            args.chatId = configuration_.chatID;
            args.photo = ad.images.front();
            args.caption = text;
            args.parseMode = "HTML";
            args.messageThreadId = threadIDValue;
            return api.sendPhoto(args);
        }

        TgBot::SendMediaGroupArgs args;
        args.chatId = configuration_.chatID;
        args.messageThreadId = threadIDValue;

        const size_t imageCount = min(ad.images.size(), MAX_IMAGES_IN_GROUP);
        args.media.reserve(imageCount);

        for (size_t i = 0; i < imageCount; i++) {
            auto photo = make_shared<TgBot::InputMediaPhoto>();
            photo->media = ad.images[i];
            if (i == 0) {
                photo->caption = text;
                photo->parseMode = "HTML";
            }
            args.media.push_back(photo);
        }

        const auto messages = api.sendMediaGroup(args);
        for (const auto &messagePtr : messages) {
            if (messagePtr) {
                return messagePtr;
            }
        }

        spdlog::warn("[Telegram]: Media group was sent but Telegram returned no messages.");
        return nullptr;
    }

    void Client::validate() const {
        const TgBot::Api &api = bot_->getApi();

        const TgBot::User::Ptr me = api.getMe();
        spdlog::info("[Telegram] Бот: @{}", me->username.value_or("?"));

        const TgBot::ChatFullInfo::Ptr chat = api.getChat(configuration_.chatID);
        if (chat == nullptr) {
            throw runtime_error("Telegram не вернул данные чата.");
        }

        spdlog::info(
            "[Telegram] Чат доступен: {}",
            chat->title.value_or(std::to_string(configuration_.chatID))
        );
    }

    void Client::startPolling() {
        if (pollingThread_.joinable()) {
            return;
        }

        // Long polling does not work while a webhook is set for the bot.
        try {
            bot_->getApi().deleteWebhook();
        } catch (const exception &exc) {
            spdlog::warn("[Telegram] Не удалось удалить webhook: {}", exc.what());
        }

        try {
            const TgBot::User::Ptr me = bot_->getApi().getMe();
            spdlog::info("[Telegram] Long polling запущен (бот @{}).", me->username.value_or("?"));
        } catch (const exception &exc) {
            spdlog::warn("[Telegram] Не удалось получить данные бота: {}", exc.what());
        }

        longPoll_ = make_unique<TgBot::TgLongPoll>(*bot_, 100, 10);
        pollingThread_ = jthread([this] { pollingLoop(); });
    }

    void Client::stopPolling() {
        if (longPoll_) {
            longPoll_->stop();
        }
        if (pollingThread_.joinable()) {
            pollingThread_.join();
        }
    }

    void Client::pollingLoop() {
        try {
            // Empty signal list: the process lifecycle is managed by
            // the main thread, not by TgLongPoll.
            longPoll_->startLoop(
                [this](const exception &exc) {
                    spdlog::error("[Telegram polling] {}", exc.what());

                    // Without a pause, an unavailable Telegram API (e.g.
                    // with an invalid token) causes a continuous stream of
                    // errors in the log.
                    if (longPoll_->isRunning()) {
                        this_thread::sleep_for(chrono::seconds(3));
                    }
                },
                {}
            );
        } catch (const exception &exc) {
            spdlog::error("[Telegram polling] Остановлен: {}", exc.what());
        }
    }
};
