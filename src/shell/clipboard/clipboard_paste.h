#pragma once

#include <cstdint>

class VirtualKeyboardService;
enum class ClipboardAutoPasteMode : std::uint8_t;

namespace clipboard_paste {

  [[nodiscard]] bool pasteEntry(bool isImage, ClipboardAutoPasteMode mode, VirtualKeyboardService& virtualKeyboard);

} // namespace clipboard_paste
