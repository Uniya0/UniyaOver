#include "util/i18n.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <atomic>

namespace ir::i18n {
namespace {

std::atomic<Lang> g_lang{Lang::Ru};

Lang system_language() {
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
        case LANG_RUSSIAN:
        case LANG_UKRAINIAN:
        case LANG_BELARUSIAN:
        case LANG_KAZAK:
        case LANG_UZBEK:
        case LANG_KYRGYZ:
        case LANG_TATAR:
        case LANG_ARMENIAN:
        case LANG_GEORGIAN:
        case LANG_AZERI:
            return Lang::Ru;
        default:
            return Lang::En;
    }
}

}  // namespace

void set_language(Lang lang) {
    g_lang.store(lang);
}

Lang language() {
    return g_lang.load();
}

Lang from_code(const std::string& code) {
    if (code == "ru") {
        return Lang::Ru;
    }
    if (code == "en") {
        return Lang::En;
    }
    return system_language();
}

const char* to_code(Lang lang) {
    return lang == Lang::En ? "en" : "ru";
}

}  // namespace ir::i18n
