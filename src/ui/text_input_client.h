#pragma once

#include <cstdint>
#include <string>

class TextInputService;

enum class TextInputChangeCause : std::uint8_t {
  InputMethod,
  Other,
};

enum class TextInputPurpose : std::uint8_t {
  Normal,
  Password,
};

struct TextInputState {
  std::string surroundingText;
  std::int32_t cursor = 0;
  std::int32_t anchor = 0;
  std::int32_t cursorRectX = 0;
  std::int32_t cursorRectY = 0;
  std::int32_t cursorRectWidth = 1;
  std::int32_t cursorRectHeight = 1;
  TextInputPurpose purpose = TextInputPurpose::Normal;
  bool sendSurroundingText = true;
  bool sensitiveData = false;
  bool hiddenText = false;
  bool preeditVisible = true;
};

struct TextInputEdit {
  std::string commitText;
  std::string preeditText;
  std::uint32_t deleteBeforeLength = 0;
  std::uint32_t deleteAfterLength = 0;
  std::int32_t preeditCursorBegin = 0;
  std::int32_t preeditCursorEnd = 0;
  bool hasCommitText = false;
  bool hasPreedit = false;
  bool hasDelete = false;
  bool submit = false;
};

class TextInputClient {
public:
  virtual ~TextInputClient() = default;

  virtual TextInputState textInputState() const = 0;
  virtual void textInputApplyEdit(const TextInputEdit& edit) = 0;
  virtual void textInputResetPreedit() = 0;
  virtual void textInputActivated(TextInputService& service) = 0;
  virtual void textInputDeactivated(TextInputService& service) = 0;
};
