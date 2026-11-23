#include "filemanager/dialogs/checksum_dialog.h"

#include "core/input/key_modifiers.h"
#include "filemanager/dialogs/modal_dialog_host.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/progress_bar.h"
#include "ui/dialogs/file_dialog_popup.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/native_data_transfer.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <gio/gio.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace filemanager {

  namespace {
    constexpr int kRunChecksum = 1;
    constexpr int kCopyChecksum = 2;
    constexpr int kCloseChecksum = 3;
    constexpr std::array kChecksumAlgorithms{
        checksum::Algorithm::Sha256, checksum::Algorithm::Sha512, checksum::Algorithm::Md5
    };
  } // namespace

  struct ChecksumDialogController::ChecksumJob {
    std::filesystem::path target;
    std::filesystem::path manifest;
    checksum::Algorithm algorithm;
    std::string expected;
    checksum::Result result;
    std::shared_ptr<GCancellable> cancel{g_cancellable_new(), g_object_unref};
    std::atomic<int> percent{-1};
    std::optional<AdminSessionClient::OperationTransport> protectedOperation;
  };

  ChecksumDialogController::ChecksumDialogController(
      ModalDialogHost& dialogHost, NativeDataTransfer& transfer, WaylandConnection& wayland, Callbacks callbacks
  )
      : m_dialogHost(dialogHost), m_transfer(transfer), m_wayland(wayland), m_callbacks(std::move(callbacks)) {}

  ChecksumDialogController::~ChecksumDialogController() {
    *m_alive = false;
    dismiss();
  }

  std::array<ContextMenuControlEntry, 2> ChecksumDialogController::menuEntries(bool enabled) {
    return {
        {{.id = kCalculateMenuId, .label = "Calculate Checksum…", .enabled = enabled},
         {.id = kVerifyMenuId, .label = "Verify Checksum…", .enabled = enabled}}
    };
  }

  void ChecksumDialogController::show(const std::filesystem::path& target, bool verify, ProtectedChecksum protectedChecksum) {
    if (m_dialogHost.isOpen())
      return;
    m_dialog = std::make_unique<DialogState>();
    m_dialog->target = target;
    m_dialog->verify = verify;
    m_dialog->protectedChecksum = std::move(protectedChecksum);
    const std::unique_ptr<char, decltype(&g_free)> filename(g_filename_display_basename(target.c_str()), g_free);
    ModalDialogHost::CustomContent custom{
        .buildFields = [this](Flex& root, float width) { buildFields(root, width); },
        .bindChoiceButton =
            [this](int result, Button* button) {
              if (!m_dialog)
                return;
              if (result == kRunChecksum)
                m_dialog->run = button;
              else if (result == kCopyChecksum)
                m_dialog->copy = button;
              else if (result == kCloseChecksum)
                m_dialog->close = button;
            },
        .afterBuild = [this] { updateControls(); },
        .initialFocus =
            [this]() -> InputArea* {
              return m_dialog && m_dialog->expectedInput ? m_dialog->expectedInput->inputArea() : nullptr;
            },
        .onThemeUpdated =
            [this] {
              if (m_dialog && m_dialog->status) {
                const auto text = m_dialog->status->text();
                setStatus(m_dialog->statusTone, text);
              }
            },
        .interceptResponse = [this](int response) { return handleResponse(response); },
        .onBeforeClose =
            [this] {
              if (m_dialog && m_dialog->job)
                g_cancellable_cancel(m_dialog->job->cancel.get());
            },
        .onDismiss = [this] { dismiss(); },
        .useMeasuredMinHeight = true,
    };
    m_dialogHost.showDialog(
        verify ? "Verify Checksum" : "Calculate Checksum", filename.get(),
        {{"Close", kCloseChecksum}, {"Copy", kCopyChecksum}, {verify ? "Verify" : "Calculate", kRunChecksum}},
        [this](int) { dismiss(); }, nullptr, false, false, std::move(custom)
    );
  }

  bool ChecksumDialogController::handleResponse(int response) {
    if (!m_dialog)
      return false;
    if (response == kRunChecksum) {
      start();
      return true;
    }
    if (response == kCopyChecksum) {
      auto& dialog = *m_dialog;
      const auto& hash = dialog.calculated->value();
      if (hash.empty())
        return true;
      dialog.copyFeedbackTimer.stop();
      const bool copied = m_transfer.setSelection({{"text/plain;charset=utf-8", hash}}, m_wayland.lastInputSerial());
      dialog.copy->setText(copied ? "✓ Copied!" : "Copy");
      if (copied) {
        dialog.copyFeedbackTimer.start(std::chrono::seconds(2), [this] {
          if (m_dialog && m_dialog->copy) {
            m_dialog->copy->setText("Copy");
            m_dialogHost.requestLayout();
          }
        });
      }
      m_dialogHost.requestLayout();
      return true;
    }
    if (response == kCloseChecksum && m_dialog->job) {
      stop();
      return true;
    }
    return false;
  }

  void ChecksumDialogController::buildFields(Flex& root, float width) {
    auto& dialog = *m_dialog;
    const auto addLabel = [&root, width](std::string_view text, bool secondary = false) {
      auto label = std::make_unique<Label>();
      label->setText(text);
      label->setMaxWidth(width - 48.0F);
      label->setMaxLines(2);
      if (secondary) {
        label->setFontSize(Style::fontSizeCaption);
        label->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      }
      auto* control = label.get();
      root.addChild(std::move(label));
      return control;
    };
    const auto addInput = [this, &root](std::string_view placeholder, bool readOnly) {
      auto input = std::make_unique<Input>();
      input->setControlHeight(34.0F);
      input->setMinLayoutWidth(0.0F);
      input->setPlaceholder(placeholder);
      input->setReadOnly(readOnly);
      if (readOnly)
        input->setOnCopyText([this](std::string text) {
          if (m_callbacks.writeClipboardText)
            m_callbacks.writeClipboardText(std::move(text));
        });
      auto* control = input.get();
      root.addChild(std::move(input));
      return control;
    };
    addLabel("Algorithm");
    auto algorithms = std::make_unique<Flex>();
    algorithms->setAlign(FlexAlign::Center);
    algorithms->setGap(8.0F);
    for (std::size_t i = 0; i < kChecksumAlgorithms.size(); ++i) {
      const auto algorithm = kChecksumAlgorithms[i];
      auto button = std::make_unique<Button>();
      button->setText(checksum::name(algorithm));
      button->setOnClick([this, algorithm] {
        m_dialog->algorithm = algorithm;
        resetResult();
        updateControls();
      });
      dialog.algorithms[i] = button.get();
      algorithms->addChild(std::move(button));
    }
    root.addChild(std::move(algorithms));
    dialog.warning = addLabel(" ", true);
    if (dialog.verify) {
      addLabel("Expected hash");
      dialog.expectedInput = addInput("Paste expected hash", false);
      dialog.expectedInput->setOnSubmit([this](const std::string&) { start(); });
      dialog.expectedInput->setOnChange([this](const std::string& value) { setInput(value); });
      dialog.expectedInput->setOnKeyEvent([this](std::uint32_t sym, std::uint32_t modifiers) {
        if (((modifiers & KeyMod::Ctrl) && (sym == XKB_KEY_v || sym == XKB_KEY_V))
            || ((modifiers & KeyMod::Shift) && sym == XKB_KEY_Insert)) {
          pasteExpected();
          return true;
        }
        return false;
      });
      auto row = std::make_unique<Flex>();
      row->setAlign(FlexAlign::Center);
      row->setGap(8.0F);
      auto paste = std::make_unique<Button>();
      paste->setText("Paste Hash");
      paste->setVariant(ButtonVariant::Outline);
      paste->setOnClick([this] { pasteExpected(); });
      dialog.paste = paste.get();
      row->addChild(std::move(paste));
      auto browse = std::make_unique<Button>();
      browse->setText("Choose Manifest…");
      browse->setVariant(ButtonVariant::Outline);
      browse->setOnClick([this] { chooseManifest(); });
      dialog.browse = browse.get();
      row->addChild(std::move(browse));
      root.addChild(std::move(row));
      dialog.manifestLabel = addLabel(" ", true);
    }
    addLabel("Calculated hash");
    dialog.calculated = addInput("", true);
    auto progress = std::make_unique<ProgressBar>();
    progress->setSize(1.0F, 5.0F);
    progress->setProgress(0.0F);
    dialog.progress = progress.get();
    root.addChild(std::move(progress));
    auto statusRow = std::make_unique<Flex>();
    statusRow->setDirection(FlexDirection::Horizontal);
    statusRow->setAlign(FlexAlign::Center);
    statusRow->setGap(Style::spaceSm);
    statusRow->setPadding(Style::spaceSm, 0.0F, 0.0F, 0.0F);
    statusRow->setMinHeight(34.0F);
    auto statusIcon = std::make_unique<Glyph>();
    statusIcon->setGlyphSize(20.0F);
    statusIcon->setVisible(false);
    dialog.statusIcon = statusIcon.get();
    statusRow->addChild(std::move(statusIcon));
    auto status = std::make_unique<Label>();
    status->setText(" ");
    status->setMaxWidth(std::max(1.0F, width - 76.0F));
    status->setMaxLines(2);
    status->setFlexGrow(1.0F);
    dialog.status = status.get();
    statusRow->addChild(std::move(status));
    root.addChild(std::move(statusRow));
  }

  void ChecksumDialogController::setStatus(DialogState::StatusTone tone, std::string_view text) {
    auto& dialog = *m_dialog;
    dialog.statusTone = tone;
    constexpr float kResultFontSize = 17.0F;
    const auto statusColor = [tone]() -> ColorSpec {
      switch (tone) {
      case DialogState::StatusTone::Success:
        return fixedColorSpec(isResolvedLightTheme() ? hex("#16803C") : hex("#4ADE80"));
      case DialogState::StatusTone::Mismatch:
        return fixedColorSpec(isResolvedLightTheme() ? hex("#BE123C") : hex("#E53955"));
      case DialogState::StatusTone::Error:
        return colorSpecFromRole(ColorRole::Error);
      case DialogState::StatusTone::Neutral:
        return colorSpecFromRole(ColorRole::OnSurface);
      }
      return colorSpecFromRole(ColorRole::OnSurface);
    }();
    const bool prominent = tone == DialogState::StatusTone::Success || tone == DialogState::StatusTone::Mismatch;
    if (dialog.statusIcon) {
      if (prominent) {
        dialog.statusIcon->setGlyph(tone == DialogState::StatusTone::Success ? "circle-check" : "circle-x");
        dialog.statusIcon->setColor(statusColor);
        dialog.statusIcon->setVisible(true);
      } else {
        dialog.statusIcon->setVisible(false);
      }
    }
    dialog.status->setFontSize(prominent ? kResultFontSize : Style::fontSizeBody);
    dialog.status->setFontWeight(prominent ? FontWeight::SemiBold : FontWeight::Normal);
    dialog.status->setColor(statusColor);
    dialog.status->setText(text);
  }

  void ChecksumDialogController::chooseManifest() {
    ++m_inputRevision;
    auto& dialog = *m_dialog;
    FileDialogOptions options{
        .mode = FileDialogMode::Open, .startDirectory = dialog.target.parent_path(), .title = "Choose Checksum Manifest"
    };
    const std::weak_ptr<bool> alive = m_alive;
    if (!FileDialog::open(std::move(options), [this, alive](auto path) {
          const auto locked = alive.lock();
          if (!locked || !*locked || !m_dialog || m_dialogHost.closeQueued() || !path)
            return;
          auto& state = *m_dialog;
          state.expectedInput->setValue("");
          state.manifest = std::move(*path);
          const std::unique_ptr<char, decltype(&g_free)> filename(
              g_filename_display_basename(state.manifest.c_str()), g_free
          );
          state.manifestLabel->setText(filename.get());
          if (const auto detected = checksum::fromManifestName(state.manifest.filename().string()))
            state.algorithm = *detected;
          resetResult();
          updateControls();
        })) {
      setStatus(DialogState::StatusTone::Error, "Could not open the manifest picker.");
      m_dialogHost.requestLayout();
    }
  }

  void ChecksumDialogController::pasteExpected() {
    const auto revision = ++m_inputRevision;
    m_transfer.readTextSelection([this, alive = std::weak_ptr<bool>(m_alive), revision](std::optional<std::string> text) {
      const auto locked = alive.lock();
      if (!locked || !*locked || !m_dialog || m_dialogHost.closeQueued() || revision != m_inputRevision)
        return;
      auto& dialog = *m_dialog;
      if (!text
          || text->size() > 4096
          || text->find('\0') != std::string::npos
          || !g_utf8_validate(text->data(), static_cast<gssize>(text->size()), nullptr)) {
        setStatus(DialogState::StatusTone::Error, "The clipboard does not contain a usable checksum.");
        m_dialogHost.requestLayout();
        return;
      }
      dialog.expectedInput->setValue(checksum::normalizeHash(*text));
      setInput(dialog.expectedInput->value());
    });
  }

  void ChecksumDialogController::setInput(std::string_view value) {
    ++m_inputRevision;
    auto& dialog = *m_dialog;
    dialog.manifest.clear();
    dialog.manifestLabel->setText(" ");
    const auto hash = checksum::normalizeHash(value);
    if (const auto detected = checksum::fromHash(hash); detected && checksum::validHash(hash, *detected))
      dialog.algorithm = *detected;
    resetResult();
    updateControls();
  }

  void ChecksumDialogController::resetResult() {
    auto& dialog = *m_dialog;
    dialog.calculated->setValue("");
    dialog.copyFeedbackTimer.stop();
    dialog.copy->setText("Copy");
    if (dialog.expectedInput) {
      if (!dialog.manifest.empty())
        dialog.expectedInput->setValue("");
      dialog.expectedInput->setInvalid(false);
    }
    dialog.progress->setProgress(0.0F);
    setStatus(DialogState::StatusTone::Neutral, " ");
  }

  void ChecksumDialogController::updateControls() {
    auto& dialog = *m_dialog;
    const bool busy = dialog.job != nullptr;
    for (std::size_t i = 0; i < kChecksumAlgorithms.size(); ++i) {
      dialog.algorithms[i]->setVariant(
          dialog.algorithm == kChecksumAlgorithms[i] ? ButtonVariant::Primary : ButtonVariant::Outline
      );
      dialog.algorithms[i]->setEnabled(!busy);
    }
    dialog.warning->setText(
        dialog.algorithm == checksum::Algorithm::Md5 ? "MD5 is not secure against deliberate tampering." : " "
    );
    if (dialog.expectedInput)
      dialog.expectedInput->setEnabled(!busy);
    if (dialog.browse)
      dialog.browse->setEnabled(!busy);
    if (dialog.paste)
      dialog.paste->setEnabled(!busy);
    dialog.run->setEnabled(!busy);
    dialog.copy->setEnabled(!busy && !dialog.calculated->value().empty());
    dialog.close->setText(busy ? "Cancel" : "Close");
    dialog.close->setEnabled(!busy || !g_cancellable_is_cancelled(dialog.job->cancel.get()));
    m_dialogHost.requestLayout();
  }

  void ChecksumDialogController::start() {
    auto& dialog = *m_dialog;
    const bool pickerOpen = m_callbacks.isPickerOpen && m_callbacks.isPickerOpen();
    if (dialog.job || m_dialogHost.closeQueued() || pickerOpen)
      return;
    ++m_inputRevision;
    resetResult();
    auto job = std::make_shared<ChecksumJob>();
    job->target = dialog.target;
    job->manifest = dialog.manifest;
    job->algorithm = dialog.algorithm;
    if (dialog.verify && job->manifest.empty()) {
      job->expected = checksum::normalizeHash(dialog.expectedInput->value());
      if (!checksum::validHash(job->expected, job->algorithm)) {
        dialog.expectedInput->setInvalid(true);
        m_dialogHost.setFocus(dialog.expectedInput->inputArea());
        setStatus(
            DialogState::StatusTone::Error,
            std::string("Enter a valid ") + checksum::name(job->algorithm) + " hexadecimal hash or choose a manifest."
        );
        updateControls();
        return;
      }
    }

    struct Request {
      ChecksumDialogController* owner;
      std::weak_ptr<bool> alive;
      std::shared_ptr<ChecksumJob> job;
    };
    if (dialog.protectedChecksum) {
      job->protectedOperation = dialog.protectedChecksum(job->algorithm);
      if (!job->protectedOperation) {
        setStatus(DialogState::StatusTone::Error, "The originating Administrator session ended.");
        updateControls();
        return;
      }
    }
    dialog.job = job;
    setStatus(DialogState::StatusTone::Neutral, job->manifest.empty() ? "Calculating… 0%" : "Reading manifest…");
    updateControls();
    dialog.progressTimer.startRepeating(std::chrono::milliseconds(100), [this, job] {
      if (!m_dialog || m_dialogHost.closeQueued() || g_cancellable_is_cancelled(job->cancel.get()))
        return;
      const int percent = job->percent.load(std::memory_order_relaxed);
      if (percent >= 0) {
        auto& state = *m_dialog;
        state.progress->setProgress(static_cast<float>(percent) / 100.0F);
        setStatus(DialogState::StatusTone::Neutral, "Calculating… " + std::to_string(percent) + "%");
        m_dialogHost.requestLayout();
      }
    });
    GTask* task = g_task_new(
        nullptr, job->cancel.get(),
        [](GObject*, GAsyncResult* result, gpointer) {
          auto* completed = G_TASK(result);
          const auto& request = *static_cast<const Request*>(g_task_get_task_data(completed));
          GError* rawError = nullptr;
          const bool ok = g_task_propagate_boolean(completed, &rawError);
          const std::unique_ptr<GError, decltype(&g_error_free)> error(rawError, g_error_free);
          auto* owner = request.owner;
          const auto locked = request.alive.lock();
          if (!locked
              || !*locked
              || !owner->m_dialog
              || owner->m_dialogHost.closeQueued()
              || owner->m_dialog->job != request.job)
            return;
          auto& state = *owner->m_dialog;
          state.progressTimer.stop();
          state.job.reset();
          if (ok) {
            state.progress->setProgress(1.0F);
            state.calculated->setValue(request.job->result.hash);
            if (state.verify) {
              if (!request.job->manifest.empty())
                state.expectedInput->setValue(request.job->expected);
              const bool match =
                  checksum::compare(request.job->result.hash, request.job->expected) == checksum::Comparison::Match;
              owner->setStatus(
                  match ? DialogState::StatusTone::Success : DialogState::StatusTone::Mismatch,
                  match ? "Checksums match" : "Checksums do not match"
              );
            } else {
              owner->setStatus(DialogState::StatusTone::Success, "Checksum calculated.");
            }
          } else if (g_error_matches(error.get(), G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            state.progress->setProgress(0.0F);
            owner->setStatus(DialogState::StatusTone::Neutral, "Cancelled.");
          } else {
            state.progress->setProgress(0.0F);
            owner->setStatus(
                DialogState::StatusTone::Error, error ? error->message : "Checksum operation failed."
            );
          }
          owner->updateControls();
        },
        nullptr
    );
    g_task_set_task_data(task, new Request{this, m_alive, job}, [](gpointer data) {
      delete static_cast<Request*>(data);
    });
    g_task_run_in_thread(task, [](GTask* worker, gpointer, gpointer data, GCancellable* cancel) {
      auto& work = *static_cast<const Request*>(data)->job;
      GError* error = nullptr;
      if (!work.manifest.empty()
          && !checksum::loadExpected(work.manifest, work.target, work.algorithm, cancel, work.expected, &error)) {
        if (work.protectedOperation && work.protectedOperation->cancel)
          (void)work.protectedOperation->cancel();
        g_task_return_error(worker, error);
        return;
      }
      const auto progress = [&work](std::uint64_t bytes, std::uint64_t total) {
        const int percent =
            total == 0 ? 100 : static_cast<int>(100.0 * (static_cast<double>(bytes) / static_cast<double>(total)));
        work.percent.store(percent, std::memory_order_relaxed);
      };
      if (work.protectedOperation) {
        while (true) {
          for (auto& frame : work.protectedOperation->waitForFrames()) {
            if (frame.kind == AdminServerFrameKind::OpProgress) {
              progress(frame.completedBytes, frame.totalBytes);
            } else if (frame.kind == AdminServerFrameKind::OpResult) {
              if (frame.status == AdminStatusCode::Ok && checksum::validHash(frame.customDescription, work.algorithm)) {
                work.result.hash = std::move(frame.customDescription);
                g_task_return_boolean(worker, true);
              } else {
                g_task_return_new_error(
                    worker, G_IO_ERROR, frame.status == AdminStatusCode::Cancelled ? G_IO_ERROR_CANCELLED : G_IO_ERROR_FAILED,
                    "%s", frame.detail.empty() ? "Protected checksum failed." : frame.detail.c_str()
                );
              }
              return;
            }
          }
        }
      }
      if (checksum::calculate(work.target, work.algorithm, cancel, work.result, &error, progress))
        g_task_return_boolean(worker, true);
      else
        g_task_return_error(worker, error);
    });
    g_object_unref(task);
  }

  void ChecksumDialogController::stop() {
    auto& dialog = *m_dialog;
    g_cancellable_cancel(dialog.job->cancel.get());
    if (dialog.job->protectedOperation && dialog.job->protectedOperation->cancel)
      (void)dialog.job->protectedOperation->cancel();
    setStatus(DialogState::StatusTone::Neutral, "Cancelling…");
    updateControls();
  }

  void ChecksumDialogController::dismiss() {
    if (!m_dialog)
      return;
    ++m_inputRevision;
    if (m_dialog->job) {
      g_cancellable_cancel(m_dialog->job->cancel.get());
      if (m_dialog->job->protectedOperation && m_dialog->job->protectedOperation->cancel)
        (void)m_dialog->job->protectedOperation->cancel();
    }
    m_dialog.reset();
    if (m_callbacks.closePicker)
      m_callbacks.closePicker();
  }

} // namespace filemanager
