### Build stage
FROM debian:trixie-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git ca-certificates \
        libcurl4-openssl-dev libssl-dev libspdlog-dev libboost-dev nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY include include
COPY src src

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
 && cmake --build build --parallel

### Runtime stage
FROM debian:trixie-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates libcurl4t64 libspdlog1.15 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/bin/Kufar-Telegram-Notifier /usr/local/bin/kufar-notifier

WORKDIR /data
USER 1000:1000

ENTRYPOINT ["kufar-notifier"]

CMD ["--config", "/data/kufar-configuration.json", "--cache", "/data/cached-data.json", "--no-log"]
