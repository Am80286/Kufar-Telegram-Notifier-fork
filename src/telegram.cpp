//
//  telegram.cpp
//  Kufar Telegram Notifier
//
//  Created by Macintosh on 04.06.2022.
//

#include <algorithm>
#include <array>
#include <chrono>
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
#include "tgbot/Types.h"

namespace Telegram {
    using namespace std;
    using namespace queries;

    namespace {
        constexpr size_t MAX_IMAGES_IN_GROUP = 10;

        string formatPriceAssessment(const Market::PriceAssessment &assessment) {
            ostringstream medianStream;
            medianStream << fixed << setprecision(2) << assessment.baselineMedian / 100.0;

            ostringstream deviationStream;
            deviationStream << fixed << setprecision(1) << assessment.deviationPercent;

            return "Медиана рынка: " + medianStream.str() +
                    "Отклонение от медианы: " + (assessment.deviationPercent > 0.0 ? "+" : "") +
                    deviationStream.str() + "%\n";
        }

        string formatAdvert(const Kufar::Ad &ad, const optional<Market::PriceAssessment> &priceAssessment) {
            array<char, 26> timeBuffer{};
            if (ctime_r(&ad.date, timeBuffer.data()) == nullptr) {
                throw runtime_error("Unable to format advert timestamp.");
            }

            string formattedTime = timeBuffer.data();
            if (!formattedTime.empty() && formattedTime.back() == '\n') {
                formattedTime.pop_back();
            }

            string text;

            if (ad.tag.has_value()) {
                text += "#" + ad.tag.value() + "\n";
            }

            text += "Название: " + ad.title + "\n"
                    "Дата: " + formattedTime + "\n"
                    "Цена: " + to_string(ad.price / 100) + " BYN\n";

            if (priceAssessment.has_value()) {
                text += formatPriceAssessment(priceAssessment.value());
            }

            text += "\nИмя продавца: " + ad.sellerName + "\n"
                    "Номер телефона не скрыт: " + (ad.phoneNumberIsVisible ? "Да" : "Нет") + "\n"
                    "Ссылка: " + ad.link;

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
            api.editMessageText(args);
        } else {
            TgBot::EditMessageCaptionArgs args;
            args.chatId = configuration_.chatID;
            args.messageId = message->messageId;
            args.caption = newAdvertText;
            api.editMessageCaption(args);
        }
    }

    TgBot::Message::Ptr Client::sendAdvert(
        const Kufar::Ad &ad,
        optional<int> threadID,
        const optional<Market::PriceAssessment> &priceAssessment
    ) const {
        const string text = formatAdvert(ad, priceAssessment);
        const int32_t threadIDValue = messageThreadID(threadID);
        const TgBot::Api &api = bot_->getApi();

        if (ad.images.empty()) {
            // sendMediaGroup requires 2-10 images, and sendPhoto with
            // a nonexistent placeholder URL always failed.
            TgBot::SendMessageArgs args;
            args.chatId = configuration_.chatID;
            args.text = text;
            args.messageThreadId = threadIDValue;
            return api.sendMessage(args);
        }

        if (ad.images.size() == 1) {
            TgBot::SendPhotoArgs args;
            args.chatId = configuration_.chatID;
            args.photo = ad.images.front();
            args.caption = text;
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
