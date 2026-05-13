#include "select_button.hpp"

#include "localization.hpp"
#include "ui.hpp"

#include <fmt/format.h>
#include <utility>

namespace dusk::ui {
namespace {

Rml::Element* createRoot(Rml::Element* parent) {
    auto* doc = parent->GetOwnerDocument();
    auto elem = doc->CreateElement("select-button");
    return parent->AppendChild(std::move(elem));
}

}  // namespace

SelectButton::SelectButton(Rml::Element* parent, Props props)
    : FluentComponent(createRoot(parent)) {
    mKeyElem = append(mRoot, "key");
    mIconElem = append(mRoot, "icon");
    mValueElem = append(mRoot, "value");
    update_props(std::move(props));
    on_nav_command([this](Rml::Event&, NavCommand cmd) { return handle_nav_command(cmd); });
}

bool SelectButton::modified() const {
    return mProps.modified;
}

void SelectButton::update() {
    update_props(mProps);
    Component::update();
}

void SelectButton::set_modified(bool value) {
    const auto generation = localization::generation();
    if (mProps.modified != value || mValueLocalizationGeneration != generation) {
        mValueElem->SetClass("modified", value);
        const auto translatedValue = localization::translate(mProps.value);
        if (value) {
            mValueElem->SetInnerRML(fmt::format("•&nbsp;{}", escape(translatedValue)));
        } else {
            mValueElem->SetInnerRML(escape(translatedValue));
        }
        mProps.modified = value;
        mValueLocalizationGeneration = generation;
    }
}

void SelectButton::set_value_label(const Rml::String& value) {
    const auto generation = localization::generation();
    if (mProps.value != value || mValueLocalizationGeneration != generation) {
        const auto translatedValue = localization::translate(value);
        if (mProps.modified) {
            mValueElem->SetInnerRML(fmt::format("•&nbsp;{}", escape(translatedValue)));
        } else {
            mValueElem->SetInnerRML(escape(translatedValue));
        }
        mProps.value = value;
        mValueLocalizationGeneration = generation;
    }
}

SelectButton& SelectButton::on_pressed(SelectButtonCallback callback) {
    if (!callback) {
        return *this;
    }
    listen(Rml::EventId::Submit, [this, callback = std::move(callback)](Rml::Event& event) {
        if (!disabled() && event.GetTargetElement() == mRoot) {
            callback();
            event.StopPropagation();
        }
    });
    return *this;
}

void SelectButton::update_props(Props props) {
    const auto generation = localization::generation();
    if (mProps.key != props.key || mKeyLocalizationGeneration != generation) {
        mKeyElem->SetInnerRML(escape(localization::translate(props.key)));
        mKeyLocalizationGeneration = generation;
    }
    if (mProps.icon != props.icon) {
        Rml::StringList iconClasses;
        Rml::StringUtilities::ExpandString(iconClasses, mIconElem->GetClassNames(), ' ', true);
        for (const auto& className : iconClasses) {
            mIconElem->SetClass(className, false);
        }
        if (!props.icon.empty()) {
            mIconElem->SetClass(props.icon, true);
        }
    }
    set_value_label(props.value);
    set_modified(props.modified);
    mProps = std::move(props);
}

bool SelectButton::handle_nav_command(NavCommand cmd) {
    if (cmd == NavCommand::Confirm && mProps.submit) {
        mRoot->DispatchEvent(Rml::EventId::Submit, {});
        return true;
    }
    return false;
}

void BaseControlledSelectButton::update() {
    update_props(mProps);
    set_disabled(disabled());
    set_value_label(format_value());
    set_modified(modified());
    SelectButton::update();
}

bool ControlledSelectButton::modified() const {
    if (mIsModified) {
        return mIsModified();
    }
    return BaseControlledSelectButton::modified();
}

bool ControlledSelectButton::disabled() const {
    if (mIsDisabled) {
        return mIsDisabled();
    }
    return BaseControlledSelectButton::disabled();
}

Rml::String ControlledSelectButton::format_value() {
    if (!mGetValue) {
        return "";
    }
    return mGetValue();
}

}  // namespace dusk::ui
