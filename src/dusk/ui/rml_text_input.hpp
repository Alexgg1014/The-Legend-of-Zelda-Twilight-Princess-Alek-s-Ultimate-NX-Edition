#pragma once

// aleks2 (v1.1-beta2): the system keyboard for any RmlUi text <input>, not only the game's own string controls
// (string_button.cpp). A mod that writes its own <input> in RML -- Twilit Essentials 3's "Search settings" -- had a
// field nothing could type into on the console. With a native keyboard (platform_text_input().is_native()), activating
// such a field (A / Enter on it, or a tap) opens it once; an accepted text becomes the field's value and the field
// dispatches "change", as typing would. Elsewhere nothing changes.
namespace dusk::ui::rml_text_input {

// Once per frame from ui::update(): listens on the context the first time, then runs the keyboard of a requested edit.
void update() noexcept;

}  // namespace dusk::ui::rml_text_input
