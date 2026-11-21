**English** | [Русский](README.ru.md)

# Kufar Telegram Notifier

A self-hosted Telegram bot that watches [Kufar](https://kufar.by) searches and posts new listings to a chat or forum topic. Each search is scheduled independently, Telegram delivery is queued separately from polling, and graphics-card listings get a market price reference.

Notifications are written in Russian and sent as HTML, with photos or a media group when the listing has them.

> **Disclaimer:** This project is for informational and educational purposes only. It is not affiliated with or endorsed by Kufar. Use it at your own risk and in compliance with Kufar's terms of service and applicable law.

## Requirements

- C++20 compiler (`g++` or `clang++`)
- CMake 3.16+
- libcurl, spdlog, Boost, and nlohmann-json development headers
- Git and network access for the first configure (tgbot-cpp is downloaded automatically)
- Docker and Docker Compose (optional)

## Build

### Linux

Debian / Ubuntu:

```sh
sudo apt update
sudo apt install -y build-essential cmake git libcurl4-openssl-dev libspdlog-dev libboost-dev nlohmann-json3-dev
```

Fedora:

```sh
sudo dnf install -y gcc-c++ make cmake git libcurl-devel spdlog-devel boost-devel json-devel
```

Arch Linux:

```sh
sudo pacman -S --needed base-devel cmake git curl spdlog boost nlohmann-json
```

### macOS

```sh
xcode-select --install
brew install cmake boost nlohmann-json curl spdlog
```

If CMake cannot find Homebrew packages, add `-DCMAKE_PREFIX_PATH="$(brew --prefix)"` to the configure command.

### Compile

```sh
git clone https://github.com/Am80286/Kufar-Telegram-Notifier-fork.git
cd Kufar-Telegram-Notifier-fork
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The executable is `build/bin/Kufar-Telegram-Notifier`. The first configure downloads tgbot-cpp v1.13 into `build/_deps`; later builds reuse it. If you already have the source, pass `-DFETCHCONTENT_SOURCE_DIR_TGBOT_CPP=/path/to/tgbot-cpp`.

### Tests

Run `ctest --output-on-failure` from the `build` directory. Use `-DBUILD_TESTING=OFF` when configuring to skip the tests.

## Docker

The image runs as UID/GID `1000`, stores the configuration and cache in `/data`, and logs to the container output.

```sh
mkdir -p data
cp kufar-configuration.json data/kufar-configuration.json
# edit data/kufar-configuration.json

docker compose up -d --build
docker compose logs -f
docker compose down
```

`data` must be writable by UID `1000` because the bot rewrites the configuration and cache. The Compose file makes the rest of the filesystem read-only, drops capabilities, enables `no-new-privileges`, and restarts the container unless stopped.

Without Compose, run `docker build -t kufar-telegram-notifier .` and then `docker run --rm -v "$PWD/data:/data" kufar-telegram-notifier`.

The image starts with `--config /data/kufar-configuration.json --cache /data/cached-data.json --no-log`.

## Run

Run the bot from a writable directory; the cache and log files are created there by default.

```sh
./build/bin/Kufar-Telegram-Notifier
```

At startup the bot verifies the token and access to `chat-id`; if the check fails, it logs the error and exits. Stop it with `Ctrl+C` or `SIGTERM`; the cache is saved on a clean shutdown.

Polling and Telegram delivery run independently. Detected listings are queued and sent at about 20 messages per minute; transient send errors are retried automatically, and a failed listing is released for the next poll.

### Command-line options

| Option | Description |
| --- | --- |
| `-c`, `--config <file>` | Configuration file. Default: `kufar-configuration.json`. |
| `--cache <file>` | Cache file. Default: `cached-data.json`. Created if missing. |
| `-l`, `--log <file>` | Log file. Default: `kufar-notifier.log`. |
| `--no-log` | Log to the console only. |
| `--log-level <level>` | `trace`, `debug`, `info`, `warn`, `error`, or `critical`. Default: `info`. |
| `-V`, `--version` | Print the version and exit. |
| `-h`, `--help` | Print usage and exit. |

## Telegram commands

The bot uses long polling, so no public IP or webhook is required. Replies go to the same chat and topic. `/add` and `/del` are ignored outside the configured chat.

| Command | Description |
| --- | --- |
| `/start` | Show the bot status. |
| `/add <URL> [label]` | Add a search from a Kufar search-results URL. |
| `/list` | List searches in the current topic. |
| `/del <N>` | Delete search number `N` from the current topic. |

`/add` accepts `https` links to search pages on `www.kufar.by`, `auto.kufar.by`, and `re.kufar.by`; the path must be `/l` or start with `/l/`. The page is fetched in the background, the query is bound to the current topic, and the optional label (up to 64 characters) is stored for `/list`. Unknown search filters cause an error. The web UI `size` parameter is ignored because polling uses its own page size.

`/list` numbers searches per topic, and `/del N` removes the search with that number from the current topic. `/add` and `/del` rewrite the configuration file.

## Configuration

The bot reads `kufar-configuration.json` by default. The file contains `telegram`, `queries`, and optional `scheduler` and `kufar` sections.

Minimal example:

```json
{
    "telegram": {
        "bot-token": "1111111111:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "chat-id": -1001234567890
    },
    "queries": [
        {
            "tag": "iPhone",
            "label": "Phones in Minsk",
            "message-thread-id": 42,
            "only-title-search": true,
            "poll-interval-seconds": 120,
            "region": 7
        }
    ],
    "scheduler": {
        "default-interval-seconds": 60,
        "min-interval-seconds": 60,
        "max-interval-seconds": 300,
        "jitter-percent": 15
    },
    "kufar": {
        "requests-per-second": 4,
        "burst": 4,
        "cooldown-seconds": 30
    }
}
```

### Telegram

Create a bot with [@BotFather](https://t.me/BotFather) and add it to the target chat. For separate topics, enable **Topics** in a supergroup and give the bot permission to post there.

| Key | Required | Description |
| --- | --- | --- |
| `bot-token` | Yes | Token issued by BotFather. |
| `chat-id` | Yes | Target chat ID. Group and supergroup IDs are negative. |

### Queries

Each object in `queries` is one search feed. All fields are optional, but a query without filters returns Kufar's default results.

| Key | Type | Description |
| --- | --- | --- |
| `id` | integer | Internal query ID. Managed by the bot; do not edit. |
| `tag` | string | Search text. Also shown as a hashtag when it has no spaces. |
| `label` | string | Display name for `/list` and logs, up to 64 characters. |
| `message-thread-id` | integer | Topic ID for this query (must be positive). If omitted, posts to the General topic. |
| `poll-interval-seconds` | integer | Desired polling interval for this query (must be positive). |
| `only-title-search` | boolean | Search only in titles. |
| `price` | object | Price range in whole BYN. |
| `price.min` | integer | Minimum price. |
| `price.max` | integer | Maximum price. |
| `language` | string | Search language, for example `ru`. |
| `currency` | string | Currency code sent to Kufar. Notifications show BYN. |
| `condition` | integer | `1` = used, `2` = new. |
| `seller-type` | integer | `0` = private person, `1` = company. |
| `kufar-delivery-required` | boolean | Only Kufar Delivery listings. |
| `kufar-payment-required` | boolean | Only Kufar Payment listings. |
| `kufar-halva-required` | boolean | Only Kufar Halva listings. |
| `only-with-photos` | boolean | Only listings with photos. |
| `only-with-videos` | boolean | Only listings with videos. |
| `only-with-exchange-available` | boolean | Only listings that allow exchange. |
| `sort-type` | integer | Price sorting: `1` = descending, `2` = ascending. |
| `category` | integer | Main category ID. |
| `sub-category` | integer | Subcategory ID. |
| `computer-component-type` | integer | Component type for subcategory `16010`. |
| `region` | integer | Region ID. |
| `areas` | array of integers | Area IDs. |
| `source-url` | string | Kufar URL the query was imported from. Informational. |

Boolean filters are sent only when `true`. The removed `limit` field is ignored if present.

Without `sort-type`, queries are polled newest-first. A normal poll reads up to 10 pages of 200 listings and stops after passing a 10-minute overlap below the previous frontier. If the page limit is reached, older listings in that window are skipped with a warning. A cold start (new query or no saved frontier) posts only the 10 newest listings. Queries with `sort-type` examine only the 10 newest entries on the current page.

### Scheduler

| Key | Default | Description |
| --- | --- | --- |
| `default-interval-seconds` | `60` | Starting interval for queries without `poll-interval-seconds`. |
| `min-interval-seconds` | same as default | Minimum interval. |
| `max-interval-seconds` | `max(300, min)` | Maximum interval after idle backoff. |
| `jitter-percent` | `15` | Random ± variation applied to each interval, in percent. |

Each query is scheduled independently. Its base interval is the larger of `poll-interval-seconds` (or `default-interval-seconds`) and `min-interval-seconds`. The interval starts at that base and resets to it when new listings are enqueued. After polls that enqueue nothing, it grows by 1.5× up to the maximum. `poll-interval-seconds` overrides the default for one query.

The old `delays` section is deprecated. `delays.query` is ignored. If `scheduler` is absent, `delays.loop` is used as a fallback interval.

### Kufar request limits

All Kufar HTTP requests share one limiter and cooldown.

| Key | Default | Description |
| --- | --- | --- |
| `requests-per-second` | `4` | Sustained request rate. |
| `burst` | `4` | Maximum burst size. |
| `cooldown-seconds` | `30` | Base pause after HTTP 429, 5xx, or transport errors. |

Repeated 5xx and transport failures back off exponentially up to 5 minutes. `Retry-After` is honored.

### Categories

| ID | Category | ID | Category |
| --- | --- | --- | --- |
| `1000` | Real estate | `12000` | Everything for children and mothers |
| `2000` | Cars and transport | `13000` | Services |
| `3000` | Everything for home | `14000` | Repair and construction |
| `4000` | Hobbies, sports and tourism | `15000` | Household appliances |
| `5000` | Electronics | `16000` | Computer equipment |
| `6000` | Jobs | `17000` | Phones and tablets |
| `7000` | Other | `18000` | Beauty and health |
| `8000` | Women's wardrobe | `19000` | Men's wardrobe |
| `9000` | Wedding and holidays | `20000` | Ready business and equipment |
| `10000` | Garden | `21000` | Furniture |
| `11000` | Animals | | |

Subcategories and area IDs are listed in [`include/kufar.hpp`](include/kufar.hpp).

### Computer component types

Use `computer-component-type` with `"sub-category": 16010`. For example, graphics cards use `11`.

| ID | Component type |
| --- | --- |
| `1` | Cases |
| `3` | Coolers |
| `5` | Power supplies and batteries |
| `7` | Motherboards |
| `8` | Laptops for parts |
| `9` | RAM |
| `10` | Processors |
| `11` | Graphics cards |
| `13` | Hard drives |
| `15` | SSDs |
| `17` | Sound cards |
| `19` | Optical drives |
| `21` | Other computer components |

### Regions

| ID | Region |
| --- | --- |
| `1` | Brest region |
| `2` | Gomel region |
| `3` | Grodno region |
| `4` | Mogilev region |
| `5` | Minsk region |
| `6` | Vitebsk region |
| `7` | Minsk city |

`region` and `areas` can be combined.

### Topics

To find a topic ID, open a message link inside the topic:

```text
https://t.me/c/1234567890/42/123
```

Here `42` is the topic ID. The cache is kept per topic, so the same listing is sent only once to each topic, even if several queries share it.

## Cache file

`cached-data.json` stores delivered listing IDs per topic and the polling frontier for each query. It is created automatically and should not be edited while the bot is running.

```json
{
    "version": 3,
    "buckets": {
        "default": [123456789],
        "42": [123456789, 987654321]
    },
    "frontiers": {
        "1": {
            "frontier": 1732123456,
            "suppress-before": 1732119856
        }
    }
}
```

`default` is the General topic; other keys are topic IDs. `frontiers` lets polling resume from the previous position after a restart instead of cold-starting. Version 2 caches are loaded with an empty frontier state; an old flat-array cache is ignored.

A listing is claimed before sending and marked delivered only after Telegram confirms it. Failed sends are retried on a later poll. The cache is written atomically at most every two minutes and on a clean shutdown; after an abrupt kill, recently sent listings may be sent again. Deleting a query with `/del` does not remove its cache entries.

## Price assessment

For graphics cards, the notification shows the market median and whether the price is below or above it. This is an work in progress feature, hence the limt on only graphics card listings.

Only listings in subcategory `16010` with component type `11` are rated. The bot identifies the GPU model and memory from the structured parameters or the title; laptop GPUs are a separate variant. Defective or parts listings, bundles, listings containing a CPU model, ambiguous or conflicting models, ambiguous memory, and listings without a recognizable model are skipped.

Baselines are crawled in the background from the whole Components subcategory, nationwide, ignoring the query's filters. Prices outside 1.5× the interquartile range are removed, and the median of the rest is used. A specific model/variant needs at least 10 samples; otherwise the base model needs at least 5. If neither is available, the listing is sent without an assessment.

Snapshots are held in memory, reused for 6 hours, and refreshed in the background; a failed refresh is retried after 5 minutes. A truncated crawl still publishes a partial baseline. If no snapshot is ready, the listing is sent immediately and its message is edited when the crawl finishes.

## Logging

By default the bot writes `kufar-notifier.log` and also prints to the console. Use `--no-log`, `--log <file>`, and `--log-level <level>` to change this.

## Credits

This project is a fork of [TechUnRestricted/Kufar-Telegram-Notifier](https://github.com/TechUnRestricted/Kufar-Telegram-Notifier).
## License

Apache License 2.0. See [LICENSE](LICENSE).
