#include "filemanager/dialogs/trash_fallback_dialog.h"

#include "core/files/display_text.h"
#include "filemanager/dialogs/modal_dialog_host.h"
#include "filemanager/folder_size.h"
#include "filemanager/wastebin_fallback.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <atomic>
#include <filesystem>
#include <gio/gio.h>
#include <sys/stat.h>

namespace filemanager {

  struct TrashFallbackDialogController::SizeJob {
    std::vector<std::string> sources;
    std::shared_ptr<GCancellable> cancel{g_cancellable_new(), g_object_unref};
    std::uint64_t totalBytes = 0;
    std::uint64_t totalItems = 0;
    bool partial = false;
    std::optional<std::uint64_t> availableBytes;
  };

  TrashFallbackDialogController::TrashFallbackDialogController(ModalDialogHost& dialogHost)
      : m_dialogHost(dialogHost) {}

  TrashFallbackDialogController::~TrashFallbackDialogController() {
    *m_alive = false;
    dismiss();
  }

  bool TrashFallbackDialogController::calculationFinished() const noexcept { return m_state && m_state->calculated; }

  bool TrashFallbackDialogController::moveToHomeEnabled() const noexcept {
    return m_state && m_state->calculated && m_state->sufficientSpace;
  }

  void TrashFallbackDialogController::dismiss() {
    if (m_state && m_state->job)
      g_cancellable_cancel(m_state->job->cancel.get());
    m_state.reset();
  }

  void TrashFallbackDialogController::show(
      const FileOperationManager::TrashFallbackOffer& offer, std::function<void(std::vector<std::string>)> onMoveToHome,
      std::function<void(std::vector<std::string>)> onPermanentDelete
  ) {
    if (offer.sources.empty() || m_dialogHost.isOpen())
      return;

    m_state = std::make_unique<DialogState>();
    m_state->offer = offer;
    m_state->onMoveToHome = std::move(onMoveToHome);
    m_state->onPermanentDelete = std::move(onPermanentDelete);

    std::string headerMessage = offer.diagnostic;
    if (headerMessage.empty())
      headerMessage = "The mount Wastebin is unavailable for the selected items.";
    headerMessage += "\n\nMoving items to your Home Wastebin transfers them across filesystems.";

    ModalDialogHost::CustomContent custom{
        .buildFields = [this](Flex& root, float width) { buildFields(root, width); },
        .bindChoiceButton =
            [this](int result, Button* button) {
              if (!m_state || button == nullptr)
                return;
              if (result == kMoveToHomeResponse) {
                m_state->moveButton = button;
                button->setEnabled(m_state->calculated && m_state->sufficientSpace);
              }
            },
        .afterBuild = [this] { updateLabels(); },
        .onThemeUpdated = [this] { updateLabels(); },
        .interceptResponse = [this](int response) { return interceptResponse(response); },
        .onBeforeClose =
            [this] {
              if (m_state && m_state->job)
                g_cancellable_cancel(m_state->job->cancel.get());
            },
        .onDismiss = [this] { dismiss(); },
        .useMeasuredMinHeight = true,
        .preferredWidth = 480,
    };

    startCalculation();

    m_dialogHost.showDialog(
        "Wastebin Unavailable", std::move(headerMessage),
        {{"Cancel", kCancelResponse},
         {"Move to Home Wastebin", kMoveToHomeResponse},
         {"Permanently Delete…", kPermanentDeleteResponse}},
        [this](int response) {
          if (!m_state)
            return;
          auto sources = std::move(m_state->offer.sources);
          auto moveCb = std::move(m_state->onMoveToHome);
          auto deleteCb = std::move(m_state->onPermanentDelete);
          dismiss();
          if (response == kMoveToHomeResponse && moveCb)
            moveCb(std::move(sources));
          else if (response == kPermanentDeleteResponse && deleteCb)
            deleteCb(std::move(sources));
        },
        nullptr, false, false, std::move(custom)
    );
  }

  bool TrashFallbackDialogController::interceptResponse(int response) {
    if (!m_state)
      return false;
    if (response == kMoveToHomeResponse && (!m_state->calculated || !m_state->sufficientSpace))
      return true;
    return false;
  }

