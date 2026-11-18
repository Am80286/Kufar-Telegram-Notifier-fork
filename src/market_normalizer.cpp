//
//  market_normalizer.cpp
//  Kufar Telegram Notifier
//
//  Created by Am80286 on 18.11.2026
//

#include <algorithm>
#include <cctype>
#include <functional>
#include <initializer_list>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "market_normalizer.hpp"
#include "kufar.hpp"

namespace Market {
    using namespace std;
    using namespace Kufar;

    namespace {
        string componentFamilyName(ComponentFamily family) {
            switch (family) {
                case ComponentFamily::Gpu:
                    return "gpu";
                case ComponentFamily::Cpu:
                    return "cpu";
                case ComponentFamily::Motherboard:
                    return "motherboard";
                case ComponentFamily::Ram:
                    return "ram";
                case ComponentFamily::Ssd:
                    return "ssd";
                case ComponentFamily::Hdd:
                    return "hdd";
                case ComponentFamily::Psu:
                    return "psu";
                case ComponentFamily::Unknown:
                    return "unknown";
                default:
                    return "unknown";
            }
        }

        string lowercase(const string &text) {
            string result = text;

            for (size_t index = 0; index < result.size();) {
                const unsigned char first = static_cast<unsigned char>(result[index]);
                if (first < 0x80) {
                    result[index] = static_cast<char>(tolower(first));
                    index++;
                    continue;
                }

                if (index + 1 < result.size()) {
                    const unsigned char second = static_cast<unsigned char>(result[index + 1]);

                    // Cyrillic uppercase letters U+0410..U+042F are encoded as
                    // D0 90..D0 AF and map to D0 B0..D0 BF.
                    if (first == 0xD0 && second >= 0x90 && second <= 0xAF) {
                        result[index] = static_cast<char>(0xD0);
                        result[index + 1] = static_cast<char>(second + 0x20);
                        index += 2;
                        continue;
                    }

                    // Ё (U+0401) maps to ё (U+0451).
                    if (first == 0xD0 && second == 0x81) {
                        result[index] = static_cast<char>(0xD1);
                        result[index + 1] = static_cast<char>(0x91);
                        index += 2;
                        continue;
                    }
                }

                index++;
            }

            return result;
        }

        string normalizeForModelSearch(const string &text) {
            string result = lowercase(text);

            for (char &character : result) {
                if (
                    character == ',' ||
                    character == ';' ||
                    character == '(' ||
                    character == ')' ||
                    character == '/' ||
                    character == '+' ||
                    character == '-'
                ) {
                    character = ' ';
                }
            }

            const auto replaceAll = [](string &value, const string &from, const string &to) {
                size_t position = 0;
                while ((position = value.find(from, position)) != string::npos) {
                    value.replace(position, from.size(), to);
                    position += to.size();
                }
            };

            replaceAll(result, "gtx geforce ", "geforce gtx ");
            replaceAll(result, "gts geforce ", "geforce gts ");
            replaceAll(result, "gtx geforce", "geforce gtx");
            replaceAll(result, "rtx geforce ", "geforce rtx ");
            replaceAll(result, "rtx geforce", "geforce rtx");
            replaceAll(result, "gt geforce ", "geforce gt ");
            replaceAll(result, "gt geforce", "geforce gt");

            return result;
        }

        bool containsAny(const string &text, initializer_list<string_view> tokens) {
            for (const string_view token : tokens) {
                if (text.find(token) != string::npos) {
                    return true;
                }
            }
            return false;
        }

        bool hasDefectSignal(const string &text) {
            return containsAny(text, {
                "не работает",
                "нерабоч",
                "на запчасти",
                "на донора",
                "донор",
                "ошибка",
                "артефакт",
                "не включается",
                "битая",
                "битый",
                "под ремонт",
                "трещин"
            });
        }

        bool hasBundleSignal(const string &text) {
            if (containsAny(text, {
                "сборка",
                "системник",
                "системный блок",
                "набор",
                "бандл"
            })) {
                return true;
            }

            if (text.find('+') != string::npos || text.find(';') != string::npos) {
                return true;
            }

            const bool hasKit = text.find("комплект") != string::npos;
            const bool hasFullKit =
                text.find("полный комплект") != string::npos ||
                text.find("полным комплектом") != string::npos ||
                text.find("в комплекте") != string::npos;

            return hasKit && !hasFullKit;
        }

        bool hasCpuModelSignal(const string &text) {
            static const regex cpuPattern(
                R"(\b(?:intel\s+)?(?:core\s+)?i[3579][- ]?[0-9]{4,5}[a-z]{0,2}\b|\bryzen\s+[3579]\s+[0-9]{4}[a-z]{0,3}\b|\bxeon\b)",
                regex_constants::icase
            );

            return regex_search(text, cpuPattern);
        }

        struct GpuModelMatch {
            string model;
        };

        void addUniqueModel(vector<GpuModelMatch> &models, GpuModelMatch match) {
            const auto existing = find_if(
                models.begin(),
                models.end(),
                [&match](const GpuModelMatch &candidate) {
                    return candidate.model == match.model;
                }
            );

            if (existing == models.end()) {
                models.push_back(std::move(match));
            }
        }

