#include "system/lyrics_catalog.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <glib.h>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <tuple>
#include <utility>
#include <vector>

namespace {
  using Tokens = std::vector<std::string>;

  constexpr double kExactDurationMs = 3000.0;
  // Music videos often add intros/outros around the audio recording.
  constexpr double kLooseDurationMs = 30000.0;
  // A title only counts as a decorated version of the previous one when it adds at least this many words.
  constexpr std::size_t kMinDecorationWords = 3;
  // Words in the player title that neither the matched recording nor video decoration explains, e.g. an unlisted guest.
  constexpr std::size_t kMaxUnexplainedTitleWords = 2;

  struct Token {
    std::string text;
    bool separatedBefore = false;
  };

  // Casefolded alphanumeric words; `separatedBefore` marks list punctuation (`,&+/;|` or a dash) before the word.
  std::vector<Token> scan(std::string_view text) {
    if (text.empty() || !g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)) {
      return {};
    }
    std::unique_ptr<char, decltype(&g_free)> normalized(
        g_utf8_normalize(text.data(), static_cast<gssize>(text.size()), G_NORMALIZE_ALL_COMPOSE), g_free
    );
    std::unique_ptr<char, decltype(&g_free)> folded(g_utf8_casefold(normalized.get(), -1), g_free);
    std::vector<Token> result;
    std::string word;
    bool separator = false;
    const auto flush = [&] {
      if (!word.empty()) {
        result.push_back({.text = std::move(word), .separatedBefore = separator});
        word.clear();
        separator = false;
      }
    };
    for (const char* p = folded.get(); *p != '\0'; p = g_utf8_next_char(p)) {
      const auto codepoint = g_utf8_get_char(p);
      if (g_unichar_isalnum(codepoint) || g_unichar_ismark(codepoint)) {
        word.append(p, static_cast<std::size_t>(g_utf8_next_char(p) - p));
        continue;
      }
      flush();
      constexpr std::array<gunichar, 11> separators = {
          ',', '&', '+', '/', ';', '|', '-', 0x2013, 0x2014, 0x3001, 0xFF0C,
      };
      if (std::ranges::contains(separators, codepoint)) {
        separator = true;
      }
    }
    flush();
    return result;
  }

  Tokens tokens(std::string_view text) {
    Tokens result;
    for (auto& token : scan(text)) {
      result.push_back(std::move(token.text));
    }
    return result;
  }

  bool contains(const Tokens& haystack, const Tokens& needle) {
    return !needle.empty() && !std::ranges::search(haystack, needle).empty();
  }

  std::string compact(const Tokens& words) {
    std::string result;
    for (const auto& word : words) {
      result += word;
    }
    return result;
  }

  // Whether consecutive words spell `glued`, e.g. {outline, in, color} for the channel name "OutlineInColor".
  bool spells(const Tokens& words, std::string_view glued) {
    if (glued.empty()) {
      return false;
    }
    for (std::size_t start = 0; start < words.size(); ++start) {
      std::string joined;
      for (std::size_t i = start; i < words.size() && joined.size() < glued.size(); ++i) {
        joined += words[i];
      }
      if (joined == glued) {
        return true;
      }
    }
    return false;
  }

  bool isConnector(std::string_view token) {
    constexpr std::array<std::string_view, 10> connectors = {
        "x", "feat", "ft", "featuring", "mit", "with", "vs", "prod", "and", "und",
    };
    return std::ranges::contains(connectors, token);
  }

  bool isNoise(std::string_view token) {
    constexpr std::array<std::string_view, 19> exact = {
        "video", "audio", "4k",   "hd",         "hq",    "mv",    "clip", "performance", "explicit",  "version",
        "edit",  "live",  "prod", "musikvideo", "music", "topic", "feat", "ft",          "featuring",
    };
    constexpr std::array<std::string_view, 5> prefixes = {"official", "offiziel", "remaster", "visuali", "lyric"};
    return std::ranges::contains(exact, token)
        || std::ranges::any_of(prefixes, [token](std::string_view prefix) { return token.starts_with(prefix); })
        || std::ranges::all_of(token, [](char c) { return c >= '0' && c <= '9'; });
  }

  // Strips `VEVO`, ` - Topic` and `Official` decorations that video platforms add to channel names.
  Tokens artistTokens(std::string_view artist) {
    Tokens result;
    for (auto& token : tokens(artist)) {
      if (token.size() > 4 && token.ends_with("vevo")) {
        token.resize(token.size() - 4);
      }
      if (token != "vevo" && token != "topic" && token != "official") {
        result.push_back(std::move(token));
      }
    }
    return result;
  }

  // Individually credited performers, e.g. "Swiss & Die Andern, Diggen" -> {swiss}, {die andern}, {diggen}.
  std::vector<Tokens> artistParts(std::string_view artist) {
    std::vector<Tokens> parts(1);
    for (auto& token : scan(artist)) {
      if (token.text.size() > 4 && token.text.ends_with("vevo")) {
        token.text.resize(token.text.size() - 4);
      }
      if (token.text == "vevo" || token.text == "topic" || token.text == "official") {
        continue;
      }
      const bool connector = isConnector(token.text);
      if ((token.separatedBefore || connector) && !parts.back().empty()) {
        parts.emplace_back();
      }
      if (!connector) {
        parts.back().push_back(std::move(token.text));
      }
    }
    if (parts.back().empty()) {
      parts.pop_back();
    }
    return parts;
  }

  struct CleanTitle {
    std::string text;
    bool namesArtist = false;
  };

  bool noiseGroup(std::string_view text, bool requireAll) {
    const auto words = tokens(text);
    if (words.empty()) {
      return false;
    }
    return requireAll ? std::ranges::all_of(words, isNoise) : std::ranges::any_of(words, isNoise);
  }

  // Removes "(Official Video)"-style groups and " - Remastered 2011"-style suffixes.
  CleanTitle cleanTitle(std::string_view title) {
    std::string withoutGroups;
    for (std::size_t i = 0; i < title.size();) {
      const char open = title[i];
      const char close = open == '(' ? ')' : open == '[' ? ']' : open == '{' ? '}' : '\0';
      if (close == '\0') {
        withoutGroups.push_back(open);
        ++i;
        continue;
      }
      const auto end = title.find(close, i + 1);
      const auto content = title.substr(i + 1, end == std::string_view::npos ? std::string_view::npos : end - i - 1);
      withoutGroups.push_back(' ');
      if (!noiseGroup(content, false)) {
        withoutGroups.append(content);
        withoutGroups.push_back(' ');
      }
      i = end == std::string_view::npos ? title.size() : end + 1;
    }

    CleanTitle result;
    std::size_t kept = 0;
    std::string_view rest = withoutGroups;
    constexpr std::array<std::string_view, 5> separators = {" - ", " \u2013 ", " \u2014 ", " | ", " // "};
    while (!rest.empty()) {
      auto cut = std::string_view::npos;
      std::size_t width = 0;
      for (const auto separator : separators) {
        if (const auto at = rest.find(separator); at < cut) {
          cut = at;
          width = separator.size();
        }
      }
      const auto segment = rest.substr(0, cut);
      rest = cut == std::string_view::npos ? std::string_view{} : rest.substr(cut + width);
      if (tokens(segment).empty() || (kept > 0 && noiseGroup(segment, true))) {
        continue;
      }
      ++kept;
      const auto words = tokens(segment);
      for (std::size_t i = 0; i < words.size(); ++i) {
        const auto& token = words[i];
        const bool between = i > 0 && i + 1 < words.size();
        if (token == "x" && between) {
          result.namesArtist = true;
        }
        if (isConnector(token) && (token != "x" || between)) {
          continue;
        }
        if (!result.text.empty()) {
          result.text.push_back(' ');
        }
        result.text += token;
      }
    }
    result.namesArtist = result.namesArtist || kept > 1;
    return result;
  }

  // The title with bracketed qualifiers and dash suffixes removed, e.g. "Song - Remastered 2011" -> "Song".
  Tokens baseTitle(std::string_view title) {
    for (const auto marker :
         {std::string_view{"("}, std::string_view{"["}, std::string_view{" - "}, std::string_view{" \u2013 "}}) {
      if (const auto at = title.find(marker); at != std::string_view::npos && at > 0) {
        title = title.substr(0, at);
      }
    }
    return tokens(title);
  }

  bool validIsrc(std::string_view isrc) {
    if (isrc.size() != 12) {
      return false;
    }
    for (std::size_t i = 0; i < isrc.size(); ++i) {
      const char c = isrc[i];
      const bool letter = c >= 'A' && c <= 'Z';
      const bool digit = c >= '0' && c <= '9';
      if ((i < 2 && !letter) || (i >= 2 && i < 5 && !letter && !digit) || (i >= 5 && !digit)) {
        return false;
      }
    }
    return true;
  }

  struct Candidate {
    LyricsCatalogMatch match;
    LyricsTrackScore score;
    int catalogScore = 0;
  };

  std::optional<Candidate> candidate(LyricsCatalogMatch match, const LyricsTrack& raw, int catalogScore) {
    auto score = scoreLyricsTrack(raw, match.track);
    if (!score) {
      return std::nullopt;
    }
    return Candidate{.match = std::move(match), .score = *score, .catalogScore = catalogScore};
  }

  // Equally strong candidates share title, performer and timing, so the catalog's own ranking breaks the tie.
  std::optional<LyricsCatalogMatch> choose(std::vector<Candidate> candidates) {
    if (candidates.empty()) {
      return std::nullopt;
    }
    std::ranges::stable_sort(candidates, [](const Candidate& left, const Candidate& right) {
      if (strongerLyricsTrack(left.score, right.score) || strongerLyricsTrack(right.score, left.score)) {
        return strongerLyricsTrack(left.score, right.score);
      }
      if (left.match.isrc.empty() != right.match.isrc.empty()) {
        return !left.match.isrc.empty();
      }
      return left.catalogScore > right.catalogScore;
    });
    return std::move(candidates.front().match);
  }

  std::string escapeQuery(std::string_view input) {
    constexpr std::string_view reserved = "+-&|!(){}[]^\"~*?:\\/";
    std::string result;
    result.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
      const char c = input[i];
      const bool wordStart = i == 0 || g_ascii_isspace(input[i - 1]);
      if (wordStart) {
        const auto end = input.find_first_of(" \t\r\n", i);
        const auto word = input.substr(i, end == std::string_view::npos ? end : end - i);
        if (word == "AND" || word == "OR" || word == "NOT") {
          result.push_back('\\');
        }
      }
      if (reserved.contains(c)) {
        result.push_back('\\');
      }
      result.push_back(c);
    }
    return result;
  }
} // namespace

