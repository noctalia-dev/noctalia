pragma Singleton

import QtQuick
import Quickshell
import qs.Commons

Singleton {
  id: root

  property real scoreThreshold: 0.2

  // Manual overrides for tricky apps
  property var substitutions: ({
                                 "code-url-handler": "visual-studio-code",
                                 "Code": "visual-studio-code",
                                 "gnome-tweaks": "org.gnome.tweaks",
                                 "opencode": "opencode-desktop",
                                 "@opencode-aidesktop": "opencode-desktop",
                                 "pavucontrol-qt": "pavucontrol",
                                 "wps": "wps-office2019-kprometheus",
                                 "wpsoffice": "wps-office2019-kprometheus",
                                 "footclient": "foot"
                               })

  // Dynamic fixups
  property var regexSubstitutions: [
    {
      "regex": /^steam_app_(\d+)$/,
      "replace": "steam_icon_$1"
    },
    {
      "regex": /Minecraft.*/,
      "replace": "minecraft-launcher"
    },
    {
      "regex": /.*polkit.*/,
      "replace": "system-lock-screen"
    },
    {
      "regex": /gcr.prompter/,
      "replace": "system-lock-screen"
    }
  ]

  property list<DesktopEntry> entryList: []
  property var preppedNames: []
  property var preppedIcons: []
  property var preppedIds: []

  Component.onCompleted: refreshEntries()

  Connections {
    target: DesktopEntries.applications
    function onValuesChanged() {
      refreshEntries();
    }
  }

  function refreshEntries() {
    if (typeof DesktopEntries === 'undefined')
      return;

    const values = Array.from(DesktopEntries.applications.values);
    if (values) {
      entryList = values.sort((a, b) => a.name.localeCompare(b.name));
      updatePreppedData();
    }
  }

  function updatePreppedData() {
    if (typeof FuzzySort === 'undefined')
      return;

    const list = Array.from(entryList);
    preppedNames = list.map(a => ({
                                    name: FuzzySort.prepare(`${a.name} `),
                                    entry: a
                                  }));
    preppedIcons = list.map(a => ({
                                    name: FuzzySort.prepare(`${a.icon} `),
                                    entry: a
                                  }));
    preppedIds = list.map(a => ({
                                  name: FuzzySort.prepare(`${a.id} `),
                                  entry: a
                                }));
  }

  function iconForAppId(appId, fallbackName) {
    const fallback = fallbackName || "application-x-executable";
    if (!appId)
      return iconFromName(fallback, fallback);

    const entry = findAppEntry(appId);
    if (entry) {
      return iconFromName(entry.icon, fallback);
    }

    return iconFromName(appId, fallback);
  }

  // Robust lookup strategy
  function findAppEntry(str) {
    if (!str || str.length === 0)
      return null;

    let result = null;

    if (result = checkHeuristic(str))
      return result;
    if (result = checkSubstitutions(str))
      return result;
    if (result = checkRegex(str))
      return result;
    if (result = checkSimpleTransforms(str))
      return result;
    if (result = checkFuzzySearch(str))
      return result;
    if (result = checkCleanMatch(str))
      return result;

    return null;
  }

  function iconFromName(iconName, fallbackName) {
    const fallback = fallbackName || "application-x-executable";
    try {
      if (iconName && typeof Quickshell !== 'undefined' && Quickshell.iconPath) {
        const p = Quickshell.iconPath(iconName, fallback);
        if (p && p !== "")
          return p;
      }
    } catch (e) {}

    try {
      return Quickshell.iconPath ? (Quickshell.iconPath(fallback, true) || "") : "";
    } catch (e2) {
      return "";
    }
  }

  function distroLogoPath() {
    try {
      return (typeof OSInfo !== 'undefined' && OSInfo.distroIconPath) ? OSInfo.distroIconPath : "";
    } catch (e) {
      return "";
    }
  }

  // --- Lookup Helpers ---

  function checkHeuristic(str) {
    if (typeof DesktopEntries !== 'undefined' && DesktopEntries.heuristicLookup) {
      const entry = DesktopEntries.heuristicLookup(str);
      if (entry)
        return entry;
    }
    return null;
  }

  function checkSubstitutions(str) {
    let effectiveStr = substitutions[str];
    if (!effectiveStr)
      effectiveStr = substitutions[str.toLowerCase()];

    if (effectiveStr && effectiveStr !== str) {
      return findAppEntry(effectiveStr);
    }
    return null;
  }

  function checkRegex(str) {
    for (let i = 0; i < regexSubstitutions.length; i++) {
      const sub = regexSubstitutions[i];
      const replaced = str.replace(sub.regex, sub.replace);
      if (replaced !== str) {
        return findAppEntry(replaced);
      }
    }
    return null;
  }

  function checkSimpleTransforms(str) {
    if (typeof DesktopEntries === 'undefined' || !DesktopEntries.byId)
      return null;

    const lower = str.toLowerCase();

    const variants = [str, lower, getFromReverseDomain(str), getFromReverseDomain(str)?.toLowerCase(), normalizeWithHyphens(str), str.replace(/_/g, '-').toLowerCase(), str.replace(/-/g, '_').toLowerCase()];

    for (let i = 0; i < variants.length; i++) {
      const variant = variants[i];
      if (variant) {
        const entry = DesktopEntries.byId(variant);
        if (entry)
          return entry;
      }
    }
    return null;
  }

  function checkFuzzySearch(str) {
    if (typeof FuzzySort === 'undefined')
      return null;

    // Require the matched entry id to be length-coherent with the query.
    // Prevents a short id (e.g. "code", 4 chars) from outscoring a longer
    // partial match (e.g. "opencode-desktop", 16 chars) when searching for
    // "opencode" — FuzzySort normalizes score by target length, so tight
    // short substrings otherwise dominate.
    const minIdLen = Math.max(3, str.length - 2);
    const lengthOk = (entry) => entry && entry.id && entry.id.length >= minIdLen;

    // Check filenames (IDs) first
    if (preppedIds.length > 0) {
      let results = fuzzyQuery(str, preppedIds).filter(lengthOk);
      if (results.length === 0) {
        const underscored = str.replace(/-/g, '_').toLowerCase();
        if (underscored !== str)
          results = fuzzyQuery(underscored, preppedIds).filter(lengthOk);
      }
      if (results.length > 0)
        return results[0];
    }

    // Then icons
    if (preppedIcons.length > 0) {
      const results = fuzzyQuery(str, preppedIcons).filter(lengthOk);
      if (results.length > 0)
        return results[0];
    }

    // Then names
    if (preppedNames.length > 0) {
      const results = fuzzyQuery(str, preppedNames).filter(lengthOk);
      if (results.length > 0)
        return results[0];
    }

    return null;
  }

  function checkCleanMatch(str) {
    if (!str || str.length <= 3)
      return null;
    if (typeof DesktopEntries === 'undefined' || !DesktopEntries.byId)
      return null;

    // Aggressive fallback: strip all separators.
    // Only match when the entry id is a superset of the query (cleaned) —
    // never the other way around. The reverse direction (e.g. "code"
    // matching a query of "opencode" because "opencode".includes("code"))
    // was the source of the misresolution where VS Code's icon was shown
    // for opencode-desktop.
    const cleanStr = str.toLowerCase().replace(/[\.\-_]/g, '');
    const list = Array.from(entryList);

    for (let i = 0; i < list.length; i++) {
      const entry = list[i];
      const cleanId = (entry.id || "").toLowerCase().replace(/[\.\-_]/g, '');
      if (cleanId.includes(cleanStr)) {
        return entry;
      }
    }
    return null;
  }

  function fuzzyQuery(search, preppedData) {
    if (!search || !preppedData || preppedData.length === 0)
      return [];
    return FuzzySort.go(search, preppedData, {
                          all: true,
                          key: "name"
                        }).map(r => r.obj.entry);
  }

  function iconExists(iconName) {
    if (!iconName || iconName.length === 0)
      return false;
    if (iconName.startsWith("/"))
      return true;

    const path = Quickshell.iconPath(iconName, true);
    return path && path.length > 0 && !path.includes("image-missing");
  }

  function getFromReverseDomain(str) {
    if (!str)
      return "";
    return str.split('.').slice(-1)[0];
  }

  function normalizeWithHyphens(str) {
    if (!str)
      return "";
    return str.toLowerCase().replace(/\s+/g, "-");
  }

  // Deprecated shim
  function guessIcon(str) {
    const entry = findAppEntry(str);
    return entry ? entry.icon : "image-missing";
  }
}
