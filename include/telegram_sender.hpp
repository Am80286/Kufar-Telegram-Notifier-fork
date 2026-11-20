//
//  telegram_sender.hpp
//  Kufar Telegram Notifier
//
//  Production send function for the delivery worker.
//

#ifndef telegram_sender_hpp
#define telegram_sender_hpp

#include "delivery.hpp"

namespace Telegram {
    class Client;
}

namespace Market {
    class MarketAnalyzer;
}

namespace delivery {
    SendFunction makeTelegramSender(
        Telegram::Client &client,
        Market::MarketAnalyzer &marketAnalyzer
    );
}

#endif /* telegram_sender_hpp */