std::string lyricsSearchText(const LyricsTrack& raw) {
  auto [text, namesArtist] = cleanTitle(raw.title);
  const auto titleWords = tokens(raw.title);
  const auto parts = artistParts(raw.artist);
  const bool artistInTitle = std::ranges::any_of(parts, [&](const Tokens& part) { return contains(titleWords, part); })
      || spells(titleWords, compact(artistTokens(raw.artist)));
  if (!namesArtist && !artistInTitle) {
    for (const auto& part : parts) {
      for (const auto& word : part) {
        if (!text.empty()) {
          text.push_back(' ');
        }
        text += word;
      }
    }
  }
  return text;
}

bool lyricsTitleContinues(std::string_view previous, std::string_view current) {
  const auto before = tokens(previous);
  const auto after = tokens(current);
  if (before.empty() || after.empty()) {
    return false;
  }
  return before == after || (after.size() >= before.size() + kMinDecorationWords && contains(after, before));
}

std::optional<LyricsTrackScore> scoreLyricsTrack(const LyricsTrack& wanted, const LyricsTrack& candidate) {
  const auto wantedTitle = tokens(wanted.title);
  const auto actualTitle = tokens(candidate.title);
  LyricsTrackScore score;
  if (actualTitle.empty()) {
    return std::nullopt;
  }
  if (wantedTitle == actualTitle) {
    score.titleConfidence = 3;
  } else if (contains(wantedTitle, actualTitle)) {
    score.titleConfidence = 2;
  } else {
    const auto base = baseTitle(candidate.title);
    const bool alternateTake = std::ranges::any_of(actualTitle, [&](const std::string& word) {
      constexpr std::array<std::string_view, 6> takes = {"instrumental", "karaoke", "live",
                                                         "acoustic",     "remix",   "cover"};
      return std::ranges::contains(takes, word) && !std::ranges::contains(wantedTitle, word);
    });
    if (base.empty() || base == actualTitle || alternateTake || !contains(wantedTitle, base)) {
      return std::nullopt;
    }
    score.titleConfidence = 1;
    score.titleSpecificity = base.size();
  }
  if (score.titleSpecificity == 0) {
    score.titleSpecificity = actualTitle.size();
  }

  const auto wantedArtist = artistTokens(wanted.artist);
  const auto actualArtist = artistTokens(candidate.artist);
  if (contains(wantedArtist, actualArtist)
      || (!actualArtist.empty() && compact(wantedArtist) == compact(actualArtist))) {
    score.artistConfidence = 3;
    score.artistSpecificity = actualArtist.size();
  } else {
    for (const auto& part : artistParts(candidate.artist)) {
      const int confidence = contains(wantedArtist, part) ? 2 : contains(wantedTitle, part) ? 1 : 0;
      if (confidence > score.artistConfidence
          || (confidence == score.artistConfidence && part.size() > score.artistSpecificity)) {
        score.artistConfidence = confidence;
        score.artistSpecificity = part.size();
      }
    }
    if (score.artistConfidence == 0) {
      return std::nullopt;
    }
  }

  // A match must account for the whole player title, so a long scraped page title cannot match an unrelated song
  // whose title and artist merely occur inside it.
  Tokens unexplained;
  for (const auto& word : wantedTitle) {
    const bool explained = std::ranges::contains(actualTitle, word)
        || std::ranges::contains(actualArtist, word)
        || std::ranges::contains(wantedArtist, word)
        || std::ranges::any_of(artistParts(candidate.artist),
                               [&](const Tokens& part) { return std::ranges::contains(part, word); })
        || isConnector(word)
        || isNoise(word);
    if (!explained && !std::ranges::contains(unexplained, word)) {
      unexplained.push_back(word);
    }
  }
  if (unexplained.size() > kMaxUnexplainedTitleWords) {
    return std::nullopt;
  }

  if (wanted.durationUs > 0 && candidate.durationUs > 0) {
    score.durationDifferenceMs =
        std::abs(static_cast<double>(candidate.durationUs) / 1000.0 - static_cast<double>(wanted.durationUs) / 1000.0);
    if (score.durationDifferenceMs > kLooseDurationMs) {
      return std::nullopt;
    }
    score.durationConfidence = score.durationDifferenceMs <= kExactDurationMs ? 2 : 1;
  }
  return score;
}

