//
//  telegram.hpp
//  Kufar Telegram Notifier
//
//  Created by Macintosh on 04.06.2022.
//

#ifndef Header_hpp
#define Header_hpp

#include <cstdint>
#include <optional>
#include <string>

#include "kufar.hpp"

namespace Telegram {
    struct TelegramConfiguration {
        std::string botToken;
        // ID супергруппы/чата. Отрицательный для групп и каналов.
        int64_t chatID = 0;
    };

    /**
      Отправка объявления в чат.
      Если messageThreadID задан, объявление публикуется в соответствующую
      тему (topic) форума, иначе — в основной чат / тему "General".
     */
    void sendAdvert(const TelegramConfiguration &, const Kufar::Ad &, std::optional<int> messageThreadID);
};
#endif /* Header_hpp */
