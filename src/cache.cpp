//
//  cache.cpp
//  Kufar Telegram Notifier
//

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "cache.hpp"
#include "helperfunctions.hpp"

namespace cache {
    using namespace std;
    using json = nlohmann::json;

    namespace {
        constexpr auto SAVE_INTERVAL = chrono::seconds(120);

        queries::QueryID parseFrontierID(const string &key) {
            try {
                if (key.empty() || key.front() == '-') {
                    throw invalid_argument("bad id");
                }

                size_t position = 0;
                const auto id = stoull(key, &position);
                if (position != key.size() || id == 0) {
                    throw invalid_argument("bad id");
                }
                return static_cast<queries::QueryID>(id);
            } catch (const exception &) {
                throw runtime_error("Некорректный ID запроса в файле кеша: \"" + key + "\".");
            }
        }

        time_t parseTimestamp(const json &value, const string &field) {
            if (!value.is_number_integer() && !value.is_number_unsigned()) {
                throw runtime_error("Поле \"" + field + "\" в файле кеша должно быть целым числом.");
            }

            const long long timestamp = value.get<long long>();
            if (timestamp < 0) {
                throw runtime_error("Поле \"" + field + "\" в файле кеша не может быть отрицательным.");
            }
            return static_cast<time_t>(timestamp);
        }
    }

    CacheStore::CacheStore(string path, json document)
        : path_(std::move(path)), lastSave_(Clock::now()) {
        if (document.is_array()) {
            if (!document.empty()) {
                spdlog::warn(
                    "[Кеш] Обнаружен устаревший формат кеша (плоский массив). "
                    "Идентификаторы проигнорированы, кеш будет создан заново."
                );
            }
            return;
        }

        if (!document.is_object() || !document.contains("buckets")) {
            throw runtime_error("Некорректный формат файла кеша: ожидается объект с полем \"buckets\".");
        }

        const int version = document.value("version", VERSION);
        if (version > VERSION) {
            throw runtime_error("Файл кеша создан более новой версией программы (version " + to_string(version) + ").");
        }

        const json &bucketsData = document.at("buckets");
        if (!bucketsData.is_object()) {
            throw runtime_error("Некорректный формат файла кеша: поле \"buckets\" должно быть объектом.");
        }

        for (const auto &[topic, ids] : bucketsData.items()) {
            topics_[topic].delivered = ids.get<unordered_set<int>>();
        }

        if (document.contains("frontiers")) {
            const json &frontiersData = document.at("frontiers");
            if (!frontiersData.is_object()) {
                throw runtime_error("Некорректный формат файла кеша: поле \"frontiers\" должно быть объектом.");
            }

            for (const auto &[key, value] : frontiersData.items()) {
                if (!value.is_object()) {
                    throw runtime_error("Некорректная запись \"frontiers\" в файле кеша.");
                }

                FrontierState state;
                state.frontier = value.contains("frontier")
                    ? parseTimestamp(value.at("frontier"), "frontier")
                    : 0;
                state.suppressBefore = value.contains("suppress-before")
                    ? parseTimestamp(value.at("suppress-before"), "suppress-before")
                    : state.frontier;

                frontiers_[parseFrontierID(key)] = state;
            }
        }
    }

    bool CacheStore::tryClaim(const string &topic, int adID) {
        lock_guard<mutex> lock(mtx_);
        TopicState &state = topics_[topic];

        if (state.delivered.contains(adID) || state.pending.contains(adID)) {
            return false;
        }

        state.pending.insert(adID);
        return true;
    }

    void CacheStore::markDelivered(const string &topic, int adID) {
        lock_guard<mutex> lock(mtx_);
        TopicState &state = topics_[topic];
        state.pending.erase(adID);
        state.delivered.insert(adID);
        dirty_ = true;
    }

    void CacheStore::releaseClaim(const string &topic, int adID) {
        lock_guard<mutex> lock(mtx_);
        topics_[topic].pending.erase(adID);
    }

    optional<FrontierState> CacheStore::getFrontier(queries::QueryID id) const {
        lock_guard<mutex> lock(mtx_);
        const auto iterator = frontiers_.find(id);
        if (iterator == frontiers_.end()) {
            return nullopt;
        }
        return iterator->second;
    }

    void CacheStore::setFrontier(queries::QueryID id, FrontierState state) {
        lock_guard<mutex> lock(mtx_);
        frontiers_[id] = state;
        dirty_ = true;
    }

    CacheStore::Snapshot CacheStore::snapshotLocked() const {
        Snapshot snapshot;

        for (const auto &[topic, state] : topics_) {
            if (!state.delivered.empty()) {
                snapshot.delivered.emplace(topic, state.delivered);
            }
        }

        snapshot.frontiers = frontiers_;
        return snapshot;
    }

    json CacheStore::snapshotToJSON(const Snapshot &snapshot) {
        json buckets = json::object();
        for (const auto &[topic, delivered] : snapshot.delivered) {
            vector<int> ids(delivered.begin(), delivered.end());
            sort(ids.begin(), ids.end());
            buckets[topic] = std::move(ids);
        }

        json frontiers = json::object();
        for (const auto &[id, state] : snapshot.frontiers) {
            frontiers[to_string(id)] = json{
                {"frontier", state.frontier},
                {"suppress-before", state.suppressBefore}
            };
        }

        return json{
            {"version", VERSION},
            {"buckets", std::move(buckets)},
            {"frontiers", std::move(frontiers)}
        };
    }

    json CacheStore::toJSON() const {
        Snapshot snapshot;
        {
            lock_guard<mutex> lock(mtx_);
            snapshot = snapshotLocked();
        }
        return snapshotToJSON(snapshot);
    }

    json CacheStore::emptyDocument() {
        return json{
            {"version", VERSION},
            {"buckets", json::object()},
            {"frontiers", json::object()}
        };
    }

    void CacheStore::saveIfDue(Clock::time_point now) {
        optional<Snapshot> snapshot;
        {
            unique_lock<mutex> lock(mtx_);
            if (!dirty_ || saving_) {
                return;
            }
            if (now - lastSave_ < SAVE_INTERVAL) {
                return;
            }

            snapshot = snapshotLocked();
            saving_ = true;
            dirty_ = false;
            lastSave_ = now;
        }

        const json document = snapshotToJSON(*snapshot);
        bool succeeded = true;
        try {
            saveFile(path_, document.dump(4));
        } catch (const exception &exc) {
            succeeded = false;
            spdlog::error("[Кеш] Не удалось сохранить файл кеша: {}", exc.what());
        }

        {
            lock_guard<mutex> lock(mtx_);
            saving_ = false;
            if (!succeeded) {
                dirty_ = true;
            }
            saveCv_.notify_all();
        }
    }

    void CacheStore::saveNow() {
        optional<Snapshot> snapshot;
        {
            unique_lock<mutex> lock(mtx_);
            saveCv_.wait(lock, [this] { return !saving_; });

            snapshot = snapshotLocked();
            saving_ = true;
            dirty_ = false;
            lastSave_ = Clock::now();
        }

        const json document = snapshotToJSON(*snapshot);
        bool succeeded = true;
        try {
            saveFile(path_, document.dump(4));
        } catch (const exception &exc) {
            succeeded = false;
            spdlog::error("[Кеш] Не удалось сохранить файл кеша: {}", exc.what());
        }

        {
            lock_guard<mutex> lock(mtx_);
            saving_ = false;
            if (!succeeded) {
                dirty_ = true;
            }
            saveCv_.notify_all();
        }
    }
}
