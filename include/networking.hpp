//
//  networking.hpp
//  Kufar Telegram Notifier
//
//  Created by Macintosh on 04.06.2022.
//

#ifndef networking_hpp
#define networking_hpp

#include <cstddef>
#include <string>

namespace Networking {
    std::string urlEncode(const std::string &);

    // A zero maxResponseSize disables the response size limit.
    std::string getTextFromURL(const std::string &, std::size_t maxResponseSize = 0);
    std::string getJSONFromURL(const std::string &);
};

#endif /* networking_hpp */