bool strongerLyricsTrack(const LyricsTrackScore& left, const LyricsTrackScore& right) {
  return std::tuple{left.titleConfidence,  left.titleSpecificity,  left.durationConfidence,
                    left.artistConfidence, left.artistSpecificity, -left.durationDifferenceMs}
  > std::tuple{right.titleConfidence,  right.titleSpecificity,  right.durationConfidence,
               right.artistConfidence, right.artistSpecificity, -right.durationDifferenceMs};
}

std::string lyricsCatalogQuery(const LyricsTrack& raw) {
  const auto title = escapeQuery(raw.title);
  auto artists = title;
  if (!raw.artist.empty() && !contains(tokens(raw.title), tokens(raw.artist))) {
    artists += ' ';
    artists += escapeQuery(raw.artist);
  }
  auto query = std::format("recording:({}) AND artistname:({})", title, artists);
  if (raw.durationUs > 0) {
    constexpr auto toleranceMs = static_cast<std::int64_t>(kLooseDurationMs);
    const auto durationMs = raw.durationUs / 1000;
    const auto upperMs = durationMs + (raw.durationUs % 1000 != 0 ? 1 : 0);
    query +=
        std::format(" AND dur:[{} TO {}]", std::max<std::int64_t>(0, durationMs - toleranceMs), upperMs + toleranceMs);
  }
  return query;
}

