#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "market_normalizer.hpp"

using namespace std;

namespace {
    void check(bool condition, const char *expression, int line) {
        if (!condition) {
            throw runtime_error("Check failed at line " + to_string(line) + ": " + expression);
        }
    }

    void addParameter(
        Kufar::Ad &ad,
        const string &name,
        const string &value,
        const string &label = string()
    ) {
        Kufar::AdParameter parameter;
        parameter.name = name;
        parameter.value = value;
        if (!label.empty()) {
            parameter.label = label;
        }
        ad.adParameters.push_back(std::move(parameter));
    }

    Kufar::Ad makeGpuAd(const string &title) {
        Kufar::Ad ad{};
        ad.title = title;
        ad.category = 16000;
        ad.subCategory = 16010;
        addParameter(ad, "computers_component_type", "11");
        return ad;
    }

    void testGpuModelNormalization() {
        Market::ComputerPartsNormalizer normalizer;

        Kufar::Ad structured = makeGpuAd("Видеокарта ASUS KO GeForce RTX 3060 (12GB/GDDR6)");
        addParameter(structured, "computers_component_videocard_cpu", "14", "GeForce RTX 3060");
        addParameter(structured, "computers_desktop_ram", "15", "12 ГБ");

        check(normalizer.supports(structured), "GPU ad is supported", __LINE__);
        const Market::NormalizationResult structuredResult = normalizer.normalize(structured);
        check(structuredResult.key.has_value(), "structured GPU normalized", __LINE__);
        check(structuredResult.key->family == Market::ComponentFamily::Gpu, "structured family", __LINE__);
        check(structuredResult.key->model == "nvidia:rtx-3060", "structured model", __LINE__);
        check(structuredResult.key->variant == "12gb", "structured variant", __LINE__);
        check(structuredResult.key->condition == "any", "default condition", __LINE__);

        Kufar::Ad alias = makeGpuAd("Видеокарта RTX5060 8gb");
        addParameter(alias, "computers_component_videocard_cpu", "20", "GeForce RTX 5060");
        addParameter(alias, "computers_desktop_ram", "7", "8 ГБ");
        const Market::NormalizationResult aliasResult = normalizer.normalize(alias);
        check(aliasResult.key.has_value(), "alias GPU normalized", __LINE__);
        check(aliasResult.key->model == "nvidia:rtx-5060", "alias model", __LINE__);
        check(aliasResult.key->variant == "8gb", "alias variant", __LINE__);

        Kufar::Ad ti = makeGpuAd("Видеокарта RTX 5060 Ti 16GB");
        const Market::NormalizationResult tiResult = normalizer.normalize(ti);
        check(tiResult.key.has_value(), "Ti GPU normalized", __LINE__);
        check(tiResult.key->model == "nvidia:rtx-5060-ti", "Ti model", __LINE__);
        check(tiResult.key->variant == "16gb", "Ti variant", __LINE__);

        Kufar::Ad rangedMemory = makeGpuAd("Видеокарта MSI Gaming X 4060 Ti 8gb");
        addParameter(rangedMemory, "computers_desktop_ram", "12", "8-10 ГБ");
        addParameter(rangedMemory, "computers_component_videocard_cpu", "17", "GeForce RTX 4060 Ti");
        const Market::NormalizationResult rangedMemoryResult = normalizer.normalize(rangedMemory);
        check(rangedMemoryResult.key.has_value(), "ranged structured memory ignored", __LINE__);
        check(rangedMemoryResult.key->model == "nvidia:rtx-4060-ti", "ranged memory model", __LINE__);
        check(rangedMemoryResult.key->variant == "8gb", "ranged memory uses title", __LINE__);

        Kufar::Ad reversedModel = makeGpuAd("Gigabyte 1660 GTX");
        addParameter(reversedModel, "computers_desktop_ram", "8", "6 ГБ");
        addParameter(reversedModel, "computers_component_videocard_cpu", "10", "GeForce GTX 1660 SUPER");
        const Market::NormalizationResult reversedModelResult = normalizer.normalize(reversedModel);
        check(reversedModelResult.key.has_value(), "reversed model uses structured data", __LINE__);
        check(reversedModelResult.key->model == "nvidia:gtx-1660-super", "suffix is part of model", __LINE__);
        check(reversedModelResult.key->variant == "6gb", "model number is not parsed as memory", __LINE__);

        const Market::NormalizationResult reversedBrandResult =
            normalizer.normalize(makeGpuAd("Видеокарта GTX GeForce 1080 Ti 11GB"));
        check(reversedBrandResult.key.has_value(), "reversed brand order normalized", __LINE__);
        check(reversedBrandResult.key->model == "nvidia:gtx-1080-ti", "reversed brand model", __LINE__);
        check(reversedBrandResult.key->variant == "11gb", "reversed brand variant", __LINE__);

        Kufar::Ad amd = makeGpuAd("Видеокарта Radeon RX 5700 XT 8GB");
        const Market::NormalizationResult amdResult = normalizer.normalize(amd);
        check(amdResult.key.has_value(), "AMD GPU normalized", __LINE__);
        check(amdResult.key->model == "amd:rx-5700-xt", "AMD model", __LINE__);
        check(amdResult.key->variant == "8gb", "AMD variant", __LINE__);

        Kufar::Ad intel = makeGpuAd("Видеокарта Intel Arc A770 16GB");
        const Market::NormalizationResult intelResult = normalizer.normalize(intel);
        check(intelResult.key.has_value(), "Intel GPU normalized", __LINE__);
        check(intelResult.key->model == "intel:arc-a770", "Intel model", __LINE__);
        check(intelResult.key->variant == "16gb", "Intel variant", __LINE__);

        Kufar::Ad laptop = makeGpuAd("Видеокарта для ноутбука RTX 3060 6GB");
        const Market::NormalizationResult laptopResult = normalizer.normalize(laptop);
        check(laptopResult.key.has_value(), "laptop GPU normalized", __LINE__);
        check(laptopResult.key->variant == "6gb,laptop", "laptop variant", __LINE__);
    }

