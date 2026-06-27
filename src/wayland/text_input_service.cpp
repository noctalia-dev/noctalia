#include "wayland/text_input_service.h"

#include "core/log.h"
#include "text-input-unstable-v3-client-protocol.h"

#include <algorithm>
#include <limits>

namespace {

  constexpr Logger kLog("text-input");
  constexpr std::size_t kMaxSurroundingTextBytes = 4000;

  void textInputEnter(void* data, zwp_text_input_v3* /*textInput*/, wl_surface* surface) {
    static_cast<TextInputService*>(data)->handleEnter(surface);
  }

  void textInputLeave(void* data, zwp_text_input_v3* /*textInput*/, wl_surface* surface) {
    static_cast<TextInputService*>(data)->handleLeave(surface);
  }

  void textInputPreeditString(
      void* data, zwp_text_input_v3* /*textInput*/, const char* text, std::int32_t cursorBegin, std::int32_t cursorEnd
  ) {
    static_cast<TextInputService*>(data)->handlePreeditString(text, cursorBegin, cursorEnd);
  }

  void textInputCommitString(void* data, zwp_text_input_v3* /*textInput*/, const char* text) {
    static_cast<TextInputService*>(data)->handleCommitString(text);
  }

  void textInputDeleteSurroundingText(
      void* data, zwp_text_input_v3* /*textInput*/, std::uint32_t beforeLength, std::uint32_t afterLength
  ) {
    static_cast<TextInputService*>(data)->handleDeleteSurroundingText(beforeLength, afterLength);
  }

  void textInputDone(void* data, zwp_text_input_v3* /*textInput*/, std::uint32_t serial) {
    static_cast<TextInputService*>(data)->handleDone(serial);
  }

  void textInputAction(
      void* data, zwp_text_input_v3* /*textInput*/, std::uint32_t action, std::uint32_t /*serial*/
  ) {
    static_cast<TextInputService*>(data)->handleAction(action);
  }

  void textInputLanguage(void* /*data*/, zwp_text_input_v3* /*textInput*/, const char* /*language*/) {}

  void textInputPreeditHint(
      void* /*data*/, zwp_text_input_v3* /*textInput*/, std::uint32_t /*start*/, std::uint32_t /*end*/,
      std::uint32_t /*hint*/
  ) {}

  const zwp_text_input_v3_listener kTextInputListener = {
      .enter = &textInputEnter,
      .leave = &textInputLeave,
      .preedit_string = &textInputPreeditString,
      .commit_string = &textInputCommitString,
      .delete_surrounding_text = &textInputDeleteSurroundingText,
      .done = &textInputDone,
      .action = &textInputAction,
      .language = &textInputLanguage,
      .preedit_hint = &textInputPreeditHint,
  };

  [[nodiscard]] std::uint32_t contentHintFor(const TextInputState& state, std::uint32_t version) {
    std::uint32_t hint = ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE;
    if (state.hiddenText) {
      hint |= ZWP_TEXT_INPUT_V3_CONTENT_HINT_HIDDEN_TEXT;
    }
    if (state.sensitiveData) {
      hint |= ZWP_TEXT_INPUT_V3_CONTENT_HINT_SENSITIVE_DATA;
    }
    if (state.preeditVisible && version >= 2) {
      hint |= ZWP_TEXT_INPUT_V3_CONTENT_HINT_PREEDIT_SHOWN;
    }
    return hint;
  }

  [[nodiscard]] std::uint32_t contentPurposeFor(TextInputPurpose purpose) {
    switch (purpose) {
    case TextInputPurpose::Normal:
      return ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL;
    case TextInputPurpose::Password:
      return ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PASSWORD;
    }
    return ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL;
  }

  [[nodiscard]] bool stateCanSendSurroundingText(const TextInputState& state) {
    return state.sendSurroundingText && state.surroundingText.size() <= kMaxSurroundingTextBytes;
  }

  [[nodiscard]] std::int32_t clampProtocolInt(std::int32_t value) {
    return std::clamp(value, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max());
  }

} // namespace

TextInputService::TextInputService() = default;

TextInputService::~TextInputService() { cleanup(); }

