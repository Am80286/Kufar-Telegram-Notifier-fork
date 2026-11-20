//
//  telegram_sender.cpp
//  Kufar Telegram Notifier
//

#include <optional>
#include <string>

#include <spdlog/spdlog.h>
#include <tgbot/TgException.h>

#include "telegram_sender.hpp"
#include "telegram.hpp"
#include "market.hpp"

namespace delivery {
    namespace {
        SendException translateTelegramError(const TgBot::TgException &exc) {
            switch (exc.errorCode) {
                case TgBot::TgException::ErrorCode::Flood: {
                    const int retryAfter = exc.parameters && exc.parameters->retryAfter.has_value()
                        ? exc.parameters->retryAfter.value()
                        : 1;
                    return SendException(
                        SendFailureKind::RateLimited,
                        "Telegram rate limit: " + std::string(exc.what()),
                        std::chrono::seconds(retryAfter)
                    );
                }
                case TgBot::TgException::ErrorCode::Unauthorized:
                case TgBot::TgException::ErrorCode::Forbidden:
                case TgBot::TgException::ErrorCode::NotFound:
                case TgBot::TgException::ErrorCode::BadRequest:
                    return SendException(
                        SendFailureKind::Permanent,
                        std::string(exc.what())
                    );
                default:
                    return SendException(
                        SendFailureKind::Transient,
                        std::string(exc.what())
                    );
            }
        }
    }

    SendFunction makeTelegramSender(
        Telegram::Client &client,
        Market::MarketAnalyzer &marketAnalyzer
    ) {
        return [&client, &marketAnalyzer](const DeliveryItem &item) {
            std::optional<Market::PriceAssessment> assessment;
            try {
                assessment = marketAnalyzer.rate(item.ad);
            } catch (const std::exception &exc) {
                spdlog::warn(
                    "[Delivery] Market rating failed for listing {}: {}",
                    item.ad.id,
                    exc.what()
                );
            }

            TgBot::Message::Ptr message;
            bool usedTextFallback = false;

            try {
                message = client.sendAdvert(
                    item.ad,
                    item.messageThreadID,
                    assessment,
                    true
                );
            } catch (const TgBot::TgException &exc) {
                const bool retryAsText =
                    exc.errorCode == TgBot::TgException::ErrorCode::BadRequest &&
                    !item.ad.images.empty();

                if (!retryAsText) {
                    throw translateTelegramError(exc);
                }

                spdlog::warn(
                    "[Delivery] Listing {} was rejected with media; retrying as text: {}",
                    item.ad.id,
                    exc.what()
                );

                try {
                    message = client.sendAdvert(
                        item.ad,
                        item.messageThreadID,
                        assessment,
                        false
                    );
                    usedTextFallback = true;
                } catch (const TgBot::TgException &fallbackError) {
                    throw translateTelegramError(fallbackError);
                }
            }

            if (!assessment.has_value() && message != nullptr) {
                const Kufar::Ad advert = item.ad;
                Telegram::Client *clientPtr = &client;
                marketAnalyzer.rateDeferred(
                    advert,
                    [clientPtr, message, advert](Market::PriceAssessment newAssessment) {
                        clientPtr->editAdvertAssessment(message, advert, newAssessment);
                    }
                );
            }

            if (usedTextFallback) {
                spdlog::debug(
                    "[Delivery] Listing {} was delivered using the text fallback.",
                    item.ad.id
                );
            }
        };
    }
}