    void testGpuRejection() {
        Market::ComputerPartsNormalizer normalizer;

        Kufar::Ad unsupportedType = makeGpuAd("Видеокарта RTX 3060 12GB");
        unsupportedType.adParameters.clear();
        addParameter(unsupportedType, "computers_component_type", "10");
        const Market::NormalizationResult unsupportedResult = normalizer.normalize(unsupportedType);
        check(!unsupportedResult.key.has_value(), "non-GPU component rejected", __LINE__);
        check(
            unsupportedResult.reason == "unsupported computer component type",
            "non-GPU reason",
            __LINE__
        );

        Kufar::Ad missingType = makeGpuAd("Видеокарта RTX 3060 12GB");
        missingType.adParameters.clear();
        const Market::NormalizationResult missingResult = normalizer.normalize(missingType);
        check(!missingResult.key.has_value(), "missing component type rejected", __LINE__);
        check(missingResult.reason == "missing computer component type", "missing type reason", __LINE__);

        const Market::NormalizationResult bundleResult =
            normalizer.normalize(makeGpuAd("RTX 3060 + i5 12400F 16GB"));
        check(!bundleResult.key.has_value(), "bundle rejected", __LINE__);
        check(bundleResult.reason == "bundle listing", "bundle reason", __LINE__);

        const Market::NormalizationResult defectResult =
            normalizer.normalize(makeGpuAd("RX 550 2gb (ошибка 43)"));
        check(!defectResult.key.has_value(), "defective GPU rejected", __LINE__);
        check(defectResult.reason == "defective listing", "defect reason", __LINE__);

        const Market::NormalizationResult multipleResult =
            normalizer.normalize(makeGpuAd("Видеокарта RTX 3060 / RTX 3070"));
        check(!multipleResult.key.has_value(), "multiple GPU models rejected", __LINE__);
        check(multipleResult.reason == "multiple GPU models in title", "multiple model reason", __LINE__);

        const Market::NormalizationResult missingModelResult =
            normalizer.normalize(makeGpuAd("Видеокарта ASUS 8GB"));
        check(!missingModelResult.key.has_value(), "missing GPU model rejected", __LINE__);
        check(missingModelResult.reason == "no GPU model", "missing model reason", __LINE__);

        Kufar::Ad mismatch = makeGpuAd("Видеокарта RTX 3070 8GB");
        addParameter(mismatch, "computers_component_videocard_cpu", "14", "GeForce RTX 3060");
        const Market::NormalizationResult mismatchResult = normalizer.normalize(mismatch);
        check(!mismatchResult.key.has_value(), "model mismatch rejected", __LINE__);
        check(
            mismatchResult.reason == "GPU model mismatch between title and parameters",
            "model mismatch reason",
            __LINE__
        );

        Kufar::Ad memoryMismatch = makeGpuAd("Видеокарта RTX 3060 8GB");
        addParameter(memoryMismatch, "computers_desktop_ram", "15", "12 ГБ");
        const Market::NormalizationResult memoryMismatchResult = normalizer.normalize(memoryMismatch);
        check(memoryMismatchResult.key.has_value(), "title memory wins over structured mismatch", __LINE__);
        check(memoryMismatchResult.key->variant == "8gb", "title memory value", __LINE__);

        const Market::NormalizationResult ambiguousMemoryResult =
            normalizer.normalize(makeGpuAd("Видеокарта RTX 3060 8GB/16GB"));
        check(!ambiguousMemoryResult.key.has_value(), "ambiguous memory rejected", __LINE__);
        check(ambiguousMemoryResult.reason == "ambiguous GPU memory", "ambiguous memory reason", __LINE__);

        const Market::NormalizationResult fullKitResult =
            normalizer.normalize(makeGpuAd("Видеокарта RTX 3060 12GB, полный комплект"));
        check(fullKitResult.key.has_value(), "full kit is not a bundle", __LINE__);
        check(fullKitResult.key->model == "nvidia:rtx-3060", "full kit model", __LINE__);
    }

