//
//  market_normalizer.hpp
//  Kufar Telegram Notifier
//
//  Created by Am80286 on 18.11.2026
//

#ifndef market_normalizer_hpp
#define market_normalizer_hpp

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kufar.hpp"

namespace Market {
    enum class ComponentFamily {
        Gpu,
        Cpu,
        Motherboard,
        Ram,
        Ssd,
        Hdd,
        Psu,
        Unknown
    };

    struct MarketKey {
        ComponentFamily family = ComponentFamily::Unknown;
        std::string model;
        std::string variant;
        std::string condition = "any";

        bool operator==(const MarketKey &) const = default;
        std::string toString() const;

        // The same market without memory/form-factor qualifiers.
        MarketKey baseModel() const;
    };

    struct MarketKeyHash {
        std::size_t operator()(const MarketKey &) const;
    };

    struct MarketCrawlScope {
        std::string id;
        Kufar::KufarConfiguration configuration;
    };

    class MarketNormalizer;

    struct NormalizationResult {
        std::optional<MarketKey> key;
        std::string reason;

        // Set by MarketNormalizerRegistry so callers can use the same
        // normalizer to build a crawl for the returned key.
        const MarketNormalizer *normalizer = nullptr;

        static NormalizationResult accepted(MarketKey key);
        static NormalizationResult rejected(std::string reason);
    };

    class MarketNormalizer {
        public:
            virtual ~MarketNormalizer() = default;

            virtual bool supports(const Kufar::Ad &ad) const = 0;
            virtual NormalizationResult normalize(const Kufar::Ad &ad) const = 0;
            virtual std::optional<MarketCrawlScope> buildCrawlScope(const MarketKey &key) const = 0;
    };

    class MarketNormalizerRegistry {
        public:
            void add(std::unique_ptr<MarketNormalizer> normalizer);
            NormalizationResult normalize(const Kufar::Ad &ad) const;

        private:
            std::vector<std::unique_ptr<MarketNormalizer>> normalizers_;
    };

    class ComputerPartsNormalizer : public MarketNormalizer {
        public:
            bool supports(const Kufar::Ad &ad) const override;
            NormalizationResult normalize(const Kufar::Ad &ad) const override;
            std::optional<MarketCrawlScope> buildCrawlScope(const MarketKey &key) const override;
    };

    MarketNormalizerRegistry createDefaultMarketNormalizerRegistry();
}

#endif /* market_normalizer_hpp */
