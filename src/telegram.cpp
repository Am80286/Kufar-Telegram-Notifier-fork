//
//  telegram.cpp
//  Kufar Telegram Notifier
//
//  Created by Macintosh on 04.06.2022.
//

#include <iostream>
#include <cstdint>
#include <stdexcept>
#include <optional>
#include "kufar.hpp"
#include "telegram.hpp"
#include "networking.hpp"
#include "json.hpp"

namespace Telegram {
    using namespace std;
    using namespace Networking;
    using nlohmann::json;

    const short int MAX_IMAGES_IN_GROUP = 10;

    string makeImageGroupJSON(const vector<string> &images, const string &caption) {
        json j_array = json::array();
        for (int i = 0; (i < images.size()) && (i < MAX_IMAGES_IN_GROUP); i++){
            json j_list = json::object({
                    {"type", "photo"},
                    {"media", images[i]}
            });
            
            if (i == 0) {
                j_list.push_back({"caption", caption});
            }
            j_array.push_back(j_list);
        }
        
        return j_array.dump();
    }

    namespace {
        /**
          Проверяет ответ Telegram API и бросает исключение с описанием ошибки,
          если запрос не был выполнен. Без этой проверки неудачная отправка
          (например, из-за неверного ID темы) оставалась бы незамеченной,
          а объявление считалось бы отправленным.
         */
        void ensureSuccessfulResponse(const string &response) {
            if (response.empty()) {
                throw runtime_error("Пустой ответ Telegram API (проблема с сетью?)");
            }

            json parsedResponse;
            try {
                parsedResponse = json::parse(response);
            } catch (const exception &) {
                throw runtime_error("Некорректный ответ Telegram API: \"" + response.substr(0, 200) + "\"");
            }

            if (!parsedResponse.value("ok", false)) {
                const int errorCode = parsedResponse.value("error_code", 0);
                const string description = parsedResponse.value("description", "описание отсутствует");
                throw runtime_error("Telegram API [" + to_string(errorCode) + "]: " + description);
            }
        }

        /**
          Параметр message_thread_id для URL запроса.
          Пустая строка, если тема не задана — тогда сообщение уйдёт
          в основной чат (тему "General").
         */
        string threadIDParameter(const optional<int> &messageThreadID) {
            if (!messageThreadID.has_value()) {
                return "";
            }
            return "&message_thread_id=" + to_string(messageThreadID.value());
        }
    }

    void sendAdvert(const TelegramConfiguration &telegramConfiguration, const Kufar::Ad &ad, optional<int> messageThreadID) {
        string formattedTime = ctime(&ad.date);
        formattedTime.pop_back();
        
        string text = "";
        
        if (ad.tag.has_value()) {
            text += "#" + ad.tag.value() + "\n";
        }
        
        text += "Название: " + ad.title + "\n"
                "Дата: " + formattedTime + "\n"
                "Цена: " + to_string(ad.price / 100) + " BYN\n\n"
                "Имя продавца: " + ad.sellerName + "\n"
                "Номер телефона не скрыт: " + (ad.phoneNumberIsVisible ? "Да" : "Нет") + "\n"
                "Ссылка: " + ad.link;
        
        const string threadParameter = threadIDParameter(messageThreadID);
        string url = "https://api.telegram.org/bot" + telegramConfiguration.botToken;
        if (!ad.images.empty()) {
            url += "/sendMediaGroup?chat_id=" + to_string(telegramConfiguration.chatID) + threadParameter + "&"
                   "media=" + urlEncode(makeImageGroupJSON(ad.images, text));
        } else {
            url += "/sendPhoto?chat_id=" + to_string(telegramConfiguration.chatID) + threadParameter + "&"
                   "caption=" + urlEncode(text) + "&"
                   "photo=https://via.placeholder.com/1080";
            /*url += "/sendMessage?chat_id=" + telegramConfiguration.chatID_or_Username + "&"
                   "text=" + urlEncode("[No Photo]\n\n" + text);*/
        }
        
        ensureSuccessfulResponse(getJSONFromURL(url));
    }
};
