#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "request_gate.hpp"

using namespace std;
using namespace std::chrono_literals;

namespace {
    void check(bool condition, const char *expression, int line) {
        if (!condition) {
            throw runtime_error("Check failed at line " + to_string(line) + ": " + expression);
        }
    }

    Networking::RequestGateConfig makeConfig() {
        Networking::RequestGateConfig configuration;
        configuration.requestsPerSecond = 2.0;
        configuration.burst = 2;
        configuration.cooldown = 10s;
        return configuration;
    }

    void testBurstAndRefill() {
        auto current = Networking::GateClock::time_point{} + 1000s;
        Networking::RequestGate gate(
            makeConfig(),
            {},
            [&] { return current; }
        );

        const auto start = current;
        check(gate.tryAcquireAt(start), "first burst token", __LINE__);
        check(gate.tryAcquireAt(start), "second burst token", __LINE__);
        check(!gate.tryAcquireAt(start), "burst exhausted", __LINE__);

        check(gate.tryAcquireAt(start + 500ms), "one token after 0.5s", __LINE__);
        check(!gate.tryAcquireAt(start + 500ms), "second token not ready", __LINE__);
        check(gate.tryAcquireAt(start + 1s), "one token after 1s", __LINE__);
    }

    void testPause() {
        auto current = Networking::GateClock::time_point{} + 2000s;
        Networking::RequestGate gate(
            makeConfig(),
            {},
            [&] { return current; }
        );

        const auto start = current;
        gate.pauseFor(5s);

        check(!gate.tryAcquireAt(start + 100ms), "paused request is denied", __LINE__);
        check(gate.tryAcquireAt(start + 5s), "pause expires", __LINE__);
    }

    void testFailureCooldowns() {
        auto current = Networking::GateClock::time_point{} + 3000s;
        Networking::RequestGate gate(
            makeConfig(),
            {},
            [&] { return current; }
        );

        const auto start = current;

        gate.reportFailure(429, 3s);
        check(!gate.tryAcquireAt(start + 1s), "429 cooldown blocks", __LINE__);
        check(gate.status().pausedFor >= 2s, "429 pause remaining", __LINE__);
        check(gate.tryAcquireAt(start + 3s), "429 cooldown expires", __LINE__);

        current = start + 10s;
        gate.reportSuccess();
        gate.reportFailure(500);
        check(!gate.tryAcquireAt(current), "5xx cooldown blocks", __LINE__);
        check(gate.status().consecutiveFailures == 1, "failure counted", __LINE__);

        gate.reportSuccess();
        check(gate.status().consecutiveFailures == 0, "success resets failures", __LINE__);

        gate.reportFailure(400);
        check(gate.status().consecutiveFailures == 0, "client errors do not count", __LINE__);
    }

    void testStop() {
        auto current = Networking::GateClock::time_point{} + 4000s;
        bool stop = false;
        Networking::RequestGate gate(
            makeConfig(),
            [&] { return stop; },
            [&] { return current; }
        );

        check(gate.acquire(), "first request allowed", __LINE__);
        stop = true;
        check(!gate.acquire(), "stopped gate denies requests", __LINE__);
    }
}

int main() {
    const vector<pair<string, function<void()>>> tests = {
        {"burst and refill", testBurstAndRefill},
        {"pause", testPause},
        {"failure cooldowns", testFailureCooldowns},
        {"stop", testStop}
    };

    int failures = 0;
    for (const auto &[name, test] : tests) {
        try {
            test();
            cout << "[PASS] " << name << '\n';
        } catch (const exception &exception) {
            failures++;
            cerr << "[FAIL] " << name << ": " << exception.what() << '\n';
        }
    }

    if (failures > 0) {
        cerr << failures << " test(s) failed.\n";
        return 1;
    }

    cout << tests.size() << " test(s) passed.\n";
    return 0;
}