  void TrashFallbackDialogController::buildFields(Flex& root, float width) {
    if (!m_state)
      return;
    const float cardInnerWidth = std::max(180.0F, width - 68.0F);
    auto card = std::make_unique<Flex>();
    card->setDirection(FlexDirection::Vertical);
    card->setAlign(FlexAlign::Stretch);
    card->setGap(Style::spaceXs);
    card->setPadding(Style::spaceSm, Style::spaceMd);
    card->setFill(colorSpecFromRole(ColorRole::SurfaceContainerHigh, 0.78F));
    card->setBorder(colorSpecFromRole(ColorRole::OutlineVariant, 0.72F), Style::borderWidth);
    m_dialogHost.registerCard(card.get());

    auto sizeLabel = std::make_unique<Label>();
    sizeLabel->setFontSize(Style::fontSizeCaption);
    sizeLabel->setColor(colorSpecFromRole(ColorRole::OnSurface));
    sizeLabel->setMaxWidth(cardInnerWidth);
    sizeLabel->setMaxLines(2);
    m_state->sizeLabel = sizeLabel.get();
    card->addChild(std::move(sizeLabel));

    auto freeLabel = std::make_unique<Label>();
    freeLabel->setFontSize(Style::fontSizeCaption);
    freeLabel->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    freeLabel->setMaxWidth(cardInnerWidth);
    freeLabel->setMaxLines(2);
    m_state->freeSpaceLabel = freeLabel.get();
    card->addChild(std::move(freeLabel));

    auto warnLabel = std::make_unique<Label>();
    warnLabel->setFontSize(Style::fontSizeCaption);
    warnLabel->setColor(colorSpecFromRole(ColorRole::Error));
    warnLabel->setMaxWidth(cardInnerWidth);
    warnLabel->setMaxLines(3);
    warnLabel->setVisible(false);
    warnLabel->setParticipatesInLayout(false);
    m_state->warningLabel = warnLabel.get();
    card->addChild(std::move(warnLabel));

    root.addChild(std::move(card));
  }

  void TrashFallbackDialogController::updateLabels() {
    if (!m_state)
      return;
    if (m_state->sizeLabel != nullptr) {
      if (!m_state->calculated) {
        m_state->sizeLabel->setText("Estimated transfer size: Calculating…");
      } else {
        std::string sizeText = "Estimated transfer size: ";
        if (m_state->partialSize)
          sizeText += "≥ ";
        sizeText += wastebin_fallback::formatByteSize(m_state->estimatedBytes);
        sizeText +=
            " (" + std::to_string(m_state->estimatedItems) + (m_state->estimatedItems == 1 ? " item" : " items") + ")";
        if (m_state->partialSize)
          sizeText += " (some subdirectories could not be inspected)";
        m_state->sizeLabel->setText(std::move(sizeText));
      }
      m_state->sizeLabel->setColor(colorSpecFromRole(ColorRole::OnSurface));
    }

    if (m_state->freeSpaceLabel != nullptr) {
      if (!m_state->calculated) {
        m_state->freeSpaceLabel->setText("Home Wastebin free space: Checking…");
        m_state->freeSpaceLabel->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      } else if (m_state->availableBytes.has_value()) {
        m_state->freeSpaceLabel->setText(
            "Home Wastebin free space: " + wastebin_fallback::formatByteSize(*m_state->availableBytes) + " available"
        );
        m_state->freeSpaceLabel->setColor(
            colorSpecFromRole(m_state->sufficientSpace ? ColorRole::OnSurfaceVariant : ColorRole::Error)
        );
      } else {
        m_state->freeSpaceLabel->setText("Home Wastebin free space: Unavailable");
        m_state->freeSpaceLabel->setColor(colorSpecFromRole(ColorRole::Error));
      }
    }

    if (m_state->warningLabel != nullptr) {
      const bool showWarning = m_state->calculated && !m_state->sufficientSpace;
      m_state->warningLabel->setVisible(showWarning);
      m_state->warningLabel->setParticipatesInLayout(showWarning);
      if (showWarning) {
        m_state->warningLabel->setText(
            m_state->availableBytes.has_value()
                ? "Insufficient free space in Home Wastebin to transfer the selected items."
                : "Could not verify available free space in Home Wastebin."
        );
      }
      m_state->warningLabel->setColor(colorSpecFromRole(ColorRole::Error));
    }

    if (m_state->moveButton != nullptr)
      m_state->moveButton->setEnabled(m_state->calculated && m_state->sufficientSpace);
  }