bool TextInputService::bind(zwp_text_input_manager_v3* manager, wl_seat* seat) {
  if (manager == nullptr || seat == nullptr) {
    return false;
  }
  if (m_manager == manager && m_seat == seat && m_textInput != nullptr) {
    return true;
  }

  cleanup();
  m_manager = manager;
  m_seat = seat;
  m_textInput = zwp_text_input_manager_v3_get_text_input(m_manager, m_seat);
  if (m_textInput == nullptr) {
    kLog.warn("failed to create text-input-v3 object");
    cleanup();
    return false;
  }

  m_textInputVersion = static_cast<std::uint32_t>(zwp_text_input_v3_get_version(m_textInput));
  zwp_text_input_v3_add_listener(m_textInput, &kTextInputListener, this);
  kLog.info("text-input-v3 bound");
  return true;
}

void TextInputService::cleanup() {
  if (m_activeClient != nullptr) {
    deactivateClient(m_activeClient);
  }
  if (m_textInput != nullptr) {
    zwp_text_input_v3_destroy(m_textInput);
    m_textInput = nullptr;
  }
  m_manager = nullptr;
  m_seat = nullptr;
  m_enteredSurface = nullptr;
  m_keyboardFocusSurface = nullptr;
  m_keyboardFocusParentSurface = nullptr;
  m_activeSurface = nullptr;
  m_activeClient = nullptr;
  m_activeAcceptsKeyboardFocusActivation = false;
  m_pendingEdit = {};
  m_commitSerial = 0;
  m_textInputVersion = 0;
  m_enabled = false;
}

bool TextInputService::isAvailable() const noexcept { return m_textInput != nullptr; }

void TextInputService::setFocusedClient(
    wl_surface* surface, TextInputClient* client, bool acceptKeyboardFocusActivation,
    wl_surface* keyboardFocusParentSurface
) {
  if (surface == nullptr || client == nullptr) {
    clearFocusedClient(m_activeClient);
    return;
  }
  if (m_textInput == nullptr) {
    return;
  }

  m_activeAcceptsKeyboardFocusActivation = acceptKeyboardFocusActivation;
  m_keyboardFocusParentSurface = keyboardFocusParentSurface;

  if (m_activeClient == client && m_activeSurface == surface) {
    if (!m_enabled) {
      enableActive(TextInputChangeCause::Other);
    } else {
      commitActiveState(TextInputChangeCause::Other);
    }
    return;
  }

  disableActive();
  if (m_activeClient != nullptr) {
    deactivateClient(m_activeClient);
  }

  m_activeSurface = surface;
  m_activeClient = client;
  m_activeClient->textInputActivated(*this);
  enableActive(TextInputChangeCause::Other);
}

void TextInputService::clearFocusedClient(TextInputClient* client) {
  if (client == nullptr || client != m_activeClient) {
    return;
  }
  disableActive();
  deactivateClient(client);
  m_activeClient = nullptr;
  m_activeSurface = nullptr;
  m_activeAcceptsKeyboardFocusActivation = false;
  m_keyboardFocusParentSurface = nullptr;
  m_pendingEdit = {};
}

void TextInputService::notifyClientStateChanged(TextInputClient* client, TextInputChangeCause cause) {
  if (client == nullptr || client != m_activeClient) {
    return;
  }
  commitActiveState(cause);
}

void TextInputService::onKeyboardFocusSurface(wl_surface* surface, bool entered) {
  if (entered) {
    m_keyboardFocusSurface = surface;
  } else if (m_keyboardFocusSurface == surface) {
    m_keyboardFocusSurface = nullptr;
  }

  if (m_activeClient != nullptr && entered && activeSurfaceAcceptsTextInput()) {
    enableActive(TextInputChangeCause::Other);
  }
}

bool TextInputService::activeSurfaceAcceptsTextInput() const noexcept {
  if (m_activeSurface == nullptr) {
    return false;
  }
  if (m_enteredSurface == m_activeSurface) {
    return true;
  }
  if (!m_activeAcceptsKeyboardFocusActivation || m_keyboardFocusSurface == nullptr) {
    return false;
  }
  if (m_keyboardFocusSurface == m_activeSurface) {
    return true;
  }
  return m_keyboardFocusParentSurface != nullptr && m_keyboardFocusSurface == m_keyboardFocusParentSurface;
}

