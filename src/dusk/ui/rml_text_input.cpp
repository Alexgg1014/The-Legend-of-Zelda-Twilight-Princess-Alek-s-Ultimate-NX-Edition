#include "rml_text_input.hpp"

#include "platform_text_input.hpp"

#include <aurora/rmlui.hpp>

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>

#include <cstdlib>
#include <string>

namespace dusk::ui::rml_text_input {
namespace {

bool takes_text(Rml::Element* element) {
    if (element == nullptr || element->GetTagName() != "input" || element->HasAttribute("disabled") ||
        element->HasAttribute("readonly"))
    {
        return false;
    }
    const Rml::String type = element->GetAttribute<Rml::String>("type", "text");
    return type == "text" || type == "search" || type == "password" || type == "email" || type == "number" ||
           type == "url" || type.empty();
}

class Activation final : public Rml::EventListener {
public:
    void ProcessEvent(Rml::Event& event) override {
        Rml::Element* target = event.GetTargetElement();
        if (!takes_text(target) || !platform_text_input().is_native()) {
            return;
        }
        if (event.GetId() == Rml::EventId::Keydown) {
            const auto key = static_cast<Rml::Input::KeyIdentifier>(
                event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN));
            if (key != Rml::Input::KI_RETURN && key != Rml::Input::KI_NUMPADENTER) {
                return;
            }
        }
        if (mSession.request()) {
            mTarget = target->GetObserverPtr();
        }
        event.StopPropagation();
    }

    void pump() {
        if (!mSession.busy()) {
            return;
        }
        Rml::Element* element = mTarget.get();
        TextInputRequest request;
        if (element != nullptr) {
            request.initialText = element->GetAttribute<Rml::String>("value", "");
            request.header = element->GetAttribute<Rml::String>("placeholder", "");
            request.numeric = element->GetAttribute<Rml::String>("type", "text") == "number";
            request.maxLength = element->GetAttribute<int>("maxlength", -1);
        }
        mSession.pump(
            platform_text_input(), request,
            [this](std::string value) {
                // The field may have gone while the keyboard was up (its window closed): the text is dropped.
                if (auto* field = rmlui_dynamic_cast<Rml::ElementFormControl*>(mTarget.get())) {
                    field->SetValue(value);
                    field->DispatchEvent(Rml::EventId::Change, {{"value", Rml::Variant{value}}});
                }
            },
            [this] {
                if (Rml::Element* field = mTarget.get()) {
                    field->Focus(true);
                }
            });
    }

private:
    NativeTextEditSession mSession;
    Rml::ObserverPtr<Rml::Element> mTarget;
};

Activation s_activation;
Rml::Context* s_listening = nullptr;

}  // namespace

void update() noexcept {
    Rml::Context* context = aurora::rmlui::get_context();
    if (context == nullptr) {
        return;
    }
    if (s_listening != context) {
        // Capture phase, so the field's own handlers (and the document's navigation) never see the activation.
        context->GetRootElement()->AddEventListener(Rml::EventId::Click, &s_activation, true);
        context->GetRootElement()->AddEventListener(Rml::EventId::Keydown, &s_activation, true);
        s_listening = context;
    }
    s_activation.pump();
}

}  // namespace dusk::ui::rml_text_input
