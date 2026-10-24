//
//  helperfunctions.hpp
//  Kufar Telegram Notifier
//
//  Created by Macintosh on 04.06.2022.
//

#ifndef helperfunctions_hpp
#define helperfunctions_hpp

/**
Определение состояния вывода отладочных
сообщений на экран в зависимости от типа
сборки программы
 */

#ifdef DEBUG
    #define DEBUG_MSG(...) do { spdlog::debug(__VA_ARGS__); } while( false )
#else
    #define DEBUG_MSG(...) do { } while ( false )
#endif

#if defined(WIN32) || defined(_WIN32)
    #define PATH_SEPARATOR '\\'
#else
    #define PATH_SEPARATOR '/'
#endif

#include <optional>
#include <sstream>
#include <string>

#include <spdlog/spdlog.h>

#include "json.hpp"

static const std::string PROPERTY_UNDEFINED = "[UNDEFINED]";

/**
 Получение значения из JSON
 для переменных опционального типа.
 */

template<typename T>
std::optional<T> getOptionalValue(const nlohmann::json &obj, const std::string &key) try {
    return obj.at(key).get<T>();
} catch (...) {
    return std::nullopt;
}

/**
 Вывод переменных опционального типа в удобном формате.
 Значение есть: *unwrapped optional*
 Значения нет: [UNDEFINED]
 
 Если значение типа bool, то вывод будет следующего формата:
 true:  Да
 false: Нет
 */

template<typename T>
std::ostream &operator << (std::ostream &os, std::optional<T> const &opt) {
    if (opt.has_value()) {
        if constexpr (std::is_same_v<T, bool>) {
            return (os << (opt.value() == true ? "Да" : "Нет"));
        }
        return (os << opt.value());
    }
    return (os << PROPERTY_UNDEFINED);
}

/**
  Приведение значения к строке с использованием
  перегруженного оператора вывода (в том числе
  для std::optional).
 */

template<typename T>
std::string toString(const T &value) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
}

/**
  Проверка вектора на наличие заданного значения
 */
bool vectorContains(const std::vector<int> &, const int &);

/**
  Проверка на существование файла
 */

bool fileExists(const std::string &);

/**
  Возврат размера файла
 */
uint64_t getFileSize(const std::string &);

/**
 Чтение файла и возврат
 содержимого в виде string.
 */

std::string getTextFromFile(const std::string &);

/**
 Конвертирование времени Zulu формата
 в unix timestamp
 */

time_t zuluToTimestamp(const std::string &);

/**
 Соединение значений вектора
 с использованием заданного разделителя
 */

std::string joinIntVector(const std::vector<int> &, const std::string &);

/**
 Возвращает дату в формате unix timestamp,
 со здвигом на указанное количество часов
 */

time_t timestampShift(const time_t &, int);

/**
 Сохранение текста в файл с перезаписью
 */
void saveFile(const std::string &path, const std::string &contents);

/**
  Маскирование секретного токена для безопасного вывода в лог.
  Для токена вида "123456:ABC..." возвращает "123456:***WXYZ",
  то есть позволяет опознать токен, не раскрывая его полностью.
 */
std::string maskToken(const std::string &token);

#endif /* helperfunctions_hpp */