        vector<GpuModelMatch> extractGpuModels(const string &text) {
            const string normalized = normalizeForModelSearch(text);
            vector<GpuModelMatch> models;

            static const regex nvidiaPattern(
                R"(\b(?:geforce\s+)?(rtx|gtx|gts|gt)\s*([0-9]{3,4})(?:\s*(ti|super))?\b)",
                regex_constants::icase
            );

            for (sregex_iterator it(normalized.begin(), normalized.end(), nvidiaPattern), end; it != end; ++it) {
                GpuModelMatch match;
                match.model = "nvidia:" + (*it)[1].str() + "-" + (*it)[2].str();
                if ((*it)[3].matched) {
                    match.model += "-" + (*it)[3].str();
                }
                addUniqueModel(models, std::move(match));
            }

            static const regex amdPattern(
                R"(\b(?:radeon\s+)?(rx|r[579]|hd)\s*([0-9]{3,4})(?:\s*(xtx|xt|gre))?\b)",
                regex_constants::icase
            );

            for (sregex_iterator it(normalized.begin(), normalized.end(), amdPattern), end; it != end; ++it) {
                GpuModelMatch match;
                match.model = "amd:" + (*it)[1].str() + "-" + (*it)[2].str();
                if ((*it)[3].matched) {
                    match.model += "-" + (*it)[3].str();
                }
                addUniqueModel(models, std::move(match));
            }

            static const regex intelPattern(
                R"(\b(?:intel\s+)?arc\s*([ab][0-9]{3})\b)",
                regex_constants::icase
            );

            for (sregex_iterator it(normalized.begin(), normalized.end(), intelPattern), end; it != end; ++it) {
                GpuModelMatch match;
                match.model = "intel:arc-" + (*it)[1].str();
                addUniqueModel(models, std::move(match));
            }

            return models;
        }

        vector<string> extractMemoryValues(const string &text) {
            static const regex memoryPattern(
                R"(\b([0-9]{1,2})\s*(gb|g)\b)",
                regex_constants::icase
            );

            string normalized = lowercase(text);
            const string cyrillicGb = "гб";
            for (size_t position = 0; (position = normalized.find(cyrillicGb, position)) != string::npos;) {
                normalized.replace(position, cyrillicGb.size(), "gb");
            }

            vector<string> values;

            for (sregex_iterator it(normalized.begin(), normalized.end(), memoryPattern), end; it != end; ++it) {
                const string value = (*it)[1].str() + "gb";
                if (find(values.begin(), values.end(), value) == values.end()) {
                    values.push_back(value);
                }
            }

            return values;
        }

        string joinStrings(const vector<string> &values, const string &separator) {
            string result;
            for (size_t index = 0; index < values.size(); index++) {
                if (index > 0) {
                    result += separator;
                }
                result += values[index];
            }
            return result;
        }

        const AdParameter *findParameter(const Ad &ad, string_view name) {
            for (const AdParameter &parameter : ad.adParameters) {
                if (parameter.name == name) {
                    return &parameter;
                }
            }
            return nullptr;
        }

        optional<string> parameterValue(const Ad &ad, string_view name) {
            const AdParameter *parameter = findParameter(ad, name);
            if (parameter == nullptr || !parameter->value.has_value()) {
                return nullopt;
            }
            return parameter->value;
        }

        optional<string> parameterLabel(const Ad &ad, string_view name) {
            const AdParameter *parameter = findParameter(ad, name);
            if (parameter == nullptr || !parameter->label.has_value()) {
                return nullopt;
            }
            return parameter->label;
        }

        optional<int> parameterInteger(const Ad &ad, string_view name) {
            const optional<string> value = parameterValue(ad, name);
            if (!value.has_value()) {
                return nullopt;
            }

            try {
                return stoi(value.value());
            } catch (...) {
                return nullopt;
            }
        }

        optional<string> normalizedStructuredMemory(const Ad &ad) {
            const optional<string> label = parameterLabel(ad, "computers_desktop_ram");
            if (!label.has_value()) {
                return nullopt;
            }

            static const regex memoryRangePattern(
                R"([0-9]{1,2}\s*-\s*[0-9]{1,2}\s*(gb|гб|g))",
                regex_constants::icase
            );

            const string normalized = lowercase(label.value());
            if (regex_search(normalized, memoryRangePattern)) {
                return nullopt;
            }

            const vector<string> values = extractMemoryValues(normalized);
            if (values.size() != 1) {
                return nullopt;
            }
            return values.front();
        }
    }

    string MarketKey::toString() const {
        ostringstream stream;
        stream << componentFamilyName(family) << ':' << model;

        if (!variant.empty()) {
            stream << ':' << variant;
        }

        stream << ":condition=" << condition;
        return stream.str();
    }

    MarketKey MarketKey::baseModel() const {
        MarketKey base = *this;
        base.variant.clear();
        return base;
    }

