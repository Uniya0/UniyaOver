#pragma once

#include <string>

namespace ir::i18n {

enum class Lang { Ru, En };

// Process-wide UI language; set from settings at startup and by the language switch.
void set_language(Lang lang);
Lang language();

// "ru" | "en"; anything else (e.g. empty = not chosen yet) follows the Windows UI language.
Lang from_code(const std::string& code);
const char* to_code(Lang lang);

inline const wchar_t* tr(const wchar_t* ru, const wchar_t* en) {
    return language() == Lang::En ? en : ru;
}

}  // namespace ir::i18n
