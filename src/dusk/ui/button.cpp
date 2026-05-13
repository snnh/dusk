#include "button.hpp"

#include "localization.hpp"
#include "ui.hpp"

#include "Z2AudioLib/Z2SeMgr.h"
#include "m_Do/m_Do_audio.h"

#include <utility>

namespace dusk::ui {
namespace {

Rml::Element* createRoot(Rml::Element* parent, const Rml::String& tagName) {
    auto* doc = parent->GetOwnerDocument();
    auto elem = doc->CreateElement(tagName);
    return parent->AppendChild(std::move(elem));
}

}  // namespace

Button::Button(Rml::Element* parent, Props props, const Rml::String& tagName)
    : FluentComponent(createRoot(parent, tagName)) {
    update_props(std::move(props));
}

void Button::set_text(const Rml::String& text) {
    const auto generation = localization::generation();
    if (mProps.text != text || mLocalizationGeneration != generation) {
        mRoot->SetInnerRML(escape(localization::translate(text)));
        mProps.text = text;
        mLocalizationGeneration = generation;
    }
}

void Button::update() {
    set_text(mProps.text);
    Component::update();
}

Button& Button::on_pressed(ButtonCallback callback) {
    if (!callback) {
        return *this;
    }
    // TODO: convert this to a FluentComponent method?
    on_nav_command([callback = std::move(callback)](Rml::Event&, NavCommand cmd) {
        if (cmd == NavCommand::Confirm) {
            callback();
            return true;
        }
        return false;
    });
    return *this;
}

void Button::update_props(Props props) {
    set_text(props.text);
    mProps = std::move(props);
}

void ControlledButton::update() {
    if (mIsDisabled) {
        set_disabled(mIsDisabled());
    }
    if (mIsSelected) {
        set_selected(mIsSelected());
    }
    Button::update();
}

bool ControlledButton::selected() const {
    if (mIsSelected) {
        return mIsSelected();
    }
    return Button::selected();
}

bool ControlledButton::disabled() const {
    if (mIsDisabled) {
        return mIsDisabled();
    }
    return Button::disabled();
}

}  // namespace dusk::ui