    size_t MarketKeyHash::operator()(const MarketKey &key) const {
        size_t seed = hash<int>()(static_cast<int>(key.family));

        const auto combine = [&seed](const string &value) {
            seed ^= hash<string>()(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        };

        combine(key.model);
        combine(key.variant);
        combine(key.condition);
        return seed;
    }

    NormalizationResult NormalizationResult::accepted(MarketKey key) {
        NormalizationResult result;
        result.key = std::move(key);
        return result;
    }

    NormalizationResult NormalizationResult::rejected(string reason) {
        NormalizationResult result;
        result.reason = std::move(reason);
        return result;
    }

    void MarketNormalizerRegistry::add(unique_ptr<MarketNormalizer> normalizer) {
        if (!normalizer) {
            throw invalid_argument("Market normalizer must not be null.");
        }

        normalizers_.push_back(std::move(normalizer));
    }

    NormalizationResult MarketNormalizerRegistry::normalize(const Ad &ad) const {
        for (const unique_ptr<MarketNormalizer> &normalizer : normalizers_) {
            if (normalizer->supports(ad)) {
                NormalizationResult result = normalizer->normalize(ad);
                result.normalizer = normalizer.get();
                return result;
            }
        }

        return NormalizationResult::rejected("no normalizer for listing category");
    }

    bool ComputerPartsNormalizer::supports(const Ad &ad) const {
        return ad.subCategory.has_value() && ad.subCategory.value() == int(SubCategories::ComputerEquipment::parts);
    }

    NormalizationResult ComputerPartsNormalizer::normalize(const Ad &ad) const {
        if (!supports(ad)) {
            return NormalizationResult::rejected("unsupported category");
        }

        const optional<int> componentType = parameterInteger(ad, "computers_component_type");
        if (!componentType.has_value()) {
            return NormalizationResult::rejected("missing computer component type");
        }
        if (componentType.value() != int(ComputerComponentType::graphicsCard)) {
            return NormalizationResult::rejected("unsupported computer component type");
        }

        const string lowerTitle = lowercase(ad.title);
        if (hasDefectSignal(lowerTitle)) {
            return NormalizationResult::rejected("defective listing");
        }
        if (hasBundleSignal(lowerTitle)) {
            return NormalizationResult::rejected("bundle listing");
        }
        if (hasCpuModelSignal(normalizeForModelSearch(ad.title))) {
            return NormalizationResult::rejected("listing contains a CPU model");
        }

        const optional<string> structuredLabel = parameterLabel(ad, "computers_component_videocard_cpu");
        vector<GpuModelMatch> structuredModels;
        if (structuredLabel.has_value()) {
            structuredModels = extractGpuModels(structuredLabel.value());
        }

        const vector<GpuModelMatch> titleModels = extractGpuModels(ad.title);

        if (structuredModels.size() > 1) {
            return NormalizationResult::rejected("multiple GPU models in parameters");
        }
        if (titleModels.size() > 1) {
            return NormalizationResult::rejected("multiple GPU models in title");
        }

        if (!structuredModels.empty() && !titleModels.empty()) {
            if (structuredModels.front().model != titleModels.front().model) {
                return NormalizationResult::rejected("GPU model mismatch between title and parameters");
            }
        }

        const vector<GpuModelMatch> &chosenModels =
            !structuredModels.empty() ? structuredModels : titleModels;
        if (chosenModels.empty()) {
            return NormalizationResult::rejected("no GPU model");
        }

        const GpuModelMatch &model = chosenModels.front();

        const vector<string> titleMemoryValues = extractMemoryValues(ad.title);
        if (titleMemoryValues.size() > 1) {
            return NormalizationResult::rejected("ambiguous GPU memory");
        }

        optional<string> memory;
        if (!titleMemoryValues.empty()) {
            memory = titleMemoryValues.front();
        } else {
            memory = normalizedStructuredMemory(ad);
        }

        const bool laptop = containsAny(lowerTitle, {"laptop", "ноутбук", "мобильн", "mobile"});

        vector<string> variantParts;
        if (memory.has_value()) {
            variantParts.push_back(memory.value());
        }
        if (laptop) {
            variantParts.push_back("laptop");
        }

        MarketKey key;
        key.family = ComponentFamily::Gpu;
        key.model = model.model;
        key.variant = joinStrings(variantParts, ",");

        return NormalizationResult::accepted(std::move(key));
    }

    optional<MarketCrawlScope> ComputerPartsNormalizer::buildCrawlScope(const MarketKey &key) const {
        if (key.family != ComponentFamily::Gpu) {
            return nullopt;
        }

        MarketCrawlScope scope;
        scope.id = "subcategory:16010";
        scope.configuration.category = Category::computerEquipment;
        scope.configuration.subCategory = int(SubCategories::ComputerEquipment::parts);
        return scope;
    }

    MarketNormalizerRegistry createDefaultMarketNormalizerRegistry() {
        MarketNormalizerRegistry registry;
        registry.add(make_unique<ComputerPartsNormalizer>());
        return registry;
    }
}
