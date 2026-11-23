#include "filemanager/dialogs/modal_dialog_host.h"

#include "config/config_service.h"
#include "core/files/display_text.h"
#include "core/ui_phase.h"
#include "cursor-shape-v1-client-protocol.h"
#include "filemanager/file_interaction.h"
#include "filemanager/file_manager_settings.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "ui/controls/button.h"
#include "ui/controls/checkbox.h"
#include "ui/controls/flex.h"
#include "ui/controls/image.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/separator.h"
#include "ui/dialogs/dialog_background_artwork.h"
#include "ui/dialogs/file_icon.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <future>
#include <gio/gio.h>
#include <linux/input-event-codes.h>
#include <xdg-shell-client-protocol.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace filemanager {

  namespace {
    // SVG alpha masks use the existing Image tint and scale handling.
    constexpr char kDeleteWarning[] = R"SVG(data:image/svg+xml,
<svg xmlns="http://www.w3.org/2000/svg" width="72" height="64" viewBox="0 0 72 64">
  <path fill="none" stroke="white" stroke-width="3.6" stroke-linejoin="bevel"
        d="M34.3 5 H33 L13.3 41.4 M11.5 44.5 L4.9 56.6 7 59 H32
           M35.5 59 H38 M41.5 59 H65 L67.1 56.6 54.8 34
           M53.2 31 L51.8 28.4 M50.2 25.4 L48.8 22.8 M47.2 19.8 L39 5 H37.7"/>
  <path fill="white" d="M32 20 H40 L38 42 H34 Z M33 46 H39 V52 H33 Z"/>
</svg>)SVG";

    void applyNytheraBevel(Flex& panel, float radius) {
      panel.setRadius(radius);
      panel.setCornerShapes(
          CornerShapes{
              .tl = CornerShape::Chamfered,
              .tr = CornerShape::Chamfered,
              .br = CornerShape::Chamfered,
              .bl = CornerShape::Chamfered,
          }
      );
    }
  } // namespace

  ModalDialogHost::ModalDialogHost(
      WaylandConnection& wayland, RenderContext& renderContext, ConfigService& configService,
      FileManagerSettings& settings, Callbacks callbacks
  )
      : m_wayland(wayland),
        m_renderContext(renderContext),
        m_configService(configService),
        m_settings(settings),
        m_callbacks(std::move(callbacks)) {}

  ModalDialogHost::~ModalDialogHost() {
    *m_alive = false;
    forceCloseImmediate();
  }

  std::string ModalDialogHost::sanitizeConflictDisplayText(std::string_view text) {
    return sanitizeFilenameForDisplay(text);
  }

  void ModalDialogHost::forceCloseImmediate() {
    if (m_customContent && m_customContent->onDismiss)
      m_customContent->onDismiss();
    m_customContent.reset();
    m_dispatcher.setSceneRoot(nullptr);
    if (m_surface)
      m_surface->setSceneRoot(nullptr);
    m_cards.clear();
    m_deleteWarning = nullptr;
    m_topAccent = nullptr;
    m_cornerAccent = nullptr;
    m_conflict.reset();
    m_conflictRequest.reset();
    m_root.reset();
    m_surface.reset();
    m_alternativeNameInput = nullptr;
    m_closeQueued = false;
  }

  void ModalDialogHost::requestLayout() {
    if (m_surface)
      m_surface->requestLayout();
  }

  void ModalDialogHost::setFocus(InputArea* area) {
    m_dispatcher.setFocus(area);
  }

  void ModalDialogHost::registerCard(Flex* card) {
    if (card != nullptr)
      m_cards.push_back(card);
  }

  void ModalDialogHost::textPrompt(
      std::string title, std::string message, bool password, std::function<void(std::optional<std::string>)> response
  ) {
    if (m_surface) {
      response(std::nullopt);
      return;
    }
    m_textPrompt = true;
    m_passwordPrompt = password;
    showDialog(
        std::move(title), std::move(message), {{"Cancel", 0}, {"Continue", 1}},
        [this, response = std::move(response)](int choice) mutable {
          auto value = m_alternativeName;
          m_alternativeName.clear();
          m_textPrompt = false;
          m_passwordPrompt = false;
          if (choice == 1)
            response(std::move(value));
          else
            response(std::nullopt);
        },
        nullptr, false, true
    );
  }

  void ModalDialogHost::showSymlinkDialog(
      std::function<std::optional<SymlinkError>(const std::string&, const std::string&)> onSubmit
  ) {
    if (m_surface)
      return;
    m_symlinkDialogActive = true;
    m_symlinkSubmitCallback = std::move(onSubmit);
    showDialog(
        "Create Symlink", "Enter a name and target path. Relative targets are resolved from this folder.",
        {{"Cancel", 0}, {"Create Symlink", 1}},
        [this](int) {
          m_symlinkDialogActive = false;
          m_symlinkSubmitCallback = nullptr;
          m_symlinkName = nullptr;
          m_symlinkTarget = nullptr;
          m_symlinkError = nullptr;
        }
    );
  }

  bool ModalDialogHost::submitSymlinkDialog() {
    const auto fail = [this](std::string_view message, Input* field) {
      m_symlinkError->setText(message);
      if (field) {
        field->setInvalid(true);
        m_dispatcher.setFocus(field->inputArea());
      }
      for (auto* parent = m_symlinkError->parent(); parent; parent = parent->parent()) {
        if (auto* scroll = dynamic_cast<ScrollView*>(parent)) {
          scroll->requestScrollIntoView(m_symlinkError);
          break;
        }
      }
      if (m_surface)
        m_surface->requestLayout();
      return false;
    };
    if (!m_symlinkSubmitCallback)
      return true;
    if (const auto error = m_symlinkSubmitCallback(m_symlinkName->value(), m_symlinkTarget->value())) {
      Input* field = error->field == SymlinkField::Name ? m_symlinkName
          : error->field == SymlinkField::Target        ? m_symlinkTarget
                                                        : nullptr;
      return fail(error->message, field);
    }
    return true;
  }

  FileOperationManager::ConflictResolver ModalDialogHost::makeConflictResolver(ConflictPathAccess pathAccess) {
    return [this, pathAccess, alive = std::weak_ptr<bool>(m_alive), conflictMutex = m_conflictMutex](
               const std::string& source, const std::string& destination, bool hasRemaining,
               const file_operation_engine::ConflictMetadata& metadata, GCancellable* cancellable
           ) {
      if (const auto initialAlive = alive.lock();
          !initialAlive || !*initialAlive || g_cancellable_is_cancelled(cancellable))
        return FileOperationManager::ConflictDecision{};
      std::unique_lock lock(*conflictMutex, std::defer_lock);
      while (!lock.try_lock_for(std::chrono::milliseconds(5))) {
        if (g_cancellable_is_cancelled(cancellable))
          return FileOperationManager::ConflictDecision{};
      }
      if (const auto lockedAlive = alive.lock();
          !lockedAlive || !*lockedAlive || g_cancellable_is_cancelled(cancellable))
        return FileOperationManager::ConflictDecision{};
      auto promise = std::make_shared<std::promise<std::pair<int, std::string>>>();
      auto answer = promise->get_future();
      const auto describe = [](const file_operation_engine::ConflictItemMetadata& item) {
        if (!item.inspected)
          return std::string{};
        const bool directory = item.type == G_FILE_TYPE_DIRECTORY;
        g_autofree gchar* typeName =
            item.contentType.empty() ? nullptr : g_content_type_get_description(item.contentType.c_str());
        std::string details = directory ? "Folder" : typeName == nullptr ? "File" : typeName;
        if (!directory && item.size) {
          g_autofree gchar* size = g_format_size(static_cast<guint64>(*item.size));
          details += " · ";
          details += size;
        }
        if (item.modified) {
          GDateTime* date = g_date_time_new_from_unix_local(static_cast<gint64>(*item.modified));
          if (date != nullptr) {
            g_autofree gchar* formatted = g_date_time_format(date, "%e %b %Y, %H:%M");
            details += " · Modified ";
            details += formatted;
            g_date_time_unref(date);
          }
        }
        return details;
      };
      const auto sourcePath = std::filesystem::path(source);
      const auto destinationPath = std::filesystem::path(destination);
      const auto& incoming = metadata.incoming;
      const auto& existing = metadata.existing;
      const bool differentTypes = incoming.type == G_FILE_TYPE_UNKNOWN
          || existing.type == G_FILE_TYPE_UNKNOWN
          || incoming.type != existing.type;
      const bool directoryConflict = incoming.type == G_FILE_TYPE_DIRECTORY && existing.type == G_FILE_TYPE_DIRECTORY;
      std::string comparisonSummary;
      if (differentTypes)
        comparisonSummary = "The item types differ";
      else if (!directoryConflict) {
        if (incoming.size && existing.size && *incoming.size != *existing.size)
          comparisonSummary = *incoming.size > *existing.size ? "Incoming file is larger" : "Incoming file is smaller";
        if (incoming.modified && existing.modified && *incoming.modified != *existing.modified) {
          if (!comparisonSummary.empty())
            comparisonSummary += " · ";
          comparisonSummary +=
              *incoming.modified > *existing.modified ? "Incoming item is newer" : "Incoming item is older";
        }
      }
      const ConflictDialogKind kind = metadata.sameItem       ? ConflictDialogKind::SelfConflict
          : differentTypes                                    ? ConflictDialogKind::TypeMismatch
          : directoryConflict && source.starts_with("trash:") ? ConflictDialogKind::RestoreDirectory
          : directoryConflict                                 ? ConflictDialogKind::Directory
                                                              : ConflictDialogKind::File;
      ConflictDialogData conflictData{
          .kind = kind,
          .incoming =
              ConflictItemInfo{
                  .name = sourcePath.filename().string(),
                  .metadata = describe(incoming),
                  .path = sourcePath,
                  .isDirectory = incoming.type == G_FILE_TYPE_DIRECTORY,
              },
          .existing =
              ConflictItemInfo{
                  .name = destinationPath.filename().string(),
                  .metadata = describe(existing),
                  .path = destinationPath,
                  .isDirectory = existing.type == G_FILE_TYPE_DIRECTORY,
              },
          .comparisonSummary = std::move(comparisonSummary),
          .pathAccess = pathAccess,
      };
      struct ConflictDialogRequest {
        ModalDialogHost* self;
        std::weak_ptr<bool> alive;
        std::shared_ptr<std::promise<std::pair<int, std::string>>> promise;
        ConflictDialogData data;
        std::shared_ptr<GCancellable> cancellable;
        bool hasRemaining;
      };
      g_idle_add(
          [](gpointer data) -> gboolean {
            std::unique_ptr<ConflictDialogRequest> request(static_cast<ConflictDialogRequest*>(data));
            const auto idleAlive = request->alive.lock();
            if (!idleAlive || !*idleAlive || g_cancellable_is_cancelled(request->cancellable.get())) {
              try {
                request->promise->set_value({4, {}});
              } catch (...) {
              }
              return G_SOURCE_REMOVE;
            }
            auto* self = request->self;
            if (self->m_conflictDialogHook) {
              auto respond = [responseAlive = request->alive, promise = request->promise](int choice, std::string alt) {
                const auto dialogAlive = responseAlive.lock();
                if (!dialogAlive || !*dialogAlive) {
                  try {
                    promise->set_value({4, {}});
                  } catch (...) {
                  }
                  return;
                }
                try {
                  promise->set_value({choice, std::move(alt)});
                } catch (...) {
                }
              };
              if (self->m_conflictDialogHook(request->data, request->hasRemaining, std::move(respond)))
                return G_SOURCE_REMOVE;
            }
            const bool mainWindowOpen = self->m_callbacks.isMainWindowOpen ? self->m_callbacks.isMainWindowOpen() : true;
            if (!mainWindowOpen) {
              self->m_backgroundConflictPending = true;
              if (self->m_callbacks.syncProgressWindow)
                self->m_callbacks.syncProgressWindow();
            }
            const ConflictDialogKind conflictKind = request->data.kind;
            const bool selfConflict = conflictKind == ConflictDialogKind::SelfConflict;
            const bool incompatible = conflictKind == ConflictDialogKind::TypeMismatch;
            const bool restoreFolders = conflictKind == ConflictDialogKind::RestoreDirectory;
            const bool folders = restoreFolders || conflictKind == ConflictDialogKind::Directory;
            self->m_conflict = std::move(request->data);
            self->m_conflictRequest = request->promise;
            ToplevelSurface* parent = mainWindowOpen
                ? (self->m_callbacks.mainSurface ? self->m_callbacks.mainSurface() : nullptr)
                : (self->m_callbacks.progressSurface ? self->m_callbacks.progressSurface() : nullptr);
            self->showDialog(
                selfConflict       ? "Create a duplicate?"
                    : incompatible ? "Items have different types"
                    : folders      ? "Folder already exists"
                                   : "File already exists",
                {},
                selfConflict ? std::vector<std::pair<std::string, int>>{{"Keep Both", 2}, {"Cancel", 4}}
                    : restoreFolders
                    ? std::vector<std::pair<std::string, int>>{{"Keep Both", 2}, {"Skip", 3}, {"Cancel", 4}}
                    : folders
                    ? std::vector<std::pair<std::string, int>>{{"Merge", 5}, {"Keep Both", 2}, {"Skip", 3}, {"Cancel", 4}}
                    : incompatible
                    ? std::vector<std::pair<std::string, int>>{{"Keep Both", 2}, {"Skip", 3}, {"Cancel", 4}}
                    : std::vector<
                          std::pair<std::string, int>>{{"Replace", 1}, {"Keep Both", 2}, {"Skip", 3}, {"Cancel", 4}},
                [self, responseAlive = request->alive, promise = request->promise](int choice) {
                  const auto dialogAlive = responseAlive.lock();
                  if (!dialogAlive || !*dialogAlive) {
                    try {
                      promise->set_value({4, {}});
                    } catch (...) {
                    }
                    return;
                  }
                  try {
                    promise->set_value({choice, self->m_alternativeName});
                  } catch (...) {
                  }
                  if (self->m_conflictRequest == promise)
                    self->m_conflictRequest.reset();
                  self->m_backgroundConflictPending = false;
                  if (self->m_callbacks.syncProgressWindow)
                    self->m_callbacks.syncProgressWindow();
                },
                parent, request->hasRemaining, true
            );
            return G_SOURCE_REMOVE;
          },
          new ConflictDialogRequest{
              this, alive, promise, std::move(conflictData),
              cancellable ? std::shared_ptr<GCancellable>{G_CANCELLABLE(g_object_ref(cancellable)), g_object_unref}
                          : nullptr,
              hasRemaining
          }
      );
      while (answer.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready) {
        const auto waitAlive = alive.lock();
        if (!waitAlive || !*waitAlive || g_cancellable_is_cancelled(cancellable)) {
          struct CloseRequest {
            ModalDialogHost* self;
            std::weak_ptr<bool> alive;
            std::shared_ptr<void> conflict;
          };
          g_idle_add(
              [](gpointer data) -> gboolean {
                std::unique_ptr<CloseRequest> request(static_cast<CloseRequest*>(data));
                if (const auto closeAlive = request->alive.lock();
                    closeAlive && *closeAlive && request->self->m_conflictRequest == request->conflict)
                  request->self->closeDialog(-1);
                return G_SOURCE_REMOVE;
              },
              new CloseRequest{this, alive, promise}
          );
          return FileOperationManager::ConflictDecision{};
        }
      }
      const auto [choice, alternativeName] = answer.get();
      using Action = FileOperationManager::ConflictAction;
      switch (choice >= 10 ? choice - 10 : choice) {
      case 1:
        return FileOperationManager::ConflictDecision{Action::Replace, choice >= 11, {}};
      case 2:
        return FileOperationManager::ConflictDecision{Action::Rename, choice >= 12, alternativeName};
      case 3:
        return FileOperationManager::ConflictDecision{Action::Skip, choice >= 13, {}};
      case 5:
        return FileOperationManager::ConflictDecision{Action::Merge, choice >= 15, {}};
      default:
        return FileOperationManager::ConflictDecision{Action::Cancel, false};
      }
    };
  }

  void ModalDialogHost::showDialog(
      std::string title, std::string message, std::vector<std::pair<std::string, int>> choices,
      std::function<void(int)> response, ToplevelSurface* parent, bool applyToAllCheckbox, bool alternativeName,
      std::optional<CustomContent> customContent
  ) {
    if (!alternativeName || m_textPrompt)
      m_conflict.reset();
    if (m_surface || !m_wayland.isConnected()) {
      m_conflict.reset();
      if (response)
        response(-1);
      return;
    }
    m_customContent = std::move(customContent);
    if (parent == nullptr && m_callbacks.defaultParent)
      parent = m_callbacks.defaultParent();
    m_title = std::move(title);
    m_warningStyle = m_title == "Permanently delete?" || m_title == "Empty Wastebin?";
    m_resizable = true;
    m_resizeEdges.fill(nullptr);
    m_moveHandle = nullptr;
    m_titleRow = nullptr;
    m_spacer = nullptr;
    m_footer = nullptr;
    m_divider = nullptr;
    m_preferredWidth = 0;
    m_preferredHeight = 0;
    m_sizeStateKey = FileManagerSettings::dialogSizeStateKey(m_title, alternativeName && !m_textPrompt);
    if (m_resizable) {
      if (const auto saved = m_settings.loadDialogSize(m_sizeStateKey)) {
        m_preferredWidth = saved->first;
        m_preferredHeight = saved->second;
      }
    }
    m_message = std::move(message);
    m_choices = std::move(choices);
    m_applyToAllCheckbox = applyToAllCheckbox;
    m_applyToAllChecked = false;
    m_alternativeNameEnabled = alternativeName;
    m_alternativeName.clear();
    m_alternativeNameInput = nullptr;
    m_response = std::move(response);
    m_closeQueued = false;
    auto surface = std::make_unique<ToplevelSurface>(m_wayland);
    surface->setRenderContext(&m_renderContext);
    surface->setConfigureCallback([this](std::uint32_t width, std::uint32_t height) {
      if (m_surface) {
        if (m_resizable && width > 0 && height > 0) {
          m_preferredWidth = width;
          m_preferredHeight = height;
        }
        m_surface->requestLayout();
      }
    });
    surface->setClosedCallback([this] { closeDialog(-1); });
    surface->setPrepareFrameCallback([this](bool update, bool layout) { prepareFrame(update, layout); });
    const auto* outputInfo = m_wayland.outputs().empty() ? nullptr : &m_wayland.outputs().front();
    const auto maxWidth = static_cast<std::uint32_t>(
        outputInfo == nullptr ? 720 : std::min(720, outputInfo->logicalWidth > 0 ? outputInfo->logicalWidth - 40 : 720)
    );
    const auto maxHeight = static_cast<std::uint32_t>(
        outputInfo == nullptr ? 900 : std::min(900, outputInfo->logicalHeight > 0 ? outputInfo->logicalHeight - 80 : 900)
    );
    const std::uint32_t requestedMinWidth = (m_customContent && m_customContent->minWidth > 0)
        ? m_customContent->minWidth
        : (alternativeName && !m_textPrompt ? 520U : 420U);
    const auto minWidth = std::min(requestedMinWidth, maxWidth);
    const std::uint32_t defaultWidth = (m_customContent && m_customContent->preferredWidth > 0)
        ? m_customContent->preferredWidth
        : (alternativeName ? 720U : 520U);
    const auto width = m_resizable
        ? (m_preferredWidth > 0 ? std::max(m_preferredWidth, minWidth) : std::min(maxWidth, defaultWidth))
        : std::min(maxWidth, defaultWidth);
    buildDialog(static_cast<float>(width));
    ScaledRenderer measureRenderer(m_renderContext, outputInfo == nullptr ? 1.0F : outputInfo->configuredScale());
    LayoutConstraints constraints;
    constraints.setExactWidth(static_cast<float>(width));
    const auto measured = m_root->measure(measureRenderer, constraints);
    const auto measuredHeight = std::clamp(static_cast<std::uint32_t>(std::ceil(measured.height)), 180U, maxHeight);
    const std::uint32_t requestedMinHeight = (m_customContent && m_customContent->minHeight > 0)
        ? m_customContent->minHeight
        : (alternativeName && !m_textPrompt ? 260U : 180U);
    const auto minHeight = (m_customContent && m_customContent->useMeasuredMinHeight)
        ? measuredHeight
        : std::min(requestedMinHeight, maxHeight);
    const auto defaultHeight = (m_customContent && m_customContent->preferredHeight > 0)
        ? std::clamp(std::min(measuredHeight, m_customContent->preferredHeight), minHeight, maxHeight)
        : measuredHeight;
    const auto height =
        m_resizable && m_preferredHeight > 0 ? std::max(m_preferredHeight, minHeight) : defaultHeight;
    if (measured.height > static_cast<float>(height) && !(m_customContent && m_customContent->fillContent)) {
      UiPhaseScope layoutPhase(UiPhase::Layout);
      const auto focus = m_dispatcher.captureTabFocus();
      m_dispatcher.setSceneRoot(nullptr);
      auto footer = m_root->removeChild(m_footer);
      auto spacer = m_root->removeChild(m_spacer);
      m_spacer->setFlexGrow(0.0F);
      auto scroll = std::make_unique<ScrollView>();
      scroll->setFlexGrow(1.0F);
      scroll->content()->setDirection(FlexDirection::Vertical);
      scroll->content()->setAlign(FlexAlign::Stretch);
      scroll->content()->setGap(10.0F);
      if (m_warningStyle) {
        const auto& children = m_root->children();
        const auto content =
            std::ranges::find_if(children, [](const auto& child) { return child->participatesInLayout(); });
        const auto index = static_cast<std::size_t>(content - children.begin());
        scroll->content()->addChild(m_root->removeChild(content->get()));
        m_root->insertChildAt(index, std::move(scroll));
      } else {
        const auto& children = m_root->children();
        const auto content =
            std::ranges::find_if(children, [](const auto& child) { return child->participatesInLayout(); });
        const auto scrollIndex = static_cast<std::size_t>(content - children.begin());
        auto index = scrollIndex;
        while (index < m_root->children().size()) {
          auto* child = m_root->children()[index].get();
          if (child->participatesInLayout())
            scroll->content()->addChild(m_root->removeChild(child));
          else
            ++index;
        }
        m_root->insertChildAt(scrollIndex, std::move(scroll));
      }
      m_root->addChild(std::move(spacer));
      m_root->addChild(std::move(footer));
      m_dispatcher.setSceneRoot(m_root.get());
      m_dispatcher.restoreTabFocus(focus);
    }
    ToplevelSurfaceConfig config{
        .width = width,
        .height = height,
        .minWidth = minWidth,
        .minHeight = minHeight,
        .maxWidth = m_resizable ? 0U : width,
        .maxHeight = m_resizable ? 0U : height,
        .parent = parent == nullptr ? nullptr : parent->xdgToplevel(),
        .title = m_title,
        .appId = "dev.nythera.FileManager.Dialog"
    };
    wl_output* output = outputInfo == nullptr ? nullptr : outputInfo->output;
    if (surface->initialize(output, config)) {
      m_surface = std::move(surface);
      m_surface->setSceneRoot(m_root.get());
    } else {
      m_customContent.reset();
      auto failed = std::move(m_response);
      if (failed)
        failed(-1);
    }
  }

  void ModalDialogHost::closeDialog(int response) {
    if (m_closeQueued)
      return;
    if (m_customContent) {
      if (m_customContent->interceptResponse && m_customContent->interceptResponse(response))
        return;
      if (m_customContent->onBeforeClose)
        m_customContent->onBeforeClose();
    }
    if (m_symlinkDialogActive && response == 1 && !submitSymlinkDialog())
      return;
    if (!m_textPrompt && response == 2 && m_alternativeNameInput != nullptr) {
      const auto& name = m_alternativeNameInput->value();
      if (!name.empty()) {
        const bool validName = validFileName(name);
        const bool checkDestination = !m_conflict || m_conflict->pathAccess == ConflictPathAccess::Local;
        std::error_code error;
        const bool occupied =
            validName && checkDestination && std::filesystem::exists(m_destination.parent_path() / name, error);
        if (!validName || occupied || error) {
          m_alternativeNameInput->setInvalid(true);
          m_dispatcher.setFocus(m_alternativeNameInput->inputArea());
          requestLayout();
          return;
        }
      }
    }
    m_closeQueued = true;
    struct DialogCloseRequest {
      ModalDialogHost* self;
      std::weak_ptr<bool> alive;
    };
    g_idle_add(
        [](gpointer context) -> gboolean {
          std::unique_ptr<DialogCloseRequest> request(static_cast<DialogCloseRequest*>(context));
          const auto locked = request->alive.lock();
          if (!locked || !*locked)
            return G_SOURCE_REMOVE;
          auto* self = request->self;
          auto callback = std::move(self->m_response);
          self->m_alternativeName =
              self->m_alternativeNameInput == nullptr ? std::string{} : self->m_alternativeNameInput->value();
          const int result = self->m_resultPending
              + (self->m_applyToAllChecked
                         && self->m_resultPending >= 1
                         && (self->m_resultPending <= 3 || self->m_resultPending == 5)
                     ? 10
                     : 0);
          if (self->m_resizable
              && self->m_surface
              && self->m_surface->width() >= 360
              && self->m_surface->height() >= 180) {
            self->m_preferredWidth = self->m_surface->width();
            self->m_preferredHeight = self->m_surface->height();
            if (!self->m_settings.saveDialogSize(self->m_sizeStateKey, self->m_preferredWidth, self->m_preferredHeight)
                && self->m_callbacks.setStatus)
              self->m_callbacks.setStatus("Unable to save conflict dialog size");
          }
          // The manifest picker can be an xdg_popup child of this dialogue.
          if (self->m_customContent && self->m_customContent->onDismiss)
            self->m_customContent->onDismiss();
          self->m_customContent.reset();
          self->m_dispatcher.setSceneRoot(nullptr);
          if (self->m_surface)
            self->m_surface->setSceneRoot(nullptr);
          self->m_cards.clear();
          self->m_deleteWarning = nullptr;
          self->m_topAccent = nullptr;
          self->m_cornerAccent = nullptr;
          self->m_conflict.reset();
          self->m_root.reset();
          self->m_surface.reset();
          self->m_alternativeNameInput = nullptr;
          self->m_closeQueued = false;
          if (callback)
            callback(result);
          return G_SOURCE_REMOVE;
        },
        new DialogCloseRequest{this, m_alive}
    );
    m_resultPending = response;
  }

  void ModalDialogHost::handlePointer(const PointerEvent& event) {
    if (!m_surface || event.surface != m_surface->wlSurface())
      return;
    const float x = static_cast<float>(event.sx), y = static_cast<float>(event.sy);
    switch (event.type) {
    case PointerEvent::Type::Enter:
      m_dispatcher.pointerEnter(x, y, event.serial);
      break;
    case PointerEvent::Type::Leave:
      m_dispatcher.pointerLeave();
      break;
    case PointerEvent::Type::Motion:
      m_dispatcher.pointerMotion(x, y, event.serial);
      break;
    case PointerEvent::Type::Button:
      if (event.button == BTN_LEFT)
        m_dispatcher.pointerButton(x, y, event.button, event.pressed, event.serial, event.time, false);
      break;
    case PointerEvent::Type::Axis:
      m_dispatcher.pointerAxis(
          x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines,
          event.axisGestureSerial
      );
      break;
    }
  }

  void ModalDialogHost::handleKeyboard(const KeyboardEvent& event) {
    if (!event.pressed || event.preedit || !m_surface)
      return;
    if (event.sym == XKB_KEY_Escape) {
      closeDialog(-1);
      return;
    }
    if (m_customContent && m_customContent->onKeyEvent && m_customContent->onKeyEvent(event))
      return;
    if (m_symlinkDialogActive && (event.sym == XKB_KEY_Return || event.sym == XKB_KEY_KP_Enter)) {
      auto* focus = m_dispatcher.focusedArea();
      if (focus == m_symlinkTarget->inputArea()) {
        closeDialog(1);
        return;
      }
      if (focus == m_symlinkName->inputArea()) {
        if (validFileName(m_symlinkName->value()) && m_symlinkTarget->value().empty())
          m_dispatcher.setFocus(m_symlinkTarget->inputArea());
        else
          closeDialog(1);
        return;
      }
    }
    m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
  }

  void ModalDialogHost::buildDialog(float dialogWidth) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    m_cards.clear();
    m_deleteWarning = nullptr;
    m_topAccent = nullptr;
    m_cornerAccent = nullptr;
    m_root = std::make_unique<Flex>();
    m_root->setDirection(FlexDirection::Vertical);
    m_root->setAlign(FlexAlign::Stretch);
    m_root->setGap(10.0F);
    m_root->setPadding(16.0F);
    m_root->setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow));
    m_root->setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth);
    applyNytheraBevel(*m_root, Style::scaledRadiusMd(1.0F));
    if (m_warningStyle) {
      auto topAccent = std::make_unique<DialogBackgroundArtwork>("top-left-diagonals.svg", 126.0F, 78.27F);
      m_topAccent = topAccent.get();
      m_root->addChild(std::move(topAccent));
      auto cornerAccent = std::make_unique<DialogBackgroundArtwork>("bottom-right-contour.svg", 78.0F, 46.0F);
      m_cornerAccent = cornerAccent.get();
      m_root->addChild(std::move(cornerAccent));

      auto content = std::make_unique<Flex>();
      content->setDirection(FlexDirection::Horizontal);
      content->setAlign(FlexAlign::Center);
      content->setGap(18.0F);
      // Keep the warning at its natural height. Only the footer spacer grows.
      auto warning = std::make_unique<Image>();
      warning->setHitTestVisible(false);
      warning->setForegroundTint(colorSpecFromRole(ColorRole::Error));
      warning->setFrameSize(72.0F, 64.0F);
      m_deleteWarning = warning.get();
      content->addChild(std::move(warning));
      auto text = std::make_unique<Flex>();
      text->setDirection(FlexDirection::Vertical);
      text->setAlign(FlexAlign::Stretch);
      text->setGap(7.0F);
      text->setFlexGrow(1.0F);
      auto heading = std::make_unique<Label>();
      heading->setText(m_title);
      heading->setFontSize(Style::fontSizeTitle);
      heading->setFontWeight(FontWeight::SemiBold);
      heading->setColor(colorSpecFromRole(ColorRole::Primary));
      heading->setMaxLines(2);
      text->addChild(std::move(heading));
      auto message = std::make_unique<Label>();
      message->setText(m_message);
      message->setMaxWidth(std::max(1.0F, dialogWidth - 122.0F));
      message->setMaxLines(6);
      text->addChild(std::move(message));
      if (m_resizable) {
        auto moveHandle = std::make_unique<InputArea>();
        moveHandle->setParticipatesInLayout(false);
        moveHandle->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
        moveHandle->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE);
        moveHandle->setOnPress([this](const InputArea::PointerData& data) {
          if (data.pressed && m_surface)
            xdg_toplevel_move(m_surface->xdgToplevel(), m_wayland.seat(), data.serial);
        });
        m_moveHandle = moveHandle.get();
        m_titleRow = text.get();
        text->addChild(std::move(moveHandle));
      }
      content->addChild(std::move(text));
      m_root->addChild(std::move(content));
    } else {
      auto topAccent = std::make_unique<DialogBackgroundArtwork>("top-left-diagonals.svg", 126.0F, 78.27F);
      m_topAccent = topAccent.get();
      m_root->addChild(std::move(topAccent));
      auto cornerAccent = std::make_unique<DialogBackgroundArtwork>("bottom-right-contour.svg", 78.0F, 46.0F);
      m_cornerAccent = cornerAccent.get();
      m_root->addChild(std::move(cornerAccent));
      auto heading = std::make_unique<Label>();
      heading->setText(m_title);
      heading->setMaxWidth(dialogWidth - 48.0F);
      heading->setMaxLines(2);
      heading->setFontWeight(FontWeight::SemiBold);
      heading->setColor(colorSpecFromRole(ColorRole::Primary));
      if (m_resizable) {
        auto titleRow = std::make_unique<Flex>();
        titleRow->setDirection(FlexDirection::Horizontal);
        titleRow->setAlign(FlexAlign::Center);
        heading->setFlexGrow(1.0F);
        titleRow->addChild(std::move(heading));
        auto moveHandle = std::make_unique<InputArea>();
        moveHandle->setParticipatesInLayout(false);
        moveHandle->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
        moveHandle->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE);
        moveHandle->setOnPress([this](const InputArea::PointerData& data) {
          if (data.pressed && m_surface)
            xdg_toplevel_move(m_surface->xdgToplevel(), m_wayland.seat(), data.serial);
        });
        m_moveHandle = moveHandle.get();
        titleRow->addChild(std::move(moveHandle));
        m_titleRow = titleRow.get();
        m_root->addChild(std::move(titleRow));
      } else
        m_root->addChild(std::move(heading));
    }
    if (m_alternativeNameEnabled && !m_textPrompt) {
      auto explanation = std::make_unique<Label>();
      explanation->setText("Choose what to do with the incoming item.");
      explanation->setMaxWidth(dialogWidth - 48.0F);
      m_root->addChild(std::move(explanation));
    }
    if (m_conflict && !m_textPrompt) {
      const auto addComparisonCard = [this, dialogWidth](const ConflictItemInfo& item, std::string_view sectionTitle) {
        auto card = std::make_unique<Flex>();
        m_cards.push_back(card.get());
        card->setDirection(FlexDirection::Vertical);
        card->setAlign(FlexAlign::Stretch);
        card->setGap(4.0F);
        card->setPadding(10.0F, 12.0F);
        card->setFill(colorSpecFromRole(ColorRole::SurfaceContainer));
        card->setBorder(colorSpecFromRole(ColorRole::OutlineVariant), Style::borderWidth);
        applyNytheraBevel(*card, Style::scaledRadiusSm(1.0F));
        auto headingLabel = std::make_unique<Label>();
        headingLabel->setText(sectionTitle);
        headingLabel->setFontWeight(FontWeight::SemiBold);
        headingLabel->setColor(colorSpecFromRole(ColorRole::Primary));
        card->addChild(std::move(headingLabel));
        auto itemRow = std::make_unique<Flex>();
        itemRow->setDirection(FlexDirection::Horizontal);
        itemRow->setAlign(FlexAlign::Center);
        itemRow->setGap(10.0F);
        auto icon = std::make_unique<FileIcon>();
        icon->setAllowFilesystemQueries(m_conflict->pathAccess == ConflictPathAccess::Local);
        // Pass the exact unmodified filesystem path and directory flag to FileIcon.
        icon->setFile(item.path, item.isDirectory);
        icon->setGlyphSize(28.0F);
        itemRow->addChild(std::move(icon));
        const std::string displayName = sanitizeConflictDisplayText(item.name);
        const std::string displayPath = sanitizeConflictDisplayText(item.path.string());
        auto details = std::make_unique<Label>();
        details->setText(displayName + "\n" + item.metadata);
        details->setMaxWidth(dialogWidth - 110.0F);
        details->setMaxLines(3);
        details->setEllipsize(TextEllipsize::End);
        details->setFlexGrow(1.0F);
        details->setTooltip(item.metadata);
        itemRow->addChild(std::move(details));
        card->addChild(std::move(itemRow));
        auto path = std::make_unique<Label>();
        path->setText(displayPath);
        path->setMaxWidth(dialogWidth - 56.0F);
        path->setMaxLines(2);
        path->setTooltip(displayPath);
        card->addChild(std::move(path));
        m_root->addChild(std::move(card));
      };
      addComparisonCard(m_conflict->incoming, "INCOMING ITEM");
      addComparisonCard(m_conflict->existing, "EXISTING DESTINATION");
      if (!m_conflict->comparisonSummary.empty()) {
        auto summary = std::make_unique<Label>();
        summary->setText(m_conflict->comparisonSummary);
        summary->setMaxWidth(dialogWidth - 48.0F);
        summary->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
        m_root->addChild(std::move(summary));
      }
    } else if (!m_warningStyle && !m_message.empty()) {
      auto message = std::make_unique<Label>();
      message->setText(m_message);
      message->setMaxWidth(dialogWidth - 48.0F);
      message->setMaxLines(14);
      m_root->addChild(std::move(message));
    }
    if (m_applyToAllCheckbox) {
      auto applyRow = std::make_unique<Flex>();
      applyRow->setDirection(FlexDirection::Horizontal);
      applyRow->setAlign(FlexAlign::Center);
      applyRow->setGap(6.0F);
      auto checkbox = std::make_unique<Checkbox>();
      checkbox->setOnChange([this](bool checked) { m_applyToAllChecked = checked; });
      applyRow->addChild(std::move(checkbox));
      auto label = std::make_unique<Label>();
      label->setText("Apply this decision to remaining conflicts");
      label->setMaxWidth(dialogWidth - 80.0F);
      applyRow->addChild(std::move(label));
      m_root->addChild(std::move(applyRow));
    }
    if (m_alternativeNameEnabled) {
      auto renameRow = std::make_unique<Flex>();
      renameRow->setDirection(FlexDirection::Horizontal);
      renameRow->setAlign(FlexAlign::Center);
      renameRow->setGap(8.0F);
      auto input = std::make_unique<Input>();
      m_alternativeNameInput = input.get();
      input->setPlaceholder(m_textPrompt ? "" : "Alternative name (for Keep Both)");
      input->setPasswordMode(m_passwordPrompt);
      input->setControlHeight(34.0F);
      input->setFlexGrow(1.0F);
      input->setOnSubmit([this](const std::string&) {
        if (m_textPrompt) {
          closeDialog(1);
          return;
        }
        const auto keepBoth =
            std::ranges::find_if(m_choices, [](const auto& choice) { return choice.first == "Keep Both"; });
        if (keepBoth != m_choices.end())
          closeDialog(keepBoth->second);
      });
      input->setOnKeyEvent([this](std::uint32_t sym, std::uint32_t) {
        if (m_textPrompt && (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter)) {
          closeDialog(1);
          return true;
        }
        return false;
      });
      renameRow->addChild(std::move(input));
      if (!m_textPrompt) {
        auto suggest = std::make_unique<Button>();
        suggest->setText("Suggest New Name");
        suggest->setVariant(ButtonVariant::Ghost);
        const auto incomingName = m_conflict ? m_conflict->incoming.name : m_message.substr(0, m_message.find('\n'));
        std::filesystem::path destinationPath;
        if (m_conflict) {
          destinationPath = m_conflict->existing.path;
        } else {
          const auto existingStart = m_message.find("\n\n") + 2;
          const auto existingMetadataEnd = m_message.find('\n', m_message.find('\n', existingStart) + 1);
          const auto destinationEnd = m_message.find('\n', existingMetadataEnd + 1);
          destinationPath =
              std::filesystem::path(m_message.substr(existingMetadataEnd + 1, destinationEnd - existingMetadataEnd - 1));
        }
        m_destination = destinationPath;
        m_alternativeNameInput->setOnChange([this](const std::string&) { m_alternativeNameInput->setInvalid(false); });
        const bool directory =
            m_conflict ? m_conflict->incoming.isDirectory : m_message.substr(m_message.find('\n') + 1).starts_with("Folder");
        const auto stem = directory ? incomingName : std::filesystem::path(incomingName).stem().string();
        const auto extension = directory ? std::string{} : std::filesystem::path(incomingName).extension().string();
        const bool checkDestination = !m_conflict || m_conflict->pathAccess == ConflictPathAccess::Local;
        suggest->setOnClick([this, parentPath = destinationPath.parent_path(), stem, extension, checkDestination,
                             nextSuffix = 1U]() mutable {
          for (unsigned suffix = checkDestination ? 1U : nextSuffix; suffix < 10000; ++suffix) {
            const auto candidate =
                stem + " (copy" + (suffix == 1 ? std::string{} : " " + std::to_string(suffix)) + ")" + extension;
            if (!checkDestination) {
              nextSuffix = suffix + 1;
              m_alternativeNameInput->setValue(candidate);
              break;
            }
            std::error_code error;
            const bool exists = std::filesystem::exists(parentPath / candidate, error);
            if (error)
              break;
            if (!exists) {
              m_alternativeNameInput->setValue(candidate);
              break;
            }
          }
        });
        renameRow->addChild(std::move(suggest));
      }
      m_root->addChild(std::move(renameRow));
    }
    if (m_symlinkDialogActive) {
      const auto addField = [this](const char* text) {
        auto field = std::make_unique<Flex>();
        field->setDirection(FlexDirection::Vertical);
        field->setAlign(FlexAlign::Stretch);
        field->setGap(4.0F);
        auto label = std::make_unique<Label>();
        label->setText(text);
        label->setFontWeight(FontWeight::SemiBold);
        field->addChild(std::move(label));
        auto input = std::make_unique<Input>();
        input->setControlHeight(34.0F);
        auto* control = input.get();
        field->addChild(std::move(input));
        control->setOnChange([this, control](const std::string&) {
          control->setInvalid(false);
          m_symlinkError->setText(" ");
          if (m_surface)
            m_surface->requestLayout();
        });
        m_root->addChild(std::move(field));
        return control;
      };
      m_symlinkName = addField("Symlink name");
      m_symlinkTarget = addField("Target path");
      m_symlinkName->setOnSubmit([this](const std::string&) { m_dispatcher.setFocus(m_symlinkTarget->inputArea()); });
      m_symlinkTarget->setOnSubmit([this](const std::string&) { closeDialog(1); });
      auto error = std::make_unique<Label>();
      error->setText(" ");
      error->setColor(colorSpecFromRole(ColorRole::Error));
      error->setMaxWidth(dialogWidth - 48.0F);
      error->setMaxLines(2);
      m_symlinkError = error.get();
      m_root->addChild(std::move(error));
    }
    if (m_customContent && m_customContent->buildFields)
      m_customContent->buildFields(*m_root, dialogWidth);
    auto row = std::make_unique<Flex>();
    row->setAlign(FlexAlign::Center);
    row->setJustify(FlexJustify::End);
    row->setGap(8.0F);
    row->setWrap(true);
    InputArea* safeFocus = nullptr;
    for (const auto& [label, result] : m_choices) {
      auto button = std::make_unique<Button>();
      button->setText(label);
      const bool destructive = result != 0
          && (label.find("Delete") != std::string::npos
              || label == "Quit"
              || label == "Empty Wastebin"
              || label.starts_with("Replace"));
      button->setVariant(destructive ? ButtonVariant::Destructive : ButtonVariant::Outline);
      button->setOnClick([this, result] { closeDialog(result); });
      if (m_customContent && m_customContent->bindChoiceButton)
        m_customContent->bindChoiceButton(result, button.get());
      if (label == "Cancel")
        safeFocus = button->inputArea();
      row->addChild(std::move(button));
    }
    auto spacer = std::make_unique<Flex>();
    if (m_customContent && m_customContent->fillContent) {
      spacer->setFlexGrow(0.0F);
      spacer->setVisible(false);
      spacer->setParticipatesInLayout(false);
    } else {
      spacer->setFlexGrow(1.0F);
    }
    m_spacer = spacer.get();
    m_root->addChild(std::move(spacer));
    auto footer = std::make_unique<Flex>();
    footer->setDirection(FlexDirection::Vertical);
    footer->setAlign(FlexAlign::Stretch);
    footer->setGap(8.0F);
    auto divider = std::make_unique<Separator>();
    divider->setOrientation(SeparatorOrientation::HorizontalRule);
    divider->setColor(colorSpecFromRole(ColorRole::OutlineVariant));
    m_divider = footer->addChild(std::move(divider));
    footer->addChild(std::move(row));
    m_footer = footer.get();
    m_root->addChild(std::move(footer));
    if (m_resizable) {
      const std::array<std::uint32_t, 8> edges{
          XDG_TOPLEVEL_RESIZE_EDGE_TOP,         XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM,      XDG_TOPLEVEL_RESIZE_EDGE_LEFT,
          XDG_TOPLEVEL_RESIZE_EDGE_RIGHT,       XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT,    XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT,
          XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT, XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT
      };
      const std::array<std::uint32_t, 8> cursors{
          WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE,   WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE,
          WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE,   WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE,
          WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE,
          WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE
      };
      for (std::size_t i = 0; i < edges.size(); ++i) {
        auto edge = std::make_unique<InputArea>();
        edge->setParticipatesInLayout(false);
        edge->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
        edge->setCursorShape(cursors[i]);
        edge->setOnPress([this, side = edges[i]](const InputArea::PointerData& data) {
          if (data.pressed && m_surface)
            xdg_toplevel_resize(m_surface->xdgToplevel(), m_wayland.seat(), data.serial, side);
        });
        m_resizeEdges[i] = edge.get();
        m_root->addChild(std::move(edge));
      }
      m_dispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
        m_wayland.setCursorShape(serial, shape);
      });
    }
    if (m_customContent && m_customContent->afterBuild)
      m_customContent->afterBuild();
    updateTheme();
    m_dispatcher.setSceneRoot(m_root.get());
    if (m_symlinkName)
      m_dispatcher.setFocus(m_symlinkName->inputArea());
    else if (m_customContent && m_customContent->initialFocus && m_customContent->initialFocus() != nullptr)
      m_dispatcher.setFocus(m_customContent->initialFocus());
    else if (m_textPrompt && m_alternativeNameInput)
      m_dispatcher.setFocus(m_alternativeNameInput->inputArea());
    else if (safeFocus != nullptr)
      m_dispatcher.setFocus(safeFocus);
  }

  void ModalDialogHost::updateTheme() {
    if (!m_root)
      return;
    const auto mode = m_configService.config().shell.panel.transparencyMode;
    const float background = detachedPanelBackgroundOpacityForTransparencyMode(mode);
    m_root->setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow, background));
    for (auto* card : m_cards)
      card->setFill(
          colorSpecFromRole(ColorRole::SurfaceContainer, panelCardOpacityForTransparencyMode(mode, background))
      );
    if (m_customContent && m_customContent->onThemeUpdated)
      m_customContent->onThemeUpdated();
  }

  void ModalDialogHost::prepareFrame(bool, bool needsLayout) {
    if (!m_surface || m_surface->width() == 0 || m_surface->height() == 0)
      return;
    m_renderContext.makeCurrent(m_surface->renderTarget());
    auto& renderer = m_surface->renderTarget().renderer();
    if (needsLayout) {
      UiPhaseScope layoutPhase(UiPhase::Layout);
      m_root->setSize(static_cast<float>(m_surface->width()), static_cast<float>(m_surface->height()));
      m_root->layout(renderer);
      if (m_moveHandle && m_titleRow) {
        m_moveHandle->setPosition(0.0F, 0.0F);
        m_moveHandle->setSize(m_titleRow->width(), m_titleRow->height());
        m_moveHandle->layout(renderer);
      }
      if (m_warningStyle)
        m_deleteWarning->setSourceFile(renderer, kDeleteWarning, 72);
      if (m_customContent && m_customContent->onPrepareLayout)
        m_customContent->onPrepareLayout(renderer);
      m_topAccent->setPosition(2.0F, 2.0F);
      m_topAccent->load(renderer);
      const float cornerX = m_root->width() - m_cornerAccent->width() - 7.0F;
      float dividerX = 0.0F;
      float dividerY = 0.0F;
      Node::absolutePosition(m_divider, dividerX, dividerY);
      const float cornerY = dividerY - m_cornerAccent->height() - 5.0F;
      m_cornerAccent->setVisible(cornerX >= 2.0F && cornerY >= 2.0F);
      m_cornerAccent->setPosition(cornerX, cornerY);
      m_cornerAccent->load(renderer);
      if (m_resizable) {
        const float width = m_root->width();
        const float height = m_root->height();
        const std::array<std::array<float, 4>, 8> bounds{
            {{10.0F, 0.0F, width - 20.0F, 10.0F},
             {10.0F, height - 10.0F, width - 20.0F, 10.0F},
             {0.0F, 10.0F, 10.0F, height - 20.0F},
             {width - 10.0F, 10.0F, 10.0F, height - 20.0F},
             {0.0F, 0.0F, 10.0F, 10.0F},
             {width - 10.0F, 0.0F, 10.0F, 10.0F},
             {0.0F, height - 10.0F, 10.0F, 10.0F},
             {width - 10.0F, height - 10.0F, 10.0F, 10.0F}}
        };
        for (std::size_t i = 0; i < bounds.size(); ++i) {
          m_resizeEdges[i]->setPosition(bounds[i][0], bounds[i][1]);
          m_resizeEdges[i]->setSize(bounds[i][2], bounds[i][3]);
          m_resizeEdges[i]->layout(renderer);
        }
      }
    }
  }

} // namespace filemanager
