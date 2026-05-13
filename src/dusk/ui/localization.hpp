#pragma once

#include <RmlUi/Core/StringUtilities.h>
#include <span>
#include <string_view>

namespace dusk::ui::localization {

void initialize() noexcept;
void shutdown() noexcept;
void reload() noexcept;

struct Language {
    std::string_view id;
    std::string_view name;
};

std::span<const Language> available_languages() noexcept;
std::size_t generation() noexcept;
Rml::String translate(const Rml::String& text);
Rml::String translate_text_rml(const Rml::String& text);
Rml::String translate_rml(const Rml::String& rml);

}  // namespace dusk::ui::localization
