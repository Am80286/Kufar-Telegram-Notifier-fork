//
//  request_gate.hpp
//  Kufar Telegram Notifier
//
//  Process-wide request budget and cooldown state for Kufar HTTP calls.
//

#ifndef request_gate_hpp
#define request_gate_hpp

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>

namespace Networking {
    using GateClock = std::chrono::steady_clock;
    using GateNowFunction = std::function<GateClock::time_point()>;
    using GateStopFunction = std::function<bool()>;

    struct RequestGateConfig {
        double requestsPerSecond = 4.0;
        int burst = 4;
        std::chrono::seconds cooldown{30};
    };

    struct RequestGateStatus {
        double tokens = 0.0;
        double requestsPerSecond = 0.0;
        int burst = 0;
        std::chrono::milliseconds pausedFor{0};
        int consecutiveFailures = 0;
    };

    class RequestGate {
    public:
        RequestGate(
            RequestGateConfig configuration,
            GateStopFunction shouldStop = GateStopFunction(),
            GateNowFunction now = GateNowFunction()
        );

        RequestGate(const RequestGate &) = delete;
        RequestGate &operator=(const RequestGate &) = delete;

        // Blocks until a request may start. Returns false when shutdown is
        // requested while waiting.
        bool acquire();

        // Non-blocking token consumption at an explicit time. Used by tests
        // and by acquire() internally.
        bool tryAcquireAt(GateClock::time_point now);

        void reportSuccess();

        // Applies a global cooldown when the failure indicates that requests
        // should slow down (429, 5xx or transport errors).
        void reportFailure(
            int statusCode,
            std::optional<std::chrono::seconds> retryAfter = std::nullopt
        );

        void pauseFor(std::chrono::seconds duration);

        RequestGateStatus status() const;

    private:
        void refillLocked(GateClock::time_point now);
        void pauseLocked(std::chrono::seconds duration);
        GateClock::time_point currentTime() const;

        RequestGateConfig config_;
        GateStopFunction shouldStop_;
        GateNowFunction now_;

        mutable std::mutex mtx_;
        std::condition_variable cv_;
        double tokens_;
        GateClock::time_point lastRefill_;
        GateClock::time_point pausedUntil_;
        int consecutiveFailures_ = 0;
    };
}

#endif /* request_gate_hpp */
