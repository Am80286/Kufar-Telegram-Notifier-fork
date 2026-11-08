//
//  queries.hpp
//  Kufar Telegram Notifier
//
//  Created by Am80286 on 01.11.2026
//

#ifndef queries_hpp
#define queries_hpp

#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <optional>

#include "kufar.hpp"
#include <nlohmann/json.hpp>

namespace queries {
    using QueryID = std::uint64_t;

    const std::string DEFAULT_CACHE_BUCKET = "default";

    struct QueryConfiguration {
        QueryID id;
        Kufar::KufarConfiguration kufar;
        std::optional<int> messageThreadID;

        std::string cacheKey() const;  
        std::string getDescription() const;
        std::string getDescriptionConcise() const;
    };

    QueryConfiguration parseQuery(const nlohmann::json &query);
    std::string describeQuery(const QueryConfiguration &query);

    class QueryStore {
        public:
            QueryStore(std::string path, nlohmann::json json);

            std::vector<QueryConfiguration> getQueries() const;
            nlohmann::json getRawQueries() const;
            int queryCount() const;

            void add(nlohmann::json &query);
            std::optional<QueryConfiguration> remove(QueryID id);

        private:
            std::string path_;
            nlohmann::json json_;
            std::optional<QueryID> nextQueryID_ = 1;
            mutable std::mutex mtx_;
    };
};

#endif /* queries_hpp */
