#include "theme/builtin_palettes.h"

#include "theme/fixed_palette.h"

#include <array>
#include <string_view>

namespace noctalia::theme {

  namespace {

    constexpr std::array<BuiltinPalette, 10> kPalettes =
        {
            {
                {
                    .name = "Ayu",
                    .dark =
                        FixedPaletteMode{
                            .palette =
                                Palette{
                                    .primary = hex("#E6B450"),
                                    .onPrimary = hex("#0B0E14"),
                                    .secondary = hex("#AAD94C"),
                                    .onSecondary = hex("#0B0E14"),
                                    .tertiary = hex("#39BAE6"),
                                    .onTertiary = hex("#0B0E14"),
                                    .error = hex("#D95757"),
                                    .onError = hex("#0B0E14"),
                                    .surface = hex("#0B0E14"),
                                    .onSurface = hex("#D1D1C7"),
                                    .surfaceVariant = hex("#1e222a"),
                                    .onSurfaceVariant = hex("#8E959E"),
                                    .outline = hex("#565B66"),
                                    .shadow = hex("#000000"),
                                    .hover = hex("#39BAE6"),
                                    .onHover = hex("#0B0E14"),
                                },
                            .terminal =
                                TerminalPalette{
                                    .normal =
                                        TerminalAnsiColors{
                                            .black = hex("#171b24"),
                                            .red = hex("#ed8274"),
                                            .green = hex("#87d96c"),
                                            .yellow = hex("#facc6e"),
                                            .blue = hex("#6dcbfa"),
                                            .magenta = hex("#dabafa"),
                                            .cyan = hex("#90e1c6"),
                                            .white = hex("#c7c7c7"),
                                        },
                                    .bright =
                                        TerminalAnsiColors{
                                            .black = hex("#686868"),
                                            .red = hex("#f28779"),
                                            .green = hex("#d5ff80"),
                                            .yellow = hex("#ffd173"),
                                            .blue = hex("#73d0ff"),
                                            .magenta = hex("#dfbfff"),
                                            .cyan = hex("#95e6cb"),
                                            .white = hex("#ffffff"),
                                        },
                                    .foreground = hex("#D1D1C7"),
                                    .background = hex("#1f2430"),
                                    .selectionFg = hex("#1f2430"),
                                    .selectionBg = hex("#409fff"),
                                    .cursorText = hex("#1f2430"),
                                    .cursor = hex("#ffcc66"),
                                },
                        },
                    .light =
                        FixedPaletteMode{
                            .palette =
                                Palette{
                                    .primary = hex("#FF8F40"),
                                    .onPrimary = hex("#F8F9FA"),
                                    .secondary = hex("#86B300"),
                                    .onSecondary = hex("#F8F9FA"),
                                    .tertiary = hex("#55B4D4"),
                                    .onTertiary = hex("#F8F9FA"),
                                    .error = hex("#E65050"),
                                    .onError = hex("#F8F9FA"),
                                    .surface = hex("#F8F9FA"),
                                    .onSurface = hex("#42474C"),
                                    .surfaceVariant = hex("#E4E6E9"),
                                    .onSurfaceVariant = hex("#6E757C"),
                                    .outline = hex("#8A9199"),
                                    .shadow = hex("#F8F9FA"),
                                    .hover = hex("#55B4D4"),
                                    .onHover = hex("#F8F9FA"),
                                },
                            .terminal =
                                TerminalPalette{
                                    .normal =
                                        TerminalAnsiColors{
                                            .black = hex("#000000"),
                                            .red = hex("#ea6c6d"),
                                            .green = hex("#6cbf43"),
                                            .yellow = hex("#eca944"),
                                            .blue = hex("#3199e1"),
                                            .magenta = hex("#9e75c7"),
                                            .cyan = hex("#46ba94"),
                                            .white = hex("#bababa"),
                                        },
                                    .bright =
                                        TerminalAnsiColors{
                                            .black = hex("#686868"),
                                            .red = hex("#f07171"),
                                            .green = hex("#86b300"),
                                            .yellow = hex("#f2ae49"),
                                            .blue = hex("#399ee6"),
                                            .magenta = hex("#a37acc"),
                                            .cyan = hex("#4cbf99"),
                                            .white = hex("#d1d1d1"),
                                        },
                                    .foreground = hex("#42474C"),
                                    .background = hex("#f8f9fa"),
                                    .selectionFg = hex("#f8f9fa"),
                                    .selectionBg = hex("#035bd6"),
                                    .cursorText = hex("#f8f9fa"),
                                    .cursor = hex("#ffaa33"),
                                },
                        },
                },
                {
                    .name = "Catppuccin",
                    .dark =
                        FixedPaletteMode{
                            .palette =
                                Palette{
                                    .primary = hex("#cba6f7"),
                                    .onPrimary = hex("#11111b"),
                                    .secondary = hex("#fab387"),
                                    .onSecondary = hex("#11111b"),
                                    .tertiary = hex("#94e2d5"),
                                    .onTertiary = hex("#11111b"),
                                    .error = hex("#f38ba8"),
                                    .onError = hex("#11111b"),
                                    .surface = hex("#1e1e2e"),
                                    .onSurface = hex("#cdd6f4"),
                                    .surfaceVariant = hex("#313244"),
                                    .onSurfaceVariant = hex("#a3b4eb"),
                                    .outline = hex("#4c4f69"),
                                    .shadow = hex("#11111b"),
                                    .hover = hex("#94e2d5"),
                                    .onHover = hex("#11111b"),
                                },
                            .terminal =
                                TerminalPalette{
                                    .normal =
                                        TerminalAnsiColors{
                                            .black = hex("#45475a"),
                                            .red = hex("#f38ba8"),
                                            .green = hex("#a6e3a1"),
                                            .yellow = hex("#f9e2af"),
                                            .blue = hex("#89b4fa"),
                                            .magenta = hex("#f5c2e7"),
                                            .cyan = hex("#94e2d5"),
                                            .white = hex("#a6adc8"),
                                        },
                                    .bright =
                                        TerminalAnsiColors{
                                            .black = hex("#585b70"),
                                            .red = hex("#f37799"),
                                            .green = hex("#89d88b"),
                                            .yellow = hex("#ebd391"),
                                            .blue = hex("#74a8fc"),
                                            .magenta = hex("#f2aede"),
                                            .cyan = hex("#6bd7ca"),
                                            .white = hex("#bac2de"),
                                        },
                                    .foreground = hex("#cdd6f4"),
                                    .background = hex("#1e1e2e"),
                                    .selectionFg = hex("#cdd6f4"),
                                    .selectionBg = hex("#585b70"),
                                    .cursorText = hex("#1e1e2e"),
                                    .cursor = hex("#f5e0dc"),
                                },
                        },
                    .light =
                        FixedPaletteMode{
                            .palette =
                                Palette{
                                    .primary = hex("#8839ef"),
                                    .onPrimary = hex("#eff1f5"),
                                    .secondary = hex("#fe640b"),
                                    .onSecondary = hex("#eff1f5"),
                                    .tertiary = hex("#40a02b"),
                                    .onTertiary = hex("#eff1f5"),
                                    .error = hex("#d20f39"),
                                    .onError = hex("#dce0e8"),
                                    .surface = hex("#eff1f5"),
                                    .onSurface = hex("#4c4f69"),
                                    .surfaceVariant = hex("#ccd0da"),
                                    .onSurfaceVariant = hex("#6c6f85"),
                                    .outline = hex("#a5adcb"),
                                    .shadow = hex("#dce0e8"),
                                    .hover = hex("#40a02b"),
                                    .onHover = hex("#eff1f5"),
                                },
                            .terminal =
                                TerminalPalette{
                                    .normal =
                                        TerminalAnsiColors{
                                            .black = hex("#51576d"),
                                            .red = hex("#e78284"),
                                            .green = hex("#a6d189"),
                                            .yellow = hex("#e5c890"),
                                            .blue = hex("#8caaee"),
                                            .magenta = hex("#f4b8e4"),
                                            .cyan = hex("#81c8be"),
                                            .white = hex("#a5adce"),
                                        },
                                    .bright =
                                        TerminalAnsiColors{
                                            .black = hex("#626880"),
                                            .red = hex("#e67172"),
                                            .green = hex("#8ec772"),
                                            .yellow = hex("#d9ba73"),
                                            .blue = hex("#7b9ef0"),
                                            .magenta = hex("#f2a4db"),
                                            .cyan = hex("#5abfb5"),
                                            .white = hex("#b5bfe2"),
                                        },
                                    .foreground = hex("#c6d0f5"),
                                    .background = hex("#303446"),
                                    .selectionFg = hex("#c6d0f5"),
                                    .selectionBg = hex("#626880"),
                                    .cursorText = hex("#303446"),
                                    .cursor = hex("#f2d5cf"),
                                },
                        },
                },
                {
                    .name = "Dracula",
                    .dark =
                        FixedPaletteMode{
                            .palette =
                                Palette{
                                    .primary = hex("#bd93f9"),
                                    .onPrimary = hex("#282A36"),
                                    .secondary = hex("#ff79c6"),
                                    .onSecondary = hex("#4e1d32"),
                                    .tertiary = hex("#8be9fd"),
                                    .onTertiary = hex("#003543"),
                                    .error = hex("#FF5555"),
                                    .onError = hex("#282A36"),
                                    .surface = hex("#282A36"),
                                    .onSurface = hex("#F8F8F2"),
                                    .surfaceVariant = hex("#44475A"),
                                    .onSurfaceVariant = hex("#d6d8e0"),
                                    .outline = hex("#5a5e77"),
                                    .shadow = hex("#282A36"),
                                    .hover = hex("#8be9fd"),
                                    .onHover = hex("#003543"),
                                },
                            .terminal =
                                TerminalPalette{
                                    .normal =
                                        TerminalAnsiColors{
                                            .black = hex("#21222c"),
                                            .red = hex("#ff5555"),
                                            .green = hex("#50fa7b"),
                                            .yellow = hex("#f1fa8c"),
                                            .blue = hex("#bd93f9"),
                                            .magenta = hex("#ff79c6"),
                                            .cyan = hex("#8be9fd"),
                                            .white = hex("#f8f8f2"),
                                        },
                                    .bright =
                                        TerminalAnsiColors{
                                            .black = hex("#6272a4"),
                                            .red = hex("#ff6e6e"),
                                            .green = hex("#69ff94"),
                                            .yellow = hex("#ffffa5"),
                                            .blue = hex("#d6acff"),
                                            .magenta = hex("#ff92df"),
                                            .cyan = hex("#a4ffff"),
                                            .white = hex("#ffffff"),
                                        },
                                    .foreground = hex("#f8f8f2"),
                                    .background = hex("#282a36"),
                                    .selectionFg = hex("#ffffff"),
                                    .selectionBg = hex("#44475a"),
                                    .cursorText = hex("#282a36"),
                                    .cursor = hex("#f8f8f2"),
                                },
                        },
                    .light =
                        FixedPaletteMode{
                            .palette =
                                Palette{
                                    .primary = hex("#8332f4"),
                                    .onPrimary = hex("#ffffff"),
                                    .secondary = hex("#ff1399"),
                                    .onSecondary = hex("#ffffff"),
                                    .tertiary = hex("#0398b9"),
                                    .onTertiary = hex("#ffffff"),
                                    .error = hex("#FF5555"),
                                    .onError = hex("#282A36"),
                                    .surface = hex("#f8f8f2"),
                                    .onSurface = hex("#282a36"),
                                    .surfaceVariant = hex("#e6e6ea"),
                                    .onSurfaceVariant = hex("#44475a"),
                                    .outline = hex("#cacad3"),
                                    .shadow = hex("#d6d8e0"),
                                    .hover = hex("#0398b9"),
                                    .onHover = hex("#ffffff"),
                                },
                            .terminal =
                                TerminalPalette{
                                    .normal =
                                        TerminalAnsiColors{
                                            .black = hex("#f8f8f2"),
                                            .red = hex("#ff5555"),
                                            .green = hex("#50fa7b"),
                                            .yellow = hex("#f1fa8c"),
                                            .blue = hex("#bd93f9"),
                                            .magenta = hex("#ff79c6"),
                                            .cyan = hex("#8be9fd"),
                                            .white = hex("#282a36"),
                                        },
                                    .bright =
                                        TerminalAnsiColors{
                                            .black = hex("#6272a4"),
                                            .red = hex("#ff6e6e"),
                                            .green = hex("#69ff94"),
                                            .yellow = hex("#ffffa5"),
                                            .blue = hex("#d6acff"),
                                            .magenta = hex("#ff92df"),
                                            .cyan = hex("#a4ffff"),
                                            .white = hex("#000000"),
                                        },
                                    .foreground = hex("#282a36"),
                                    .background = hex("#ffffff"),
                                    .selectionFg = hex("#ffffff"),
                                    .selectionBg = hex("#6272a4"),
                                    .cursorText = hex("#ffffff"),
                                    .cursor = hex("#282a36"),
                                },
                        },
                },
                {
                    .name = "Eldritch",
                    .dark =
                        FixedPaletteMode{
                            .palette =
                                Palette{
                                    .primary = hex("#37f499"),
                                    .onPrimary = hex("#171928"),
                                    .secondary = hex("#04d1f9"),
                                    .onSecondary = hex("#171928"),
                                    .tertiary = hex("#a48cf2"),
                                    .onTertiary = hex("#171928"),
                                    .error = hex("#f16c75"),
                                    .onError = hex("#171928"),
                                    .surface = hex("#212337"),
                                    .onSurface = hex("#ebfafa"),
                                    .surfaceVariant = hex("#292e42"),
                                    .onSurfaceVariant = hex("#ABB4DA"),
                                    .outline = hex("#3b4261"),
                                    .shadow = hex("#414868"),
                                    .hover = hex("#a48cf2"),
                                    .onHover = hex("#171928"),
                                },
                            .terminal =
                                TerminalPalette{
                                    .normal =
                                        TerminalAnsiColors{
                                            .black = hex("#21222c"),
                                            .red = hex("#f9515d"),
                                            .green = hex("#37f499"),
                                            .yellow = hex("#e9f941"),
                                            .blue = hex("#9071f4"),
                                            .magenta = hex("#f265b5"),
                                            .cyan = hex("#04d1f9"),
                                            .white = hex("#ebfafa"),
                                        },
                                    .bright =
                                        TerminalAnsiColors{
                                            .black = hex("#7081d0"),
                                            .red = hex("#f16c75"),
                                            .green = hex("#69F8B3"),
                                            .yellow = hex("#f1fc79"),
                                            .blue = hex("#a48cf2"),
                                            .magenta = hex("#FD92CE"),
                                            .cyan = hex("#66e4fd"),
                                            .white = hex("#ffffff"),
                                        },
                                    .foreground = hex("#ebfafa"),
                                    .background = hex("#212337"),
                                    .selectionFg = hex("#ebfafa"),
                                    .selectionBg = hex("#bf4f8e"),
                                    .cursorText = hex("#37f499"),
                                    .cursor = hex("#f8f8f2"),
                                },
                        },
                    .light =
                        FixedPaletteMode{
                            .palette =
                                Palette{
                                    .primary = hex("#37f499"),
                                    .onPrimary = hex("#171928"),
                                    .secondary = hex("#04d1f9"),
                                    .onSecondary = hex("#171928"),
                                    .tertiary = hex("#a48cf2"),
                                    .onTertiary = hex("#171928"),
                                    .error = hex("#f16c75"),
                                    .onError = hex("#171928"),
                                    .surface = hex("#ffffff"),
                                    .onSurface = hex("#171928"),
                                    .surfaceVariant = hex("#f2f4f8"),
                                    .onSurfaceVariant = hex("#3b4261"),
                                    .outline = hex("#b0b6c3"),
                                    .shadow = hex("#e0e3e8"),
                                    .hover = hex("#a48cf2"),
                                    .onHover = hex("#171928"),
                                },
                            .terminal =
                                TerminalPalette{
                                    .normal =
                                        TerminalAnsiColors{
                                            .black = hex("#ebfafa"),
                                            .red = hex("#ba1a1a"),
                                            .green = hex("#1a7f4c"),
                                            .yellow = hex("#9e8c13"),
                                            .blue = hex("#3a3e8c"),
                                            .magenta = hex("#8c2a6c"),
                                            .cyan = hex("#1a6c8c"),
                                            .white = hex("#212337"),
                                        },
                                    .bright =
                                        TerminalAnsiColors{
                                            .black = hex("#7081d0"),
                                            .red = hex("#d23b3b"),
                                            .green = hex("#37f499"),
                                            .yellow = hex("#e9f941"),
                                            .blue = hex("#9071f4"),
                                            .magenta = hex("#f265b5"),
                                            .cyan = hex("#04d1f9"),
                                            .white = hex("#212337"),
                                        },
                                    .foreground = hex("#212337"),
                                    .background = hex("#ebfafa"),
                                    .selectionFg = hex("#212337"),
                                    .selectionBg = hex("#bf4f8e"),
                                    .cursorText = hex("#1a7f4c"),
                                    .cursor = hex("#212337"),
                                },
                        },
                },
                {
                    .name = "Gruvbox",
                    .dark =
                        FixedPaletteMode{
                            .palette =
                                Palette{
                                    .primary = hex("#b8bb26"),
                                    .onPrimary = hex("#282828"),
                                    .secondary = hex("#fabd2f"),
                                    .onSecondary = hex("#282828"),
                                    .tertiary = hex("#83a598"),
                                    .onTertiary = hex("#282828"),
                                    .error = hex("#fb4934"),
                                    .onError = hex("#282828"),
                                    .surface = hex("#282828"),
                                    .onSurface = hex("#fbf1c7"),
                                    .surfaceVariant = hex("#3c3836"),
                                    .onSurfaceVariant = hex("#ebdbb2"),
                                    .outline = hex("#57514e"),
                                    .shadow = hex("#282828"),
                                    .hover = hex("#83a598"),
                                    .onHover = hex("#282828"),
                                },
                            .terminal =
                                TerminalPalette{
                                    .normal =
                                        TerminalAnsiColors{
                                            .black = hex("#282828"),
                                            .red = hex("#cc241d"),
                                            .green = hex("#98971a"),
                                            .yellow = hex("#d79921"),
                                            .blue = hex("#458588"),
                                            .magenta = hex("#b16286"),
                                            .cyan = hex("#689d6a"),
                                            .white = hex("#a89984"),
                                        },
                                    .bright =
                                        TerminalAnsiColors{
                                            .black = hex("#928374"),
                                            .red = hex("#fb4934"),
                                            .green = hex("#b8bb26"),
                                            .yellow = hex("#fabd2f"),
                                            .blue = hex("#83a598"),
                                            .magenta = hex("#d3869b"),
                                            .cyan = hex("#8ec07c"),
                                            .white = hex("#ebdbb2"),
                                        },
                                    .foreground = hex("#ebdbb2"),
                                    .background = hex("#282828"),
                                    .selectionFg = hex("#ebdbb2"),
                                    .selectionBg = hex("#665c54"),
                                    .cursorText = hex("#282828"),
                                    .cursor = hex("#ebdbb2"),
                                },
                        },
                    .light =
                        FixedPaletteMode{
                            .palette =
                                Palette{
                                    .primary = hex("#98971a"),
                                    .onPrimary = hex("#fbf1c7"),
                                    .secondary = hex("#d79921"),
                                    .onSecondary = hex("#fbf1c7"),
                                    .tertiary = hex("#458588"),
                                    .onTertiary = hex("#fbf1c7"),
                                    .error = hex("#cc241d"),
                                    .onError = hex("#fbf1c7"),
                                    .surface = hex("#fbf1c7"),
                                    .onSurface = hex("#3c3836"),
                                    .surfaceVariant = hex("#ebdbb2"),
                                    .onSurfaceVariant = hex("#7c6f64"),
                                    .outline = hex("#bdae93"),
                                    .shadow = hex("#d5c4a1"),
                                    .hover = hex("#458588"),
                                    .onHover = hex("#fbf1c7"),
                                },
                            .terminal =
                                TerminalPalette{
                                    .normal =
                                        TerminalAnsiColors{
                                            .black = hex("#fbf1c7"),
                                            .red = hex("#cc241d"),
                                            .green = hex("#98971a"),
                                            .yellow = hex("#d79921"),
                                            .blue = hex("#458588"),
                                            .magenta = hex("#b16286"),
                                            .cyan = hex("#689d6a"),
                                            .white = hex("#7c6f64"),
                                        },
                                    .bright =
                                        TerminalAnsiColors{
                                            .black = hex("#928374"),
                                            .red = hex("#9d0006"),
                                            .green = hex("#79740e"),
                                            .yellow = hex("#b57614"),
                                            .blue = hex("#076678"),
                                            .magenta = hex("#8f3f71"),
                                            .cyan = hex("#427b58"),
                                            .white = hex("#3c3836"),
                                        },
                                    .foreground = hex("#3c3836"),
                                    .background = hex("#fbf1c7"),
                                    .selectionFg = hex("#fbf1c7"),
                                    .selectionBg = hex("#3c3836"),
                                    .cursorText = hex("#625e5c"),
                                    .cursor = hex("#3c3836"),
                                },
                        },
                },
                {
                    .name = "Kanagawa",
                    .dark =
                        FixedPaletteMode{
                            .palette =
                                Palette{
                                    .primary = hex("#76946a"),
                                    .onPrimary = hex("#1f1f28"),
                                    .secondary = hex("#c0a36e"),
                                    .onSecondary = hex("#1f1f28"),
                                    .tertiary = hex("#7e9cd8"),
                                    .onTertiary = hex("#1f1f28"),
                                    .error = hex("#c34043"),
                                    .onError = hex("#1f1f28"),
                                    .surface = hex("#1f1f28"),
                                    .onSurface = hex("#c8c093"),
                                    .surfaceVariant = hex("#2a2a37"),
                                    .onSurfaceVariant = hex("#717c7c"),
                                    .outline = hex("#363646"),
                                    .shadow = hex("#1f1f28"),
                                    .hover = hex("#7e9cd8"),
                                    .onHover = hex("#1f1f28"),
                                },
                            .terminal =
                                TerminalPalette{
                                    .normal =
                                        TerminalAnsiColors{
                                            .black = hex("#090618"),
                                            .red = hex("#c34043"),
                                            .green = hex("#76946a"),
                                            .yellow = hex("#c0a36e"),
                                            .blue = hex("#7e9cd8"),
                                            .magenta = hex("#957fb8"),
                                            .cyan = hex("#6a9589"),
                                            .white = hex("#c8c093"),
                                        },
                                    .bright =
                                        TerminalAnsiColors{
                                            .black = hex("#727169"),
                                            .red = hex("#e82424"),
                                            .green = hex("#98bb6c"),
                                            .yellow = hex("#e6c384"),
                                            .blue = hex("#7fb4ca"),
                                            .magenta = hex("#938aa9"),
                                            .cyan = hex("#7aa89f"),
                                            .white = hex("#dcd7ba"),
                                        },
                                    .foreground = hex("#dcd7ba"),
                                    .background = hex("#1f1f28"),
                                    .selectionFg = hex("#c8c093"),
                                    .selectionBg = hex("#2d4f67"),
                                    .cursorText = hex("#1f1f28"),
                                    .cursor = hex("#c8c093"),
                                },
                        },
                    .light =
                        FixedPaletteMode{
                            .palette =
                                Palette{
                                    .primary = hex("#6f894e"),
                                    .onPrimary = hex("#f2ecbc"),
                                    .secondary = hex("#77713f"),
                                    .onSecondary = hex("#f2ecbc"),
                                    .tertiary = hex("#4d699b"),
                                    .onTertiary = hex("#f2ecbc"),
                                    .error = hex("#c84053"),
                                    .onError = hex("#f2ecbc"),
                                    .surface = hex("#f2ecbc"),
                                    .onSurface = hex("#545464"),
                                    .surfaceVariant = hex("#e5ddb0"),
                                    .onSurfaceVariant = hex("#8a8980"),
                                    .outline = hex("#cfc49c"),
                                    .shadow = hex("#f2ecbc"),
                                    .hover = hex("#4d699b"),
                                    .onHover = hex("#f2ecbc"),
                                },
                            .terminal =
                                TerminalPalette{
                                    .normal =
                                        TerminalAnsiColors{
                                            .black = hex("#1F1F28"),
                                            .red = hex("#c84053"),
                                            .green = hex("#6f894e"),
                                            .yellow = hex("#77713f"),
                                            .blue = hex("#4d699b"),
                                            .magenta = hex("#b35b79"),
                                            .cyan = hex("#597b75"),
                                            .white = hex("#545464"),
                                        },
                                    .bright =
                                        TerminalAnsiColors{
                                            .black = hex("#8a8980"),
                                            .red = hex("#d7474b"),
                                            .green = hex("#6e915f"),
                                            .yellow = hex("#836f4a"),
                                            .blue = hex("#6693bf"),
                                            .magenta = hex("#624c83"),
                                            .cyan = hex("#5e857a"),
                                            .white = hex("#43436c"),
                                        },
                                    .foreground = hex("#545464"),
                                    .background = hex("#f2ecbc"),
                                    .selectionFg = hex("#f2ecbc"),
                                    .selectionBg = hex("#c9cbd1"),
                                    .cursorText = hex("#f2ecbc"),
                                    .cursor = hex("#43436c"),
                                },
                        },
                },
                {
                    .name = "Noctalia",
                    .dark =
                        FixedPaletteMode{
                            .palette =
                                Palette{
                                    .primary = hex("#fff59b"),
                                    .onPrimary = hex("#0e0e43"),
                                    .secondary = hex("#a9aefe"),
                                    .onSecondary = hex("#0e0e43"),
                                    .tertiary = hex("#9BFECE"),
                                    .onTertiary = hex("#0e0e43"),
                                    .error = hex("#FD4663"),
                                    .onError = hex("#0e0e43"),
                                    .surface = hex("#070722"),
                                    .onSurface = hex("#f3edf7"),
                                    .surfaceVariant = hex("#11112d"),
                                    .onSurfaceVariant = hex("#7c80b4"),
                                    .outline = hex("#21215F"),
                                    .shadow = hex("#070722"),
                                    .hover = hex("#9BFECE"),
                                    .onHover = hex("#0e0e43"),
                                },
                            .terminal =
                                TerminalPalette{
                                    .normal =
                                        TerminalAnsiColors{
                                            .black = hex("#11112d"),
                                            .red = hex("#FD4663"),
                                            .green = hex("#9BFECE"),
                                            .yellow = hex("#fff59b"),
                                            .blue = hex("#a9aefe"),
                                            .magenta = hex("#FD4663"),
                                            .cyan = hex("#9BFECE"),
                                            .white = hex("#f3edf7"),
                                        },
                                    .bright =
                                        TerminalAnsiColors{
                                            .black = hex("#21215F"),
                                            .red = hex("#FD4663"),
                                            .green = hex("#9BFECE"),
                                            .yellow = hex("#fff59b"),
                                            .blue = hex("#a9aefe"),
                                            .magenta = hex("#FD4663"),
                                            .cyan = hex("#9BFECE"),
                                            .white = hex("#ffffff"),
                                        },
                                    .foreground = hex("#f3edf7"),
                                    .background = hex("#070722"),
                                    .selectionFg = hex("#070722"),
                                    .selectionBg = hex("#f3edf7"),
                                    .cursorText = hex("#070722"),
                                    .cursor = hex("#fff59b"),
                                },
                        },
                    .light =
                        FixedPaletteMode{
                            .palette =
                                Palette{
                                    .primary = hex("#5d65f5"),
                                    .onPrimary = hex("#dadcff"),
                                    .secondary = hex("#8E93D8"),
                                    .onSecondary = hex("#dadcff"),
                                    .tertiary = hex("#0e0e43"),
                                    .onTertiary = hex("#fef29a"),
                                    .error = hex("#FD4663"),
                                    .onError = hex("#0e0e43"),
                                    .surface = hex("#e6e8fa"),
                                    .onSurface = hex("#0e0e43"),
                                    .surfaceVariant = hex("#eff0ff"),
                                    .onSurfaceVariant = hex("#4b55c8"),
                                    .outline = hex("#8288fc"),
                                    .shadow = hex("#f3edf7"),
                                    .hover = hex("#0e0e43"),
                                    .onHover = hex("#fef29a"),
                                },
                            .terminal =
                                TerminalPalette{
                                    .normal =
                                        TerminalAnsiColors{
                                            .black = hex("#eff0ff"),
                                            .red = hex("#FD4663"),
                                            .green = hex("#0e0e43"),
                                            .yellow = hex("#5d65f5"),
                                            .blue = hex("#8E93D8"),
                                            .magenta = hex("#FD4663"),
                                            .cyan = hex("#0e0e43"),
                                            .white = hex("#4b55c8"),
                                        },
                                    .bright =
                                        TerminalAnsiColors{
                                            .black = hex("#8288fc"),
                                            .red = hex("#FD4663"),
                                            .green = hex("#0e0e43"),
                                            .yellow = hex("#5d65f5"),
                                            .blue = hex("#8E93D8"),
                                            .magenta = hex("#FD4663"),
                                            .cyan = hex("#0e0e43"),
                                            .white = hex("#0e0e43"),
                                        },
                                    .foreground = hex("#4b55c8"),
                                    .background = hex("#e6e8fa"),
                                    .selectionFg = hex("#e6e8fa"),
                                    .selectionBg = hex("#4b55c8"),
                                    .cursorText = hex("#e6e8fa"),
                                    .cursor = hex("#5d65f5"),
                                },
                        },
                },
                {
                    .name = "Nord",
                    .dark =
                        FixedPaletteMode{
                            .palette =
                                Palette{
                                    .primary = hex("#8fbcbb"),
                                    .onPrimary = hex("#2e3440"),
                                    .secondary = hex("#88c0d0"),
                                    .onSecondary = hex("#2e3440"),
                                    .tertiary = hex("#5e81ac"),
                                    .onTertiary = hex("#2e3440"),
                                    .error = hex("#bf616a"),
                                    .onError = hex("#2e3440"),
                                    .surface = hex("#2e3440"),
                                    .onSurface = hex("#eceff4"),
                                    .surfaceVariant = hex("#3b4252"),
                                    .onSurfaceVariant = hex("#d8dee9"),
                                    .outline = hex("#505a70"),
                                    .shadow = hex("#2e3440"),
                                    .hover = hex("#5e81ac"),
                                    .onHover = hex("#2e3440"),
                                },
                            .terminal =
                                TerminalPalette{
                                    .normal =
                                        TerminalAnsiColors{
                                            .black = hex("#3b4252"),
                                            .red = hex("#bf616a"),
                                            .green = hex("#a3be8c"),
                                            .yellow = hex("#ebcb8b"),
                                            .blue = hex("#81a1c1"),
                                            .magenta = hex("#b48ead"),
                                            .cyan = hex("#88c0d0"),
                                            .white = hex("#e5e9f0"),
                                        },
                                    .bright =
                                        TerminalAnsiColors{
                                            .black = hex("#596377"),
                                            .red = hex("#bf616a"),
                                            .green = hex("#a3be8c"),
                                            .yellow = hex("#ebcb8b"),
                                            .blue = hex("#81a1c1"),
                                            .magenta = hex("#b48ead"),
                                            .cyan = hex("#8fbcbb"),
                                            .white = hex("#eceff4"),
                                        },
                                    .foreground = hex("#d8dee9"),
                                    .background = hex("#2e3440"),
                                    .selectionFg = hex("#4c566a"),
                                    .selectionBg = hex("#eceff4"),
                                    .cursorText = hex("#282828"),
                                    .cursor = hex("#eceff4"),
                                },
                        },
                    .light =
                        FixedPaletteMode{
                            .palette =
                                Palette{
                                    .primary = hex("#5e81ac"),
                                    .onPrimary = hex("#eceff4"),
                                    .secondary = hex("#64adc2"),
                                    .onSecondary = hex("#eceff4"),
                                    .tertiary = hex("#6fa9a8"),
                                    .onTertiary = hex("#eceff4"),
                                    .error = hex("#bf616a"),
                                    .onError = hex("#eceff4"),
                                    .surface = hex("#eceff4"),
                                    .onSurface = hex("#2e3440"),
                                    .surfaceVariant = hex("#e5e9f0"),
                                    .onSurfaceVariant = hex("#4c566a"),
                                    .outline = hex("#c5cedd"),
                                    .shadow = hex("#d8dee9"),
                                    .hover = hex("#6fa9a8"),
                                    .onHover = hex("#eceff4"),
                                },
                            .terminal =
                                TerminalPalette{
                                    .normal =
                                        TerminalAnsiColors{
                                            .black = hex("#3b4252"),
                                            .red = hex("#bf616a"),
                                            .green = hex("#96b17f"),
                                            .yellow = hex("#c5a565"),
                                            .blue = hex("#81a1c1"),
                                            .magenta = hex("#b48ead"),
                                            .cyan = hex("#7bb3c3"),
                                            .white = hex("#a5abb6"),
                                        },
                                    .bright =
                                        TerminalAnsiColors{
                                            .black = hex("#4c566a"),
                                            .red = hex("#bf616a"),
                                            .green = hex("#96b17f"),
                                            .yellow = hex("#c5a565"),
                                            .blue = hex("#81a1c1"),
                                            .magenta = hex("#b48ead"),
                                            .cyan = hex("#82afae"),
                                            .white = hex("#eceff4"),
                                        },
                                    .foreground = hex("#414858"),
                                    .background = hex("#eceff4"),
                                    .selectionFg = hex("#4c556a"),
                                    .selectionBg = hex("#d8dee9"),
                                    .cursorText = hex("#3b4252"),
                                    .cursor = hex("#7bb3c3"),
                                },
                        },
                },
                {
                    .name = "Rosé Pine",
                    .dark =
                        FixedPaletteMode{
                            .palette =
                                Palette{
                                    .primary = hex("#ebbcba"),
                                    .onPrimary = hex("#191724"),
                                    .secondary = hex("#9ccfd8"),
                                    .onSecondary = hex("#191724"),
                                    .tertiary = hex("#31748f"),
                                    .onTertiary = hex("#e0def4"),
                                    .error = hex("#eb6f92"),
                                    .onError = hex("#191724"),
                                    .surface = hex("#191724"),
                                    .onSurface = hex("#e0def4"),
                                    .surfaceVariant = hex("#26233a"),
                                    .onSurfaceVariant = hex("#908caa"),
                                    .outline = hex("#403d52"),
                                    .shadow = hex("#191724"),
                                    .hover = hex("#524f67"),
                                    .onHover = hex("#e0def4"),
                                },
                            .terminal =
                                TerminalPalette{
                                    .normal =
                                        TerminalAnsiColors{
                                            .black = hex("#26233a"),
                                            .red = hex("#eb6f92"),
                                            .green = hex("#31748f"),
                                            .yellow = hex("#f6c177"),
                                            .blue = hex("#9ccfd8"),
                                            .magenta = hex("#c4a7e7"),
                                            .cyan = hex("#ebbcba"),
                                            .white = hex("#e0def4"),
                                        },
                                    .bright =
                                        TerminalAnsiColors{
                                            .black = hex("#6e6a86"),
                                            .red = hex("#eb6f92"),
                                            .green = hex("#31748f"),
                                            .yellow = hex("#f6c177"),
                                            .blue = hex("#9ccfd8"),
                                            .magenta = hex("#c4a7e7"),
                                            .cyan = hex("#ebbcba"),
                                            .white = hex("#e0def4"),
                                        },
                                    .foreground = hex("#e0def4"),
                                    .background = hex("#191724"),
                                    .selectionFg = hex("#e0def4"),
                                    .selectionBg = hex("#403d52"),
                                    .cursorText = hex("#191724"),
                                    .cursor = hex("#e0def4"),
                                },
                        },
                    .light =
                        FixedPaletteMode{
                            .palette =
                                Palette{
                                    .primary = hex("#d7827e"),
                                    .onPrimary = hex("#faf4ed"),
                                    .secondary = hex("#56949f"),
                                    .onSecondary = hex("#faf4ed"),
                                    .tertiary = hex("#286983"),
                                    .onTertiary = hex("#faf4ed"),
                                    .error = hex("#b4637a"),
                                    .onError = hex("#faf4ed"),
                                    .surface = hex("#fffaf3"),
                                    .onSurface = hex("#575279"),
                                    .surfaceVariant = hex("#f2e9e1"),
                                    .onSurfaceVariant = hex("#797593"),
                                    .outline = hex("#dfdad9"),
                                    .shadow = hex("#faf4ed"),
                                    .hover = hex("#cecacd"),
                                    .onHover = hex("#575279"),
                                },
                            .terminal =
                                TerminalPalette{
                                    .normal =
                                        TerminalAnsiColors{
                                            .black = hex("#f2e9e1"),
                                            .red = hex("#b4637a"),
                                            .green = hex("#286983"),
                                            .yellow = hex("#ea9d34"),
                                            .blue = hex("#56949f"),
                                            .magenta = hex("#907aa9"),
                                            .cyan = hex("#d7827e"),
                                            .white = hex("#575279"),
                                        },
                                    .bright =
                                        TerminalAnsiColors{
                                            .black = hex("#9893a5"),
                                            .red = hex("#b4637a"),
                                            .green = hex("#286983"),
                                            .yellow = hex("#ea9d34"),
                                            .blue = hex("#56949f"),
                                            .magenta = hex("#907aa9"),
                                            .cyan = hex("#d7827e"),
                                            .white = hex("#575279"),
                                        },
                                    .foreground = hex("#575279"),
                                    .background = hex("#faf4ed"),
                                    .selectionFg = hex("#575279"),
                                    .selectionBg = hex("#dfdad9"),
                                    .cursorText = hex("#faf4ed"),
                                    .cursor = hex("#575279"),
                                },
                        },
                },
                {
                    .name = "Tokyo-Night",
                    .dark =
                        FixedPaletteMode{
                            .palette =
                                Palette{
                                    .primary = hex("#7aa2f7"),
                                    .onPrimary = hex("#16161e"),
                                    .secondary = hex("#bb9af7"),
                                    .onSecondary = hex("#16161e"),
                                    .tertiary = hex("#9ece6a"),
                                    .onTertiary = hex("#16161e"),
                                    .error = hex("#f7768e"),
                                    .onError = hex("#16161e"),
                                    .surface = hex("#1a1b26"),
                                    .onSurface = hex("#c0caf5"),
                                    .surfaceVariant = hex("#24283b"),
                                    .onSurfaceVariant = hex("#9aa5ce"),
                                    .outline = hex("#353D57"),
                                    .shadow = hex("#15161e"),
                                    .hover = hex("#9ece6a"),
                                    .onHover = hex("#16161e"),
                                },
                            .terminal =
                                TerminalPalette{
                                    .normal =
                                        TerminalAnsiColors{
                                            .black = hex("#15161e"),
                                            .red = hex("#f7768e"),
                                            .green = hex("#9ece6a"),
                                            .yellow = hex("#e0af68"),
                                            .blue = hex("#7aa2f7"),
                                            .magenta = hex("#bb9af7"),
                                            .cyan = hex("#7dcfff"),
                                            .white = hex("#a9b1d6"),
                                        },
                                    .bright =
                                        TerminalAnsiColors{
                                            .black = hex("#414868"),
                                            .red = hex("#f7768e"),
                                            .green = hex("#9ece6a"),
                                            .yellow = hex("#e0af68"),
                                            .blue = hex("#7aa2f7"),
                                            .magenta = hex("#bb9af7"),
                                            .cyan = hex("#7dcfff"),
                                            .white = hex("#c0caf5"),
                                        },
                                    .foreground = hex("#c0caf5"),
                                    .background = hex("#1a1b26"),
                                    .selectionFg = hex("#c0caf5"),
                                    .selectionBg = hex("#283457"),
                                    .cursorText = hex("#1a1b26"),
                                    .cursor = hex("#c0caf5"),
                                },
                        },
                    .light =
                        FixedPaletteMode{
                            .palette =
                                Palette{
                                    .primary = hex("#2e7de9"),
                                    .onPrimary = hex("#e1e2e7"),
                                    .secondary = hex("#9854f1"),
                                    .onSecondary = hex("#e1e2e7"),
                                    .tertiary = hex("#587539"),
                                    .onTertiary = hex("#e1e2e7"),
                                    .error = hex("#f52a65"),
                                    .onError = hex("#e1e2e7"),
                                    .surface = hex("#e1e2e7"),
                                    .onSurface = hex("#3760bf"),
                                    .surfaceVariant = hex("#d0d5e3"),
                                    .onSurfaceVariant = hex("#6172b0"),
                                    .outline = hex("#b4b5b9"),
                                    .shadow = hex("#a8aecb"),
                                    .hover = hex("#587539"),
                                    .onHover = hex("#e1e2e7"),
                                },
                            .terminal =
                                TerminalPalette{
                                    .normal =
                                        TerminalAnsiColors{
                                            .black = hex("#e9e9ed"),
                                            .red = hex("#f52a65"),
                                            .green = hex("#587539"),
                                            .yellow = hex("#8c6c3e"),
                                            .blue = hex("#2e7de9"),
                                            .magenta = hex("#9854f1"),
                                            .cyan = hex("#007197"),
                                            .white = hex("#6172b0"),
                                        },
                                    .bright =
                                        TerminalAnsiColors{
                                            .black = hex("#a1a6c5"),
                                            .red = hex("#f52a65"),
                                            .green = hex("#587539"),
                                            .yellow = hex("#8c6c3e"),
                                            .blue = hex("#2e7de9"),
                                            .magenta = hex("#9854f1"),
                                            .cyan = hex("#007197"),
                                            .white = hex("#3760bf"),
                                        },
                                    .foreground = hex("#3760bf"),
                                    .background = hex("#e1e2e7"),
                                    .selectionFg = hex("#3760bf"),
                                    .selectionBg = hex("#99a7df"),
                                    .cursorText = hex("#e1e2e7"),
                                    .cursor = hex("#3760bf"),
                                },
                        },
                },
            }
    };

  } // namespace

  std::span<const BuiltinPalette> builtinPalettes() { return kPalettes; }

  const BuiltinPalette* findBuiltinPalette(std::string_view name) {
    for (const auto& palette : kPalettes) {
      if (palette.name == name) {
        return &palette;
      }
    }
    return nullptr;
  }

  GeneratedPalette expandBuiltinPalette(const BuiltinPalette& palette) {
    auto generated = expandFixedPalettes(palette.dark.palette, palette.light.palette);
    applyTerminalPalette(generated.dark, palette.dark.terminal);
    applyTerminalPalette(generated.light, palette.light.terminal);
    return generated;
  }

} // namespace noctalia::theme
