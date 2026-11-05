#include "system/lyrics_catalog.h"
#include "test_check.h"

#include <nlohmann/json.hpp>

namespace {
  nlohmann::json musicbrainz(
      std::string_view title, std::string_view artist, std::int64_t length, std::string_view isrc, int score = 90
  ) {
    return {
        {"title", title},   {"artist-credit", nlohmann::json::array({{{"artist", {{"name", artist}}}}})},
        {"length", length}, {"isrcs", nlohmann::json::array({isrc})},
        {"score", score},
    };
  }

  nlohmann::json deezer(std::uint64_t id, std::string_view title, std::string_view artist, std::int64_t duration) {
    return {
        {"id", id},
        {"title", title},
        {"artist", {{"name", artist}}},
        {"album", {{"title", "Album"}}},
        {"duration", duration},
    };
  }
} // namespace

int main() {
  const LyricsTrack swiss{
      .title = "SWISS + DIE ANDERN mit MOONKID - NETT WERDEN (Official Video 4K)",
      .artist = "MISSGLÜCKTE WELT",
      .durationUs = 162061000,
  };
  auto records = nlohmann::json::array({
      musicbrainz("Nett werden (Live)", "Swiss + Die Andern", 162000, "DEA612500245", 100),
      musicbrainz("Nett werden", "Cover Artist", 162000, "DEA612500246", 100),
      musicbrainz("Nett werden", "Swiss + Die Andern", 175000, "DEA612500247", 100),
      musicbrainz("Nett werden", "Swiss + Die Andern", 161093, "DEA612500244", 88),
  });
  auto match = matchLyricsRecording(nlohmann::json{{"recordings", records}}.dump(), swiss);
  TEST_CHECK(match);
  TEST_CHECK(match->has_value());
  TEST_CHECK((*match)->track.title == "Nett werden");
  TEST_CHECK((*match)->track.artist == "Swiss + Die Andern");
  TEST_CHECK((*match)->track.durationUs == 161093000);
  TEST_CHECK((*match)->isrc == "DEA612500244");
  TEST_CHECK(swiss.title == "SWISS + DIE ANDERN mit MOONKID - NETT WERDEN (Official Video 4K)");

  // Equally strong recordings fall back to the catalog ranking.
  records.push_back(musicbrainz("Nett werden", "Swiss + Die Andern", 161093, "DEA612500248", 99));
  match = matchLyricsRecording(nlohmann::json{{"recordings", records}}.dump(), swiss);
  TEST_CHECK(match);
  TEST_CHECK(match->has_value());
  TEST_CHECK((*match)->isrc == "DEA612500248");

  // Collaborators packed into a video title, an uploader channel as artist and a longer video cut.
  const LyricsTrack collaboration{
      .title = "SWISS x DIGGEN x DIE ANDERN x NOCH NICHT KAPUTT (Official Video 4K)",
      .artist = "MISSGLÜ"
                "CKTE WELT",
      .durationUs = 268421000,
  };
  TEST_CHECK(lyricsSearchText(collaboration) == "swiss diggen die andern noch nicht kaputt");
  const auto collaborationCandidates = nlohmann::json::array({
      deezer(2131294177, "Mach dich nicht kaputt", "Gustav", 150),
      deezer(1492436372, "Noch nicht kaputt", "Swiss & Die Andern", 254),
  });
  auto collaborationMatch =
      matchDeezerRecording(nlohmann::json{{"data", collaborationCandidates}}.dump(), collaboration);
  TEST_CHECK(collaborationMatch);
  TEST_CHECK(collaborationMatch->has_value());
  TEST_CHECK((*collaborationMatch)->deezerId == 1492436372);
  // A featured artist the catalog does not list, even when the player title repeats it, still matches.
  const LyricsTrack starboy{
      .title = "The Weeknd - Starboy ft. Daft Punk (Official Video) ft. Daft Punk",
      .artist = "TheWeekndVEVO",
  };
  TEST_CHECK(lyricsSearchText(starboy) == "the weeknd starboy daft punk daft punk");
  const auto starboyMatch = matchDeezerRecording(
      nlohmann::json{{"data", nlohmann::json::array({deezer(124, "Starboy", "The Weeknd", 230)})}}.dump(), starboy
  );
  TEST_CHECK(starboyMatch);
  TEST_CHECK(starboyMatch->has_value());

  // A scraped page title must not match an unrelated song whose title and artist occur inside it.
  const LyricsTrack scraped{
      .title = "DE Skip navigation Search Create Avatar image Tokyo Drift (Fast & Furious)",
      .artist = "Someone",
      .durationUs = 240000000,
  };
  const auto scrapedCandidates = nlohmann::json::array({deezer(1, "Create", "Avatar", 240)});
  const auto scrapedMatch = matchDeezerRecording(nlohmann::json{{"data", scrapedCandidates}}.dump(), scraped);
  TEST_CHECK(scrapedMatch);
  TEST_CHECK(!scrapedMatch->has_value());
  // A scraped page title wrapping the real one is the same track; a different or merely longer song is not.
  TEST_CHECK(lyricsTitleContinues("Tokyo Drift (Fast & Furious)", "Tokyo Drift (Fast & Furious)"));
  TEST_CHECK(lyricsTitleContinues(
      "Tokyo Drift (Fast & Furious)", "DE Skip navigation Search Create Avatar image Tokyo Drift (Fast & Furious)"
  ));
  TEST_CHECK(!lyricsTitleContinues("Love", "Love Story"));
  TEST_CHECK(!lyricsTitleContinues("Riders on the Storm", "Tokyo Drift (Fast & Furious)"));
  TEST_CHECK(!lyricsTitleContinues("", "Song"));
  TEST_CHECK(lyricsSearchText({.title = "Song (Lyric Video)", .artist = "Artist - Topic"}) == "song artist");
  TEST_CHECK(
      lyricsSearchText(
          {.title = "Outline In Color \"Another Nightmare\" (Official Lyric Video)", .artist = "OutlineInColor"}
      )
      == "outline in color another nightmare"
  );
  TEST_CHECK(lyricsSearchText({.title = "Eminem - Rap God (Explicit)", .artist = "EminemVEVO"}) == "eminem rap god");
  TEST_CHECK(lyricsSearchText({.title = "Malcolm X", .artist = "Band"}) == "malcolm x band");

  const LyricsTrack indic{.title = "कला", .artist = "गायक", .durationUs = 100000000};
  const auto differentIndicTitle = musicbrainz("कली", "गायक", 100000, "DEABC2600001");
  match = matchLyricsRecording(nlohmann::json{{"recordings", {differentIndicTitle}}}.dump(), indic);
  TEST_CHECK(match);
  TEST_CHECK(!match->has_value());
  const auto sameIndicTitle = musicbrainz("कला", "गायक", 100000, "DEABC2600001");
  match = matchLyricsRecording(nlohmann::json{{"recordings", {sameIndicTitle}}}.dump(), indic);
  TEST_CHECK(match);
  TEST_CHECK(match->has_value());

  const LyricsTrack unicode{.title = "Песня — ЁЛКА", .artist = "ИСПОЛНИТЕЛЬ", .durationUs = 100000000};
  const auto unicodeRecord = musicbrainz("Песня – Ёлка", "исполнитель", 100000, "DEABC2600001");
  match = matchLyricsRecording(nlohmann::json{{"recordings", {unicodeRecord}}}.dump(), unicode);
  TEST_CHECK(match);
  TEST_CHECK(match->has_value());
  const LyricsTrack composed{.title = "Café", .artist = "Björk", .durationUs = 100000000};
  const auto decomposed = musicbrainz("Cafe\u0301", "BJO\u0308RK", 100000, "DEABC2600001");
  match = matchLyricsRecording(nlohmann::json{{"recordings", {decomposed}}}.dump(), composed);
  TEST_CHECK(match);
  TEST_CHECK(match->has_value());

  const LyricsTrack rio{.title = "Rio Riseup – Maul auf", .artist = "Rio Riseup", .durationUs = 226977959};
  const auto rioCandidates = nlohmann::json::array({
      deezer(99, "Maul auf", "Cover Artist", 226),
      deezer(2560075282, "Maul auf", "Rio Riseup", 226),
  });
  auto deezerMatch = matchDeezerRecording(nlohmann::json{{"data", rioCandidates}}.dump(), rio);
  TEST_CHECK(deezerMatch);
  TEST_CHECK(deezerMatch->has_value());
  TEST_CHECK((*deezerMatch)->deezerId == 2560075282);
  TEST_CHECK((*deezerMatch)->track.title == "Maul auf");
  TEST_CHECK((*deezerMatch)->track.artist == "Rio Riseup");
  TEST_CHECK((*deezerMatch)->isrc.empty());

  const LyricsTrack antifuchs{
      .title = "Antifuchs x 257ers - Was muss, das muss",
      .artist = "Antifuchs",
      .durationUs = 193000000,
  };
  const auto antifuchsCandidates = nlohmann::json::array({
      deezer(3092789431, "Was muss, das muss", "Dämmerland", 156),
      deezer(4232755642, "Was muss, das muss", "Antifuchs", 193),
  });
  deezerMatch = matchDeezerRecording(nlohmann::json{{"data", antifuchsCandidates}}.dump(), antifuchs);
  TEST_CHECK(deezerMatch);
  TEST_CHECK(deezerMatch->has_value());
  TEST_CHECK((*deezerMatch)->deezerId == 4232755642);

  const LyricsTrack beartooth{
      .title = "Beartooth - Look the Other Way (Visualizer)",
      .artist = "BEARTOOTHband",
      .durationUs = 187301000,
  };
  const auto beartoothRecords = nlohmann::json::array({
      musicbrainz("Beartooth", "Johnny Bush", 187000, "USABC2300001", 100),
      musicbrainz("Look the Other Way (ambient mix)", "Beartooth", 184695, "USP6L2402676", 90),
      musicbrainz("Look the Other Way", "Beartooth", 187266, "USP6L2301698", 100),
  });
  match = matchLyricsRecording(nlohmann::json{{"recordings", beartoothRecords}}.dump(), beartooth);
  TEST_CHECK(match);
  TEST_CHECK(match->has_value());
  TEST_CHECK((*match)->isrc == "USP6L2301698");
  const LyricsTrack nativeBeartooth{.title = "Look the Other Way", .artist = "Beartooth", .durationUs = 187301000};
  match = matchLyricsRecording(nlohmann::json{{"recordings", beartoothRecords}}.dump(), nativeBeartooth);
  TEST_CHECK(match);
  TEST_CHECK(match->has_value());
  TEST_CHECK((*match)->isrc == "USP6L2301698");
  // Both name fields and duration constrain candidates before the catalog's result limit.
  const auto beartoothQuery = lyricsCatalogQuery(beartooth);
  TEST_CHECK(beartoothQuery.starts_with("recording:("));
  TEST_CHECK(beartoothQuery.contains(" AND artistname:("));
  TEST_CHECK(beartoothQuery.ends_with(" AND dur:[157301 TO 217301]"));

  const LyricsTrack qualifiedVideo{
      .title = "Artist - Song (Live) (Official Video)",
      .artist = "Uploader",
      .durationUs = 100000000,
  };
  const auto qualifiedRecords = nlohmann::json::array({
      musicbrainz("Song", "Artist", 100000, "DEABC2600001"),
      musicbrainz("Song (Live)", "Artist", 101000, "DEABC2600002"),
  });
  match = matchLyricsRecording(nlohmann::json{{"recordings", qualifiedRecords}}.dump(), qualifiedVideo);
  TEST_CHECK(match);
  TEST_CHECK(match->has_value());
  TEST_CHECK((*match)->track.title == "Song (Live)");
  TEST_CHECK((*match)->isrc == "DEABC2600002");

  const auto empty = matchLyricsRecording(R"({"recordings":[]})", swiss);
  TEST_CHECK(empty);
  TEST_CHECK(!empty->has_value());
  const auto unknown = matchLyricsRecording(
      nlohmann::json{{"recordings", {musicbrainz("Nett werden", "Someone Else", 162000, "DEA612500244")}}}.dump(), swiss
  );
  TEST_CHECK(unknown);
  TEST_CHECK(!unknown->has_value());
  const auto invalidIsrc = matchLyricsRecording(
      nlohmann::json{{"recordings", {musicbrainz("Nett werden", "Swiss + Die Andern", 162000, "not-an-isrc")}}}.dump(),
      swiss
  );
  TEST_CHECK(invalidIsrc);
  TEST_CHECK(invalidIsrc->has_value());
  TEST_CHECK((*invalidIsrc)->isrc.empty());
  TEST_CHECK(!matchLyricsRecording("not json", swiss));
  TEST_CHECK(!matchLyricsRecording(R"({"recordings":{}})", swiss));
  TEST_CHECK(!matchLyricsRecording(R"({"recordings":[{}]})", swiss));
  TEST_CHECK(!matchDeezerRecording(R"({"data":null})", swiss));

  const auto neteaseSong = [](std::int64_t id, std::string_view title, std::string_view artist, std::int64_t duration) {
    return nlohmann::json{
        {"id", id},
        {"name", title},
        {"artists", nlohmann::json::array({{{"name", artist}}})},
        {"duration", duration},
    };
  };
  const LyricsTrack witt{
      .title = "Witt Lowry - My Mistake (feat. Trippz Michaud)", .artist = "Witt Lowry", .durationUs = 265000000
  };
  const auto neteaseBody = nlohmann::json{
      {"result",
       {{"songs",
         nlohmann::json::array({
             neteaseSong(3457408481, "My Mistake", "Vi More", 261024),
             neteaseSong(35023080, "My Mistake (feat. Trippz Michaud)", "Witt Lowry", 264375),
         })}}}
  };
  const auto neteaseMatch = matchNeteaseSong(neteaseBody.dump(), witt);
  TEST_CHECK(neteaseMatch);
  TEST_CHECK(neteaseMatch->has_value());
  TEST_CHECK((*neteaseMatch)->sourceId == "35023080");
  TEST_CHECK((*neteaseMatch)->track.durationUs == 264375000);

  const auto kugouBody = nlohmann::json{
      {"data",
       {{"info",
         nlohmann::json::array({
             {{"songname", "My Mistake (feat. Trippz Michaud)(Explicit)"},
              {"singername", "Witt Lowry、Trippz Michaud"},
              {"duration", 264},
              {"hash", "ABC"}},
             {{"songname", "Into Your Arms"}, {"singername", "Witt Lowry"}, {"duration", 186}, {"hash", "DEF"}},
         })}}}
  };
  const auto kugouMatch = matchKugouSong(kugouBody.dump(), witt);
  TEST_CHECK(kugouMatch);
  TEST_CHECK(kugouMatch->has_value());
  TEST_CHECK((*kugouMatch)->sourceId == "ABC");
  TEST_CHECK((*kugouMatch)->track.durationUs == 264000000);
  const auto kugouLyrics = pickKugouLyrics(
      R"({"candidates":[{"id":"","accesskey":"x"},{"id":"36340823","accesskey":"E17F","duration":264441}]})"
  );
  TEST_CHECK(kugouLyrics);
  TEST_CHECK(kugouLyrics->has_value());
  TEST_CHECK((*kugouLyrics)->id == "36340823");
  TEST_CHECK((*kugouLyrics)->durationMs == 264441);
  TEST_CHECK(!pickKugouLyrics(R"({"candidates":[]})")->has_value());
  TEST_CHECK(!matchNeteaseSong(nlohmann::json{{"result", nlohmann::json::object()}}.dump(), witt)->has_value());
  TEST_CHECK(!matchNeteaseSong(
                  nlohmann::json{
                      {"result", {{"songs", nlohmann::json::array({neteaseSong(1, "Create", "Avatar", 265000)})}}}
                  }.dump(),
                  scraped
  )
                  ->has_value());
  TEST_CHECK(!matchNeteaseSong("not json", witt));

  TEST_CHECK(
      lyricsCatalogQuery(rio)
      == "recording:(Rio Riseup – Maul auf) AND artistname:(Rio Riseup – Maul auf) AND dur:[196977 TO 256978]"
  );
  const auto escaped = lyricsCatalogQuery({.title = "Song: (Title) + Artist?", .artist = "Other"});
  TEST_CHECK(
      escaped == R"(recording:(Song\: \(Title\) \+ Artist\?) AND artistname:(Song\: \(Title\) \+ Artist\? Other))"
  );
  TEST_CHECK(
      lyricsCatalogQuery({.title = "AND OR NOT"}) == R"(recording:(\AND \OR \NOT) AND artistname:(\AND \OR \NOT))"
  );
  return 0;
}
