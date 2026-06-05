pragma Singleton

import QtQuick
import Quickshell
import Quickshell.Io
import qs.Commons
import qs.Services.Compositor
import qs.Services.UI

Singleton {
  id: root
  property string currentLayout: I18n.tr("common.unknown")
  property string fullLayoutName: I18n.tr("common.unknown")
  property string previousLayout: ""
  property bool isInitialized: false
  property var allLayouts: []

  // Updates current layout from various format strings. Called by compositors
  function setCurrentLayout(layoutString) {
    root.fullLayoutName = layoutString || I18n.tr("common.unknown");
    root.currentLayout = extractLayoutCode(layoutString);
  }

  // Called by compositor backends that expose a full layout list (e.g. niri)
  function setAllLayouts(layouts) {
    root.allLayouts = layouts || [];
  }

  // Extract layout code from various format strings
  // Priority: variant > country code > language lookup > fallback
  function extractLayoutCode(layoutString) {
    if (!layoutString)
      return I18n.tr("common.unknown");

    const str = layoutString.toLowerCase();

    // If it's already a short code (2-3 chars), return uppercase
    if (/^[a-z]{2,3}(\+.*)?$/.test(str)) {
      return str.split('+')[0].toUpperCase();
    }

    // Check for layout variants first - these are more meaningful than country codes
    // when distinguishing between multiple layouts of the same language
    for (const [pattern, display] of Object.entries(variantMap)) {
      if (str.includes(pattern)) {
        return display;
      }
    }

    // Extract short code from parentheses like "English (US)"
    const shortCodeMatch = str.match(/\(([a-z]{2,3})\)/i);
    if (shortCodeMatch) {
      return shortCodeMatch[1].toUpperCase();
    }

    // Check for language/country names in the language map
    // First, try to match at the start of the string (the primary language)
    for (const [lang, code] of Object.entries(languageMap)) {
      if (str.startsWith(lang)) {
        return code.toUpperCase();
      }
    }
    // Then try word boundary matching anywhere in the string
    for (const [lang, code] of Object.entries(languageMap)) {
      const regex = new RegExp(`\\b${lang}\\b`);
      if (regex.test(str)) {
        return code.toUpperCase();
      }
    }

    // If nothing matches, try first 2-3 characters if they look like a code
    const codeMatch = str.match(/^([a-z]{2,3})/);
    return codeMatch ? codeMatch[1].toUpperCase() : I18n.tr("common.unknown");
  }

  // Watch for layout changes and show toast
  onCurrentLayoutChanged: {
    // Update previousLayout after checking for changes
    const layoutChanged = isInitialized && currentLayout !== previousLayout && currentLayout !== I18n.tr("common.unknown") && previousLayout !== "" && previousLayout !== I18n.tr("common.unknown");

    if (layoutChanged) {
      if (Settings.data.notifications.enableKeyboardLayoutToast) {
        const message = I18n.tr("toast.keyboard-layout.changed", {
                                  "layout": fullLayoutName
                                });
        ToastService.showNotice(I18n.tr("toast.keyboard-layout.title"), message, "", 2000);
      }
      Logger.d("KeyboardLayout", "Layout changed from", previousLayout, "to", currentLayout);
    }

    // Update previousLayout for next comparison
    previousLayout = currentLayout;
  }

  Component.onCompleted: {
    Logger.i("KeyboardLayout", "Service started");
    // Mark as initialized after a delay to allow first layout update to complete
    // This prevents showing a toast on the initial load
    initializationTimer.start();
  }

  Timer {
    id: initializationTimer
    interval: 2000 // Wait 2 seconds for first layout update to complete
    onTriggered: {
      isInitialized = true;
      // Set previousLayout to current value after initialization
      previousLayout = currentLayout;
      Logger.d("KeyboardLayout", "Service initialized, current layout:", currentLayout);
    }
  }

  // Layout variants - checked BEFORE country codes
  // These display the variant name when it's more meaningful than the country
  property var variantMap: {
    // Alternative keyboard layouts
    "colemak": "Colemak",
    "dvorak": "Dvorak",
    "workman": "Workman",
    "programmer dvorak": "Dvk-P",
    "norman": "Norman",
    // International variants
    "intl": "Intl",
    "international": "Intl",
    "altgr-intl": "Intl",
    "with dead keys": "Dead",
    // Common variants
    "phonetic": "Phon",
    "extended": "Ext",
    "ergonomic": "Ergo",
    "legacy": "Legacy",
    // Input methods
    "pinyin": "Pinyin",
    "cangjie": "Cangjie",
    "romaji": "Romaji",
    "kana": "Kana"
  }

  // Language/country name to ISO code mapping
  property var languageMap: {
    // English variants
    "english": "us",
    "american": "us",
    "united states": "us",
    "us english": "us",
    "british": "gb",
    "united kingdom": "gb",
    "english (uk)": "gb",
    "canadian": "ca",
    "canada": "ca",
    "canadian english": "ca",
    "australian": "au",
    "australia": "au",
    // Nordic countries
    "swedish": "se",
    "svenska": "se",
    "sweden": "se",
    "norwegian": "no",
    "norsk": "no",
    "norway": "no",
    "danish": "dk",
    "dansk": "dk",
    "denmark": "dk",
    "finnish": "fi",
    "suomi": "fi",
    "finland": "fi",
    "icelandic": "is",
    "íslenska": "is",
    "iceland": "is",
    // Western/Central European Germanic
    "german": "de",
    "deutsch": "de",
    "germany": "de",
    "austrian": "at",
    "austria": "at",
    "österreich": "at",
    "swiss": "ch",
    "switzerland": "ch",
    "schweiz": "ch",
    "suisse": "ch",
    "dutch": "nl",
    "nederlands": "nl",
    "netherlands": "nl",
    "holland": "nl",
    "belgian": "be",
    "belgium": "be",
    "belgië": "be",
    "belgique": "be",
    // Romance languages (Western/Southern Europe)
    "french": "fr",
    "français": "fr",
    "france": "fr",
    "canadian french": "ca",
    "spanish": "es",
    "español": "es",
    "spain": "es",
    "castilian": "es",
    "italian": "it",
    "italiano": "it",
    "italy": "it",
    "portuguese": "pt",
    "português": "pt",
    "portugal": "pt",
    "catalan": "ad",
    "català": "ad",
    "andorra": "ad",
    // Eastern European Romance
    "romanian": "ro",
    "română": "ro",
    "romania": "ro",
    // Slavic languages (Eastern Europe)
    "russian": "ru",
    "русский": "ru",
    "russia": "ru",
    "polish": "pl",
    "polski": "pl",
    "poland": "pl",
    "czech": "cz",
    "čeština": "cz",
    "czech republic": "cz",
    "slovak": "sk",
    "slovenčina": "sk",
    "slovakia": "sk",
    // Ukrainian
    "ukraine": "ua",
    "ukrainian": "ua",
    "українська": "ua",
    "bulgarian": "bg",
    "български": "bg",
    "bulgaria": "bg",
    "serbian": "rs",
    "srpski": "rs",
    "serbia": "rs",
    "croatian": "hr",
    "hrvatski": "hr",
    "croatia": "hr",
    "slovenian": "si",
    "slovenščina": "si",
    "slovenia": "si",
    "bosnian": "ba",
    "bosanski": "ba",
    "bosnia": "ba",
    "macedonian": "mk",
    "македонски": "mk",
    "macedonia": "mk",
    // Celtic languages (Western Europe)
    "irish": "ie",
    "gaeilge": "ie",
    "ireland": "ie",
    "welsh": "gb",
    "cymraeg": "gb",
    "wales": "gb",
    "scottish": "gb",
    "gàidhlig": "gb",
    "scotland": "gb",
    // Baltic languages (Northern Europe)
    "estonian": "ee",
    "eesti": "ee",
    "estonia": "ee",
    "latvian": "lv",
    "latviešu": "lv",
    "latvia": "lv",
    "lithuanian": "lt",
    "lietuvių": "lt",
    "lithuania": "lt",
    // Other European languages
    "hungarian": "hu",
    "magyar": "hu",
    "hungary": "hu",
    "greek": "gr",
    "ελληνικά": "gr",
    "greece": "gr",
    "albanian": "al",
    "shqip": "al",
    "albania": "al",
    "maltese": "mt",
    "malti": "mt",
    "malta": "mt",
    // West/Southwest Asian languages
    "turkish": "tr",
    "türkçe": "tr",
    "turkey": "tr",
    "arabic": "ar",
    "العربية": "ar",
    "arab": "ar",
    "hebrew": "il",
    "עברית": "il",
    "israel": "il",
    // South American languages
    "brazilian": "br",
    "brazilian portuguese": "br",
    "brasil": "br",
    "brazil": "br",
    // East Asian languages
    "japanese": "jp",
    "日本語": "jp",
    "japan": "jp",
    "korean": "kr",
    "한국어": "kr",
    "korea": "kr",
    "south korea": "kr",
    "chinese": "cn",
    "中文": "cn",
    "china": "cn",
    "simplified chinese": "cn",
    "traditional chinese": "tw",
    "taiwan": "tw",
    "繁體中文": "tw",
    // Southeast Asian languages
    "thai": "th",
    "ไทย": "th",
    "thailand": "th",
    "vietnamese": "vn",
    "tiếng việt": "vn",
    "vietnam": "vn",
    // South Asian languages
    "hindi": "in",
    "हिन्दी": "in",
    "india": "in",
    // African languages
    "afrikaans": "za",
    "south africa": "za",
    "south african": "za"
  }
}
