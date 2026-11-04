//
//  kufar_url.hpp
//  Kufar Telegram Notifier
//
//  Created by Am80286 on 01.11.2026
//

#ifndef kufar_url_hpp
#define kufar_url_hpp

#include <string>

#include <nlohmann/json.hpp>

namespace Kufar {
    // Extracts the resolved search parameters from a Kufar search page.
    nlohmann::json extractSearchParameters(const std::string &html);

    // Converts resolved Kufar parameters to the existing query JSON schema.
    // Throws when a search parameter cannot be represented without data loss.
    nlohmann::json convertSearchParameters(const std::string &sourceURL, const nlohmann::json &parameters);

    // Validates and fetches a Kufar search URL, then converts it to query JSON.
    nlohmann::json importSearchQuery(const std::string &sourceURL);
};

#endif /* kufar_url_hpp */