std::expected<std::optional<LyricsCatalogMatch>, std::string>
matchLyricsRecording(std::string_view body, const LyricsTrack& raw) {
  try {
    const auto json = nlohmann::json::parse(body);
    const auto& recordings = json.at("recordings");
    if (!recordings.is_array()) {
      return std::unexpected("catalog recordings must be an array");
    }
    std::vector<Candidate> candidates;
    for (const auto& recording : recordings) {
      if (!recording.is_object()) {
        return std::unexpected("catalog recording must be an object");
      }
      const auto title = recording.at("title").get<std::string>();
      const auto& credits = recording.at("artist-credit");
      if (!credits.is_array()) {
        return std::unexpected("catalog artist credit must be an array");
      }
      std::int64_t milliseconds = 0;
      if (const auto length = recording.find("length"); length != recording.end() && length->is_number_integer()) {
        milliseconds = length->get<std::int64_t>();
        if (milliseconds < 0 || milliseconds > std::numeric_limits<std::int64_t>::max() / 1000) {
          milliseconds = 0;
        }
      }
      std::string isrc;
      if (const auto isrcs = recording.find("isrcs"); isrcs != recording.end()) {
        if (!isrcs->is_array()) {
          return std::unexpected("catalog ISRCs must be an array");
        }
        for (const auto& value : *isrcs) {
          const auto text = value.get<std::string>();
          if (validIsrc(text) && (isrc.empty() || text < isrc)) {
            isrc = text;
          }
        }
      }
      // Each artist credit is an independently attributed performer, not an uploader alias.
      for (const auto& credit : credits) {
        const auto artist = credit.at("artist").at("name").get<std::string>();
        LyricsCatalogMatch match{
            .track = {.title = title, .artist = artist, .durationUs = milliseconds * 1000},
            .isrc = isrc,
        };
        if (auto value = candidate(std::move(match), raw, recording.value("score", 0))) {
          candidates.push_back(std::move(*value));
        }
      }
    }
    return choose(std::move(candidates));
  } catch (const nlohmann::json::exception& error) {
    return std::unexpected(error.what());
  }
}