    void testGpuCrawlScope() {
        Market::ComputerPartsNormalizer normalizer;

        Market::MarketKey key;
        key.family = Market::ComponentFamily::Gpu;
        key.model = "nvidia:rtx-3060";

        const optional<Market::MarketCrawlScope> scope = normalizer.buildCrawlScope(key);
        check(scope.has_value(), "GPU crawl scope built", __LINE__);
        check(scope->id == "subcategory:16010", "crawl scope id", __LINE__);
        check(
            scope->configuration.category == Kufar::Category::computerEquipment,
            "crawl parent category",
            __LINE__
        );
        check(
            scope->configuration.subCategory == int(Kufar::SubCategories::ComputerEquipment::parts),
            "crawl subcategory",
            __LINE__
        );
        check(!scope->configuration.computerComponentType.has_value(), "crawl has no component type", __LINE__);
        check(!scope->configuration.tag.has_value(), "crawl has no search tag", __LINE__);
        check(!scope->configuration.region.has_value(), "crawl is national", __LINE__);
        check(!scope->configuration.priceRange.priceMin.has_value(), "crawl has no minimum price", __LINE__);
        check(!scope->configuration.priceRange.priceMax.has_value(), "crawl has no maximum price", __LINE__);
        check(!scope->configuration.sortType.has_value(), "crawl has no sorting", __LINE__);

        key.model = "nvidia:rtx-5060-ti";
        key.variant = "16gb";
        const optional<Market::MarketCrawlScope> tiScope = normalizer.buildCrawlScope(key);
        check(tiScope.has_value(), "Ti crawl scope built", __LINE__);
        check(tiScope->id == scope->id, "same subcategory shares crawl scope", __LINE__);

        key.family = Market::ComponentFamily::Cpu;
        check(!normalizer.buildCrawlScope(key).has_value(), "unsupported crawl scope rejected", __LINE__);
    }

    void testNormalizerRegistry() {
        Market::MarketNormalizerRegistry emptyRegistry;
        const Market::NormalizationResult emptyResult =
            emptyRegistry.normalize(makeGpuAd("Видеокарта RTX 3060 12GB"));
        check(!emptyResult.key.has_value(), "empty registry rejects", __LINE__);
        check(emptyResult.reason == "no normalizer for listing category", "empty registry reason", __LINE__);

        Market::MarketNormalizerRegistry registry;
        registry.add(make_unique<Market::ComputerPartsNormalizer>());

        const Market::NormalizationResult result =
            registry.normalize(makeGpuAd("Видеокарта RTX 3060 12GB"));
        check(result.key.has_value(), "registry normalizes GPU", __LINE__);
        check(result.key->model == "nvidia:rtx-3060", "registry model", __LINE__);
        check(result.normalizer != nullptr, "registry exposes matched normalizer", __LINE__);
        check(
            result.normalizer->buildCrawlScope(result.key.value()).has_value(),
            "matched normalizer builds crawl scope",
            __LINE__
        );

        Kufar::Ad laptop = makeGpuAd("Ноутбук ASUS RTX 3060");
        laptop.subCategory = 16040;
        const Market::NormalizationResult unsupported = registry.normalize(laptop);
        check(!unsupported.key.has_value(), "unsupported category rejected", __LINE__);
        check(unsupported.reason == "no normalizer for listing category", "unsupported category reason", __LINE__);

        bool threw = false;
        try {
            registry.add(nullptr);
        } catch (const invalid_argument &) {
            threw = true;
        }
        check(threw, "null normalizer rejected", __LINE__);
    }

    void testMarketKeyIdentity() {
        Market::MarketKey first;
        first.family = Market::ComponentFamily::Gpu;
        first.model = "nvidia:rtx-3060";
        first.variant = "12gb";

        Market::MarketKey second = first;
        check(first == second, "equal market keys", __LINE__);
        check(first.toString() == "gpu:nvidia:rtx-3060:12gb:condition=any", "market key string", __LINE__);

        Market::MarketKey different = first;
        different.variant = "8gb";
        check(!(first == different), "different variants are distinct", __LINE__);

        const Market::MarketKey base = first.baseModel();
        check(base.variant.empty(), "base model drops variant", __LINE__);
        check(base.model == first.model, "base model keeps model", __LINE__);
        check(base.family == first.family, "base model keeps family", __LINE__);

        unordered_set<Market::MarketKey, Market::MarketKeyHash> keys;
        keys.insert(first);
        keys.insert(second);
        keys.insert(different);
        check(keys.size() == 2, "market key hashing", __LINE__);
    }
}

int main() {
    const vector<pair<string, function<void()>>> tests = {
        {"GPU model normalization", testGpuModelNormalization},
        {"GPU rejection rules", testGpuRejection},
        {"GPU crawl scope", testGpuCrawlScope},
        {"normalizer registry", testNormalizerRegistry},
        {"market key identity", testMarketKeyIdentity}
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
