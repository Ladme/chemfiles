/* Chemfiles, an efficient IO library for chemistry file formats
 * Copyright (C) 2015 Guillaume Fraux
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/
*/
#ifndef CHEMFILES_UTILS_HPP
#define CHEMFILES_UTILS_HPP

#include "chemfiles/Logger.hpp"

#include <vector>
#include <string>
#include <sstream>
#include <cctype>
#include <algorithm>

inline std::vector<std::string> split(const std::string &s, char delim) {
    std::stringstream ss(s);
    std::string item;
    std::vector<std::string> elems;
    while (std::getline(ss, item, delim)) {
        elems.push_back(item);
    }
    return elems;
}

inline std::string trim(const std::string& str) {
    auto front = std::find_if_not(str.begin(), str.end(), [](int c) {
        return std::isspace(c);
    });
    auto back = std::find_if_not(str.rbegin(), str.rend(), [](int c) {
        return std::isspace(c);
    }).base();
    return (back <= front ? std::string() : std::string(front, back));
}

#ifndef __has_builtin
  #define __has_builtin(x) 0
#endif

#if __has_builtin(__builtin_unreachable)
    #define unreachable() __builtin_unreachable()
#elif GCC_VERSION >= 40500
    #define unreachable() __builtin_unreachable()
#elif defined(_MSC_VER)
    #define unreachable() __assume(false)
#else
    #define unreachable() do {                                                     \
        Logger::error("Entered unreachable code at ", __FILE__, ":", __LINE__);    \
        abort();                                                                   \
    } while (false)
#endif


#endif