std::expected<std::optional<LyricsCatalogMatch>, std::string>
matchDeezerRecording(std::string_view body, const LyricsTrack& raw) {
  try {
    const auto json = nlohmann::json::parse(body);
    const auto& recordings = json.at("data");
    if (!recordings.is_array()) {
      return std::unexpected("catalog data must be an array");
    }
    std::vector<Candidate> candidates;
    for (const auto& recording : recordings) {
      const auto id = recording.at("id").get<std::int64_t>();
      const auto seconds = recording.at("duration").get<std::int64_t>();
      if (id <= 0 || seconds <= 0 || seconds > std::numeric_limits<std::int64_t>::max() / 1000000) {
        continue;
      }
      LyricsCatalogMatch match{
          .track =
              {
                  .title = recording.at("title").get<std::string>(),
                  .artist = recording.at("artist").at("name").get<std::string>(),
                  .album = recording.at("album").at("title").get<std::string>(),
                  .durationUs = seconds * 1000000,
              },
          .deezerId = static_cast<std::uint64_t>(id),
      };
      if (const auto value = recording.find("isrc"); value != recording.end() && value->is_string()) {
        const auto isrc = value->get<std::string>();
        if (validIsrc(isrc)) {
          match.isrc = isrc;
        }
      }
      if (auto value = candidate(std::move(match), raw, 0)) {
        candidates.push_back(std::move(*value));
      }
    }
    return choose(std::move(candidates));
  } catch (const nlohmann::json::exception& error) {
    return std::unexpected(error.what());
  }
}

