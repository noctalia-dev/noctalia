#pragma once

#include "system/lyrics.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

struct LyricsCatalogMatch {
  LyricsTrack track;
  std::string isrc;
  std::uint64_t deezerId = 0;
  // Song id at a lyrics-only source (NetEase id, KuGou hash).
  std::string sourceId;
};

struct KugouLyricsCandidate {
  std::string id;
  std::string accessKey;
  std::int64_t durationMs = 0;
};

// How closely a catalog/lyrics record matches the player's metadata; higher fields win in declaration order.
struct LyricsTrackScore {
  int titleConfidence = 0;
  std::size_t titleSpecificity = 0;
  int durationConfidence = 0;
  int artistConfidence = 0;
  std::size_t artistSpecificity = 0;
  double durationDifferenceMs = 0.0;
};

// Free-text search terms with video/channel noise, version suffixes and collaborator connectors removed.
std::string lyricsSearchText(const LyricsTrack& raw);
// Whether `current` is still the track titled `previous`: equal, or the same title wrapped in extra text such as a
// scraped page title.
bool lyricsTitleContinues(std::string_view previous, std::string_view current);
// Scores `candidate` against messy player metadata, or nullopt when it is not the same song.
std::optional<LyricsTrackScore> scoreLyricsTrack(const LyricsTrack& wanted, const LyricsTrack& candidate);
bool strongerLyricsTrack(const LyricsTrackScore& left, const LyricsTrackScore& right);

std::string lyricsCatalogQuery(const LyricsTrack& raw);
std::expected<std::optional<LyricsCatalogMatch>, std::string>
matchLyricsRecording(std::string_view body, const LyricsTrack& raw);
std::expected<std::optional<LyricsCatalogMatch>, std::string>
matchDeezerRecording(std::string_view body, const LyricsTrack& raw);
// The best match in a NetEase `search/get` response.
std::expected<std::optional<LyricsCatalogMatch>, std::string>
matchNeteaseSong(std::string_view body, const LyricsTrack& raw);
// The best match in a KuGou `api/v3/search/song` response.
std::expected<std::optional<LyricsCatalogMatch>, std::string>
matchKugouSong(std::string_view body, const LyricsTrack& raw);
// The first downloadable entry of a KuGou lyrics `search` response.
std::expected<std::optional<KugouLyricsCandidate>, std::string> pickKugouLyrics(std::string_view body);