void TextInputService::handleEnter(wl_surface* surface) {
  m_enteredSurface = surface;
  m_pendingEdit = {};
  if (m_activeClient != nullptr && m_activeSurface == surface) {
    enableActive(TextInputChangeCause::Other);
  }
}

void TextInputService::handleLeave(wl_surface* surface) {
  if (m_enteredSurface != surface) {
    return;
  }
  m_enteredSurface = nullptr;
  m_enabled = false;
  m_pendingEdit = {};
  if (m_activeClient != nullptr && m_activeSurface == surface) {
    m_activeClient->textInputResetPreedit();
  }
}

void TextInputService::handlePreeditString(const char* text, std::int32_t cursorBegin, std::int32_t cursorEnd) {
  m_pendingEdit.hasPreedit = true;
  m_pendingEdit.preeditText = text != nullptr ? text : "";
  m_pendingEdit.preeditCursorBegin = cursorBegin;
  m_pendingEdit.preeditCursorEnd = cursorEnd;
}

void TextInputService::handleCommitString(const char* text) {
  m_pendingEdit.hasCommitText = true;
  m_pendingEdit.commitText = text != nullptr ? text : "";
}

void TextInputService::handleDeleteSurroundingText(std::uint32_t beforeLength, std::uint32_t afterLength) {
  m_pendingEdit.hasDelete = true;
  m_pendingEdit.deleteBeforeLength = beforeLength;
  m_pendingEdit.deleteAfterLength = afterLength;
}

void TextInputService::handleDone(std::uint32_t serial) {
  TextInputEdit edit = std::move(m_pendingEdit);
  m_pendingEdit = {};

  if (m_activeClient != nullptr) {
    m_activeClient->textInputApplyEdit(edit);
    if (serial == m_commitSerial) {
      commitActiveState(TextInputChangeCause::InputMethod);
    }
  }
}

void TextInputService::handleAction(std::uint32_t action) {
  if (action == ZWP_TEXT_INPUT_V3_ACTION_SUBMIT) {
    m_pendingEdit.submit = true;
  }
}

void TextInputService::enableActive(TextInputChangeCause cause) {
  if (m_textInput == nullptr || m_activeClient == nullptr || m_activeSurface == nullptr) {
    return;
  }
  if (!activeSurfaceAcceptsTextInput()) {
    return;
  }

  if (!m_enabled) {
    zwp_text_input_v3_enable(m_textInput);
    m_enabled = true;
  }
  commitActiveState(cause);
}

void TextInputService::disableActive() {
  if (m_textInput == nullptr || !m_enabled) {
    m_enabled = false;
    return;
  }
  if (activeSurfaceAcceptsTextInput()) {
    zwp_text_input_v3_disable(m_textInput);
    commitProtocolState();
    wl_surface_commit(m_activeSurface);
  }
  m_enabled = false;
}

void TextInputService::commitActiveState(TextInputChangeCause cause) {
  if (m_textInput == nullptr || m_activeClient == nullptr || m_activeSurface == nullptr || !m_enabled) {
    return;
  }
  if (!activeSurfaceAcceptsTextInput()) {
    return;
  }

  const TextInputState state = m_activeClient->textInputState();
  if (stateCanSendSurroundingText(state)) {
    zwp_text_input_v3_set_surrounding_text(m_textInput, state.surroundingText.c_str(), state.cursor, state.anchor);
  }
  zwp_text_input_v3_set_content_type(
      m_textInput, contentHintFor(state, m_textInputVersion), contentPurposeFor(state.purpose)
  );
  zwp_text_input_v3_set_text_change_cause(
      m_textInput,
      cause == TextInputChangeCause::InputMethod ? ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_INPUT_METHOD
                                                 : ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_OTHER
  );
  zwp_text_input_v3_set_cursor_rectangle(
      m_textInput, clampProtocolInt(state.cursorRectX), clampProtocolInt(state.cursorRectY),
      std::max<std::int32_t>(1, state.cursorRectWidth), std::max<std::int32_t>(1, state.cursorRectHeight)
  );
  commitProtocolState();
  wl_surface_commit(m_activeSurface);
}

void TextInputService::commitProtocolState() {
  zwp_text_input_v3_commit(m_textInput);
  ++m_commitSerial;
}

void TextInputService::deactivateClient(TextInputClient* client) {
  client->textInputResetPreedit();
  client->textInputDeactivated(*this);
}
