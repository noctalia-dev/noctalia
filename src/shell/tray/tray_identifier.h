#pragma once

#include "dbus/tray/tray_service.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace tray {

  inline bool isUniqueBusName(std::string_view value) { return !value.empty() && value.front() == ':'; }

  inline bool isTransientUniqueIdentifier(std::string_view value) {
    return isUniqueBusName(value) || value.find("/:") != std::string_view::npos;
  }

  inline std::string lastPathSegment(std::string_view value) {
    if (value.empty()) {
      return {};
    }
    const auto slash = value.find_last_of('/');
    if (slash == std::string_view::npos || slash + 1 >= value.size()) {
      return std::string(value);
    }
    return std::string(value.substr(slash + 1));
  }

  inline bool looksGenericStatusItemName(std::string_view value) {
    if (value.empty()) {
      return true;
    }
    const auto lower = StringUtils::toLower(value);
    return lower.find("status_icon") != std::string::npos
        || lower.find("statusnotifieritem") != std::string::npos
        || lower.find("statusnotifier") != std::string::npos
        || lower.find("status-notifier") != std::string::npos
        || lower.find("status notifier") != std::string::npos
        || lower == "electron"
        || lower == "xdg-dbus-proxy";
  }

  inline std::vector<std::string> identifierVariants(std::string_view value) {
    std::vector<std::string> out;
    if (value.empty()) {
      return out;
    }

    auto pushUnique = [&out](std::string candidate) {
      if (candidate.empty()) {
        return;
      }
      if (!std::ranges::contains(out, candidate)) {
        out.push_back(std::move(candidate));
      }
    };

    std::string base(value);
    pushUnique(base);
    pushUnique(StringUtils::toLower(base));

    if (const auto slash = base.find_last_of('/'); slash != std::string::npos && slash + 1 < base.size()) {
      base = base.substr(slash + 1);
      pushUnique(base);
      pushUnique(StringUtils::toLower(base));
    }

    std::string dashed = base;
    std::replace(dashed.begin(), dashed.end(), '_', '-');
    pushUnique(dashed);
    pushUnique(StringUtils::toLower(dashed));

    std::string underscored = base;
    std::replace(underscored.begin(), underscored.end(), '-', '_');
    pushUnique(underscored);
    pushUnique(StringUtils::toLower(underscored));

    auto pushReducedForms = [&pushUnique](std::string candidate) {
      if (candidate.empty()) {
        return;
      }

      pushUnique(candidate);
      pushUnique(StringUtils::toLower(candidate));

      bool changed = true;
      while (changed && !candidate.empty()) {
        changed = false;

        for (const auto& suffix :
             {"_client", "-client", ".desktop", "_indicator", "-indicator", "_tray", "-tray", "_status", "-status",
              "_panel", "-panel"}) {
          if (candidate.size() > std::char_traits<char>::length(suffix) && candidate.ends_with(suffix)) {
            candidate = candidate.substr(0, candidate.size() - std::char_traits<char>::length(suffix));
            pushUnique(candidate);
            pushUnique(StringUtils::toLower(candidate));
            changed = true;
            break;
          }
        }

        if (changed || candidate.empty()) {
          continue;
        }

        const auto separator = candidate.find_last_of("-_");
        if (separator != std::string::npos && separator + 1 < candidate.size()) {
          const std::string tail = candidate.substr(separator + 1);
          const bool numericTail = std::ranges::all_of(tail, [](unsigned char c) { return std::isdigit(c) != 0; });
          if (numericTail) {
            candidate = candidate.substr(0, separator);
            pushUnique(candidate);
            pushUnique(StringUtils::toLower(candidate));
            changed = true;
            continue;
          }
        }

        for (const auto& suffix : {"-linux", "_linux"}) {
          if (candidate.size() > std::char_traits<char>::length(suffix) && candidate.ends_with(suffix)) {
            candidate = candidate.substr(0, candidate.size() - std::char_traits<char>::length(suffix));
            pushUnique(candidate);
            pushUnique(StringUtils::toLower(candidate));
            changed = true;
            break;
          }
        }
      }
    };

    for (const auto& candidate : std::vector<std::string>{base, dashed, underscored}) {
      pushReducedForms(candidate);
    }

    return out;
  }

  inline void appendIdentifierVariants(std::vector<std::string>& candidates, std::string_view text) {
    if (isTransientUniqueIdentifier(text)) {
      return;
    }
    for (const auto& variant : identifierVariants(text)) {
      if (!isTransientUniqueIdentifier(variant)
          && !looksGenericStatusItemName(variant)
          && !std::ranges::contains(candidates, variant)) {
        candidates.push_back(variant);
      }
    }
  }

  inline std::vector<std::string> pinMatchCandidates(const TrayItemInfo& item) {
    std::vector<std::string> candidates;
    appendIdentifierVariants(candidates, item.id);
    appendIdentifierVariants(candidates, item.busName);
    appendIdentifierVariants(candidates, item.itemName);
    appendIdentifierVariants(candidates, item.title);
    appendIdentifierVariants(candidates, item.statusNotifierTitle);
    appendIdentifierVariants(candidates, item.statusNotifierDescription);
    appendIdentifierVariants(candidates, item.objectPath);
    appendIdentifierVariants(candidates, item.iconName);
    appendIdentifierVariants(candidates, item.overlayIconName);
    appendIdentifierVariants(candidates, item.attentionIconName);
    appendIdentifierVariants(candidates, item.processName);
    return candidates;
  }

  inline bool tokenMatchesItem(std::string_view token, const TrayItemInfo& item) {
    if (looksGenericStatusItemName(token) || isTransientUniqueIdentifier(token)) {
      return false;
    }
    const auto normalizedToken = StringUtils::toLower(token);
    const auto candidates = pinMatchCandidates(item);
    return std::ranges::contains(candidates, normalizedToken);
  }

  [[nodiscard]] inline std::string capitalizeFirstLetter(std::string text) {
    if (!text.empty()) {
      text.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(text.front())));
    }
    return text;
  }

  [[nodiscard]] inline std::string normalizedTooltipMatchKey(std::string text) {
    text = StringUtils::toLower(std::move(text));
    text.erase(std::remove(text.begin(), text.end(), ' '), text.end());
    return text;
  }

  // Merge StatusNotifierItem Title with ToolTip title/description for hover text.
  [[nodiscard]] inline std::string formatTrayItemTooltip(const TrayItemInfo& item) {
    std::string tooltipTitle;
    if (!item.statusNotifierDescription.empty()) {
      tooltipTitle = item.statusNotifierDescription;
    } else if (!item.statusNotifierTitle.empty()) {
      tooltipTitle = item.statusNotifierTitle;
    }

    if (tooltipTitle.empty()) {
      return capitalizeFirstLetter(item.title);
    }

    if (item.title.empty()) {
      return tooltipTitle;
    }

    const std::string lowerTitle = StringUtils::toLower(item.title);
    const std::string lowerTooltipTitle = normalizedTooltipMatchKey(tooltipTitle);
    if (lowerTooltipTitle.find(lowerTitle) != std::string::npos) {
      return tooltipTitle;
    }

    return capitalizeFirstLetter(item.title) + " - " + tooltipTitle;
  }

  inline std::string preferredPinToken(const TrayItemInfo& item) {
    // Persist stable human-readable tokens; avoid transient :1.xxx ids and
    // generic Electron/proxy/SNI names shared by unrelated applications.
    if (!looksGenericStatusItemName(item.itemName)) {
      return item.itemName;
    }
    if (!looksGenericStatusItemName(item.statusNotifierTitle)) {
      return item.statusNotifierTitle;
    }
    if (!looksGenericStatusItemName(item.statusNotifierDescription)) {
      return item.statusNotifierDescription;
    }
    if (!looksGenericStatusItemName(item.iconName)) {
      return item.iconName;
    }
    if (!looksGenericStatusItemName(item.overlayIconName)) {
      return item.overlayIconName;
    }
    if (!looksGenericStatusItemName(item.attentionIconName)) {
      return item.attentionIconName;
    }
    if (!looksGenericStatusItemName(item.title)) {
      return item.title;
    }
    if (!looksGenericStatusItemName(item.processName)) {
      return item.processName;
    }
    if (const auto objectToken = lastPathSegment(item.objectPath);
        !looksGenericStatusItemName(objectToken) && !isUniqueBusName(objectToken)) {
      return objectToken;
    }
    if (const auto idToken = lastPathSegment(item.id);
        !looksGenericStatusItemName(idToken) && !isUniqueBusName(idToken)) {
      return idToken;
    }
    if (!isTransientUniqueIdentifier(item.busName) && !looksGenericStatusItemName(item.busName)) {
      return item.busName;
    }
    return {};
  }

} // namespace tray
