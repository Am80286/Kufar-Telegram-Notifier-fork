//
//  helperfunctions.cpp
//  Kufar Telegram Notifier
//
//  Created by Macintosh on 04.06.2022.
//

#include <vector>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <iterator>
#include <algorithm>
#include <cstdint>
#include <spdlog/spdlog.h>

#include "helperfunctions.hpp"

using namespace std;

bool vectorContains(const vector<int> &vector, const int &value) {
    if (find(vector.begin(), vector.end(), value) != vector.end()) {
        return true;
    }
    return false;
}

bool fileExists(const string &path) {
    ifstream f(path);
    return f.good();
}

uint64_t getFileSize(const string &path) {
    ifstream f(path, ios::binary | ios::ate);
    return f.tellg();
}

string getTextFromFile(const string &path) {
    ifstream ifs(path);
    return string((istreambuf_iterator<char>(ifs)),
                  (istreambuf_iterator<char>()));
}

time_t zuluToTimestamp(const string &zuluDate) {
    tm t{};
    istringstream stringStream(zuluDate);
    
    stringStream >> get_time(&t, "%Y-%m-%dT%H:%M:%S");
    if (stringStream.fail()) {
        throw runtime_error{"failed to parse time string"};
    }
    
    return mktime(&t);
}

string joinIntVector(const vector<int> &nums, const string &delim) {
    stringstream result;
    copy(nums.begin(), nums.end(), std::ostream_iterator<int>(result, delim.c_str()));
    
    string temp = result.str();
    
    if (!temp.empty()) {
        temp.pop_back();
    }
    
    return temp;
}

time_t timestampShift(const time_t &timestamp, int shift) {
    return timestamp + (3600 * shift);
}

bool stringHasPrefix(const string &originalString, const string &prefix) {
    return originalString.rfind(prefix, 0) == 0;
}

void saveFile(const string &path, const string &contents) {
    // Перезапись файла [ВКЛ]
    spdlog::debug("[Сохранение идентификаторов отправленных объявлений]");
    ofstream ofs(path, ofstream::trunc);
    ofs << contents;
    ofs.close();
}

string maskToken(const string &token) {
    if (token.empty()) {
        return PROPERTY_UNDEFINED;
    }

    const size_t separator = token.find(':');
    if (separator == string::npos) {
        // Неизвестный формат: оставляем только последние 4 символа.
        if (token.size() <= 4) {
            return string(token.size(), '*');
        }
        return "***" + token.substr(token.size() - 4);
    }

    const string botID = token.substr(0, separator);
    const string secret = token.substr(separator + 1);
    const string tail = secret.size() > 4 ? secret.substr(secret.size() - 4) : secret;

    return botID + ":***" + tail;
}

