cmake_minimum_required(VERSION 3.16)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

project(
    Kufar-Telegram-Notifier
    VERSION 2.6
)

include_directories(include)

find_package(CURL REQUIRED)
find_package(spdlog REQUIRED)
find_package(nlohmann_json 3.7.3 REQUIRED)

# tgbot-cpp is not packaged by most distributions, so fetch a pinned release.
set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)

include(FetchContent)

# tgbot-cpp's minimum CMake version predates the removal of the FindBoost module.
set(CMAKE_POLICY_DEFAULT_CMP0167 NEW)

FetchContent_Declare(
    tgbot_cpp
    GIT_REPOSITORY https://github.com/reo7sp/tgbot-cpp.git
    GIT_TAG v1.13
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(tgbot_cpp)

add_executable(
    Kufar-Telegram-Notifier
    
    src/kufar.cpp
    src/helperfunctions.cpp
    src/telegram.cpp
    src/networking.cpp
    src/main.cpp
    src/logging.cpp
    src/arguments.cpp
    src/queries.cpp
)

target_compile_definitions(Kufar-Telegram-Notifier PRIVATE APP_VERSION="${PROJECT_VERSION}")

target_link_libraries(Kufar-Telegram-Notifier PRIVATE curl spdlog::spdlog TgBot::TgBot)