  void TrashFallbackDialogController::startCalculation() {
    if (!m_state)
      return;
    auto job = std::make_shared<SizeJob>();
    job->sources = m_state->offer.sources;
    m_state->job = job;

    struct TaskContext {
      TrashFallbackDialogController* controller;
      std::weak_ptr<bool> alive;
      std::shared_ptr<SizeJob> job;
    };

    GTask* task = g_task_new(
        nullptr, job->cancel.get(),
        [](GObject*, GAsyncResult* res, gpointer userData) {
          std::unique_ptr<TaskContext> ctx(static_cast<TaskContext*>(userData));
          GError* error = nullptr;
          g_task_propagate_boolean(G_TASK(res), &error);
          g_clear_error(&error);
          const auto locked = ctx->alive.lock();
          if (!locked || !*locked)
            return;
          auto* self = ctx->controller;
          if (!self->m_state || self->m_state->job != ctx->job)
            return;
          if (g_cancellable_is_cancelled(ctx->job->cancel.get()))
            return;
          self->m_state->calculated = true;
          self->m_state->estimatedBytes = ctx->job->totalBytes;
          self->m_state->estimatedItems = std::max<std::uint64_t>(1, ctx->job->totalItems);
          self->m_state->partialSize = ctx->job->partial;
          self->m_state->availableBytes = ctx->job->availableBytes;
          self->m_state->sufficientSpace =
              ctx->job->availableBytes.has_value() && *ctx->job->availableBytes >= ctx->job->totalBytes;
          self->updateLabels();
          self->m_dialogHost.requestLayout();
        },
        new TaskContext{this, m_alive, job}
    );
    g_task_set_task_data(task, new std::shared_ptr<SizeJob>(job), [](gpointer data) {
      delete static_cast<std::shared_ptr<SizeJob>*>(data);
    });
    g_task_run_in_thread(task, [](GTask* workerTask, gpointer, gpointer taskData, GCancellable* cancellable) {
      auto& activeJob = *static_cast<std::shared_ptr<SizeJob>*>(taskData);
      for (const auto& sourcePath : activeJob->sources) {
        if (g_cancellable_is_cancelled(cancellable))
          break;
        struct stat st{};
        if (lstat(sourcePath.c_str(), &st) != 0) {
          activeJob->partial = true;
          continue;
        }
        if (S_ISDIR(st.st_mode)) {
          const auto folderRes = calculateFolderSize(
              FolderSizeRequest{
                  .path = sourcePath,
                  .device = static_cast<std::uint64_t>(st.st_dev),
                  .inode = static_cast<std::uint64_t>(st.st_ino),
              },
              [cancellable] { return g_cancellable_is_cancelled(cancellable) != FALSE; }
          );
          activeJob->totalBytes += folderRes.bytes;
          activeJob->totalItems += 1 + folderRes.files + folderRes.folders + folderRes.links + folderRes.other;
          activeJob->partial |= folderRes.partial();
        } else if (S_ISLNK(st.st_mode)) {
          activeJob->totalBytes += static_cast<std::uint64_t>(std::max<off_t>(0, st.st_size));
          activeJob->totalItems += 1;
        } else {
          activeJob->totalBytes += static_cast<std::uint64_t>(std::max<off_t>(0, st.st_size));
          activeJob->totalItems += 1;
        }
      }
      activeJob->availableBytes = wastebin_fallback::queryAvailableBytes(wastebin_fallback::homeWastebinRoot());
      g_task_return_boolean(workerTask, TRUE);
    });
    g_object_unref(task);
  }

} // namespace filemanager
