//
//  delivery.hpp
//  Kufar Telegram Notifier
//
//  Bounded delivery queue and sender thread. Detection and delivery are
//  decoupled so polling never waits for Telegram.
//

#ifndef delivery_hpp
#define delivery_hpp

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include "cache.hpp"
#include "queries.hpp"

namespace delivery {
    struct DeliveryItem {
        queries::QueryID queryID = 0;
        std::string cacheKey;
        std::optional<int> messageThreadID;
        Kufar::Ad ad;
    };

    enum class SendFailureKind {
        Transient,
        RateLimited,
        Permanent
    };

    class SendException : public std::runtime_error {
    public:
        SendException(
            SendFailureKind kind,
            const std::string &message,
            std::chrono::seconds retryAfter = std::chrono::seconds(0)
        );

        SendFailureKind kind() const;
        std::chrono::seconds retryAfter() const;

    private:
        SendFailureKind kind_;
        std::chrono::seconds retryAfter_;
    };

    using SendFunction = std::function<void(const DeliveryItem &)>;
    using QueryExistsFunction = std::function<bool(queries::QueryID)>;

    class DeliverySink {
    public:
        virtual ~DeliverySink() = default;
        virtual bool enqueue(DeliveryItem item) = 0;
    };

    struct DeliveryOptions {
        int messagesPerMinute = 20; // 0 disables pacing
        std::size_t queueCapacity = 10000;
        int maxAttempts = 3;
        std::chrono::milliseconds retryBackoff{2000};
        int maxRateLimitWaits = 10;
    };

    class DeliveryWorker : public DeliverySink {
    public:
        DeliveryWorker(
            SendFunction send,
            cache::CacheStore &cache,
            QueryExistsFunction queryExists,
            DeliveryOptions options = DeliveryOptions()
        );
        ~DeliveryWorker() override;

        DeliveryWorker(const DeliveryWorker &) = delete;
        DeliveryWorker &operator=(const DeliveryWorker &) = delete;

        bool enqueue(DeliveryItem item) override;

        // Stops accepting new items, drains for up to `grace`, then stops.
        void stopAndDrain(std::chrono::milliseconds grace);

    private:
        using Clock = std::chrono::steady_clock;

        void run();
        void process(DeliveryItem item);
        bool waitFor(std::chrono::milliseconds duration);
        bool waitForSendSlot();
        void release(const DeliveryItem &item, const std::string &reason);

        SendFunction send_;
        cache::CacheStore &cache_;
        QueryExistsFunction queryExists_;
        DeliveryOptions options_;
        std::chrono::milliseconds sendSpacing_;

        std::mutex mtx_;
        std::condition_variable cv_;
        std::deque<DeliveryItem> queue_;
        bool stopping_ = false;
        Clock::time_point drainDeadline_;
        Clock::time_point nextSendAllowed_;

        std::thread worker_;
    };
}

#endif /* delivery_hpp */
