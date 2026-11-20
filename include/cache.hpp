//
//  cache.hpp
//  Kufar Telegram Notifier
//
//  Shared, thread-safe store for delivered listings, in-flight claims and
//  persisted polling frontiers.
//

#ifndef cache_hpp
#define cache_hpp

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "queries.hpp"

namespace cache {
    using Clock = std::chrono::steady_clock;

    struct FrontierState {
        std::time_t frontier = 0;
        std::time_t suppressBefore = 0;
    };

    class CacheStore {
    public:
        static constexpr int VERSION = 3;

        CacheStore(std::string path, nlohmann::json document);

        // Atomically reserves an ad for delivery. Returns false when the ad is
        // already delivered or already queued/sending in this topic.
        bool tryClaim(const std::string &topic, int adID);

        // Moves a claimed ad into the delivered set.
        void markDelivered(const std::string &topic, int adID);

        // Drops a claim without marking the ad as delivered.
        void releaseClaim(const std::string &topic, int adID);

        std::optional<FrontierState> getFrontier(queries::QueryID id) const;
        void setFrontier(queries::QueryID id, FrontierState state);

        // Writes at most once per save interval, if there are pending changes.
        void saveIfDue(Clock::time_point now);

        // Forces a write. Must be called after the producer/consumer threads
        // have been stopped; it waits for an in-progress save to finish.
        void saveNow();

        nlohmann::json toJSON() const;
        static nlohmann::json emptyDocument();

    private:
        struct TopicState {
            std::unordered_set<int> delivered;
            std::unordered_set<int> pending;
        };

        struct Snapshot {
            std::map<std::string, std::unordered_set<int>> delivered;
            std::unordered_map<queries::QueryID, FrontierState> frontiers;
        };

        Snapshot snapshotLocked() const;
        static nlohmann::json snapshotToJSON(const Snapshot &snapshot);

        std::string path_;
        mutable std::mutex mtx_;
        std::condition_variable saveCv_;
        std::map<std::string, TopicState> topics_;
        std::unordered_map<queries::QueryID, FrontierState> frontiers_;
        bool dirty_ = false;
        bool saving_ = false;
        Clock::time_point lastSave_;
    };
}

#endif /* cache_hpp */