std::expected<std::optional<LyricsCatalogMatch>, std::string>
matchNeteaseSong(std::string_view body, const LyricsTrack& raw) {
  try {
    const auto json = nlohmann::json::parse(body);
    const auto result = json.find("result");
    if (result == json.end() || !result->is_object()) {
      return std::nullopt;
    }
    const auto songs = result->find("songs");
    if (songs == result->end()) {
      return std::nullopt;
    }
    if (!songs->is_array()) {
      return std::unexpected("catalog songs must be an array");
    }
    std::vector<Candidate> candidates;
    for (const auto& song : *songs) {
      const auto id = song.at("id").get<std::int64_t>();
      const auto milliseconds = song.at("duration").get<std::int64_t>();
      if (id <= 0 || milliseconds <= 0 || milliseconds > std::numeric_limits<std::int64_t>::max() / 1000) {
        continue;
      }
      std::string artists;
      for (const auto& artist : song.at("artists")) {
        if (!artists.empty()) {
          artists += ", ";
        }
        artists += artist.at("name").get<std::string>();
      }
      LyricsCatalogMatch match{
          .track = {.title = song.at("name").get<std::string>(), .artist = artists, .durationUs = milliseconds * 1000},
          .sourceId = std::to_string(id),
      };
      if (auto value = candidate(std::move(match), raw, 0)) {
        candidates.push_back(std::move(*value));
      }
    }
    return choose(std::move(candidates));
  } catch (const nlohmann::json::exception& error) {
    return std::unexpected(error.what());
  }
}

std::expected<std::optional<LyricsCatalogMatch>, std::string>
matchKugouSong(std::string_view body, const LyricsTrack& raw) {
  try {
    const auto json = nlohmann::json::parse(body);
    const auto data = json.find("data");
    if (data == json.end() || !data->is_object() || !data->contains("info")) {
      return std::nullopt;
    }
    const auto& songs = data->at("info");
    if (!songs.is_array()) {
      return std::unexpected("catalog songs must be an array");
    }
    std::vector<Candidate> candidates;
    for (const auto& song : songs) {
      const auto hash = song.value("hash", std::string{});
      const auto seconds = song.value("duration", std::int64_t{0});
      if (hash.empty() || seconds <= 0 || seconds > std::numeric_limits<std::int64_t>::max() / 1000000) {
        continue;
      }
      LyricsCatalogMatch match{
          .track =
              {
                  .title = song.value("songname", std::string{}),
                  .artist = song.value("singername", std::string{}),
                  .durationUs = seconds * 1000000,
              },
          .sourceId = hash,
      };
      if (auto value = candidate(std::move(match), raw, 0)) {
        candidates.push_back(std::move(*value));
      }
    }
    return choose(std::move(candidates));
  } catch (const nlohmann::json::exception& error) {
    return std::unexpected(error.what());
  }
}

std::expected<std::optional<KugouLyricsCandidate>, std::string> pickKugouLyrics(std::string_view body) {
  try {
    const auto json = nlohmann::json::parse(body);
    const auto candidates = json.find("candidates");
    if (candidates == json.end() || candidates->is_null()) {
      return std::nullopt;
    }
    if (!candidates->is_array()) {
      return std::unexpected("lyrics candidates must be an array");
    }
    for (const auto& entry : *candidates) {
      KugouLyricsCandidate candidate{
          .id = entry.value("id", std::string{}),
          .accessKey = entry.value("accesskey", std::string{}),
          .durationMs = entry.value("duration", std::int64_t{0}),
      };
      if (!candidate.id.empty() && !candidate.accessKey.empty()) {
        return candidate;
      }
    }
    return std::nullopt;
  } catch (const nlohmann::json::exception& error) {
    return std::unexpected(error.what());
  }
}
