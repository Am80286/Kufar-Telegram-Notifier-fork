cmake_minimum_required(VERSION 3.5)

set(CMAKE_CXX_STANDARD 17)
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

add_executable(
    Kufar-Telegram-Notifier
    
    src/kufar.cpp
    src/helperfunctions.cpp
    src/telegram.cpp
    src/networking.cpp
    src/main.cpp
    src/logging.cpp
)

target_link_libraries(Kufar-Telegram-Notifier PRIVATE curl spdlog::spdlog)
