#include "localization.hpp"

#include "aurora/lib/logging.hpp"
#include "aurora/rmlui.hpp"
#include "dusk/io.hpp"
#include "dusk/settings.h"
#include "ui.hpp"

#include <absl/container/flat_hash_map.h>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dusk::ui::localization {
namespace {

aurora::Module Log{"dusk::ui::localization"};

absl::flat_hash_map<std::string, Rml::String> sStrings;
std::optional<GameLanguage> sLoadedLanguage;
std::string sLoadedLocale;
std::size_t sGeneration = 0;

constexpr std::array<Language, 2> kLanguages = {
    Language{"en", "English"},
    Language{"zh-CN", "简体中文"},
};

std::string_view sanitize_locale(std::string_view locale) noexcept {
    for (const auto& language : kLanguages) {
        if (locale == language.id) {
            return language.id;
        }
    }
    return kLanguages.front().id;
}

void skip_whitespace(std::string_view input, std::size_t& pos) noexcept {
    while (pos < input.size() &&
           (input[pos] == ' ' || input[pos] == '\t' || input[pos] == '\r' || input[pos] == '\n'))
    {
        ++pos;
    }
}

std::string xml_decode(std::string_view input) {
    std::string result;
    result.reserve(input.size());
    for (std::size_t i = 0; i < input.size();) {
        if (input[i] != '&') {
            result.push_back(input[i++]);
            continue;
        }

        const auto end = input.find(';', i + 1);
        if (end == std::string_view::npos) {
            result.push_back(input[i++]);
            continue;
        }

        const auto entity = input.substr(i + 1, end - i - 1);
        if (entity == "amp") {
            result.push_back('&');
        } else if (entity == "lt") {
            result.push_back('<');
        } else if (entity == "gt") {
            result.push_back('>');
        } else if (entity == "quot") {
            result.push_back('"');
        } else if (entity == "apos") {
            result.push_back('\'');
        } else {
            result.append(input.substr(i, end - i + 1));
        }
        i = end + 1;
    }
    return result;
}

std::optional<std::string> read_xml_attribute(
    std::string_view tag, std::string_view name) {
    std::size_t pos = 0;
    while (pos < tag.size()) {
        pos = tag.find(name, pos);
        if (pos == std::string_view::npos) {
            return std::nullopt;
        }
        const bool beforeOk =
            pos == 0 || tag[pos - 1] == ' ' || tag[pos - 1] == '\t' || tag[pos - 1] == '\n';
        pos += name.size();
        std::size_t valuePos = pos;
        skip_whitespace(tag, valuePos);
        if (beforeOk && valuePos < tag.size() && tag[valuePos] == '=') {
            ++valuePos;
            skip_whitespace(tag, valuePos);
            if (valuePos < tag.size() && (tag[valuePos] == '"' || tag[valuePos] == '\'')) {
                const char quote = tag[valuePos++];
                const auto end = tag.find(quote, valuePos);
                if (end != std::string_view::npos) {
                    return xml_decode(tag.substr(valuePos, end - valuePos));
                }
            }
        }
    }
    return std::nullopt;
}

void parse_strings_xml(std::string_view xml) {
    std::size_t pos = 0;
    while (true) {
        const auto open = xml.find("<string", pos);
        if (open == std::string_view::npos) {
            return;
        }
        const auto tagEnd = xml.find('>', open);
        if (tagEnd == std::string_view::npos) {
            return;
        }

        const auto tag = xml.substr(open, tagEnd - open + 1);
        auto key = read_xml_attribute(tag, "key");
        if (!key.has_value()) {
            pos = tagEnd + 1;
            continue;
        }

        const auto close = xml.find("</string>", tagEnd + 1);
        if (close == std::string_view::npos) {
            return;
        }

        sStrings[std::move(*key)] = xml_decode(xml.substr(tagEnd + 1, close - tagEnd - 1));
        pos = close + std::string_view{"</string>"}.size();
    }
}

bool load_locale(std::string_view locale) {
    const auto path = resource_path(std::filesystem::path{"i18n"} /
                                    (std::string(locale) + ".xml"));
    try {
        const auto bytes = io::FileStream::ReadAllBytes(path);
        parse_strings_xml(std::string_view{
            reinterpret_cast<const char*>(bytes.data()),
            bytes.size(),
        });
        return true;
    } catch (const std::exception& e) {
        Log.warn("Failed to load localization file '{}': {}", io::fs_path_to_string(path), e.what());
        return false;
    }
}

bool is_token(std::string_view text) noexcept {
    return text.size() >= 3 && text.front() == '[' && text.back() == ']';
}

Rml::String translate_token_or_text(const Rml::String& text) {
    const auto it = sStrings.find(text);
    if (it != sStrings.end()) {
        return it->second;
    }
    return text;
}

}  // namespace

void initialize() noexcept {
    aurora::rmlui::set_translate_callback(
        [](const Rml::String& text) { return translate_rml(text); });
    reload();
}

void shutdown() noexcept {
    aurora::rmlui::set_translate_callback({});
    sStrings.clear();
}

void reload() noexcept {
    sStrings.clear();
    const std::string locale{sanitize_locale(getSettings().backend.uiLanguage.getValue())};
    load_locale(kLanguages.front().id);
    if (locale != kLanguages.front().id) {
        load_locale(locale);
    }
    sLoadedLocale = locale;
    sLoadedLanguage = getSettings().game.language.getValue();
    ++sGeneration;
}

std::span<const Language> available_languages() noexcept {
    return kLanguages;
}

std::size_t generation() noexcept {
    return sGeneration;
}

Rml::String translate(const Rml::String& text) {
    if (sLoadedLocale != sanitize_locale(getSettings().backend.uiLanguage.getValue()) ||
        sLoadedLanguage != getSettings().game.language.getValue())
    {
        reload();
    }
    return translate_token_or_text(text);
}

Rml::String translate_rml(const Rml::String& rml) {
    const auto fullTranslation = translate(rml);
    if (fullTranslation != rml) {
        return fullTranslation;
    }

    Rml::String out;
    out.reserve(rml.size());

    for (std::size_t i = 0; i < rml.size();) {
        const auto open = rml.find('[', i);
        if (open == Rml::String::npos) {
            out.append(rml.substr(i));
            break;
        }

        out.append(rml.substr(i, open - i));
        const auto close = rml.find(']', open + 1);
        if (close == Rml::String::npos) {
            out.append(rml.substr(open));
            break;
        }

        const Rml::String token = rml.substr(open, close - open + 1);
        out.append(is_token(token) ? translate_token_or_text(token) : token);
        i = close + 1;
    }

    return out;
}

}  // namespace dusk::ui::localization
