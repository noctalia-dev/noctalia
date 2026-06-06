{
  config,
  pkgs,
  lib,
  ...
}:
let
  inherit (lib.modules) mkIf;
  inherit (lib.options) mkEnableOption mkOption;
  inherit (lib.attrsets) mapAttrs mapAttrs' attrValues;
  inherit (lib.lists) optional;

  cfg = config.programs.noctalia;
  toml = pkgs.formats.toml { };
  json = pkgs.formats.json { };

  configToml = toml.generate "config.toml" cfg.settings;
  paletteFiles = mapAttrs (
    name: palette: json.generate "${name}-palette.json" palette
  ) cfg.customPalettes;
in
{
  options.programs.noctalia = {
    enable = mkEnableOption "Whether to enable noctalia, a lightweight Wayland shell and bar.";

    systemd = {
      enable = mkEnableOption "Enables a systemd user service for noctalia.";

      target = mkOption {
        type = lib.types.str;
        default = "graphical-session.target";
        example = "hyprland-session.target";
        description = "The systemd target for the noctalia service.";
      };
    };

    package = mkOption {
      type = lib.types.nullOr lib.types.package;
      description = "The noctalia package to use.";
    };

    settings = mkOption {
      type = toml.type;
      default = { };
      description = ''
        Configuration written to {file}`$XDG_CONFIG_HOME/noctalia/config.toml`.
        See <https://docs.noctalia.dev/v5> for more information and examples.
      '';
      example = lib.literalExpression ''
        shell = {
          font = "JetBrainsMono Nerd Font";
          settings_show_advanced = true;
        };

        theme = {
          mode = "dark";
          source = "builtin";
          builtin = "Catppuccin";
        };
      '';
    };

    customPalettes = mkOption {
      type = json.type;
      default = { };
      description = ''
        Custom color palettes written to {file}`$XDG_CONFIG_HOME/noctalia/palettes/<name>.json`.
        See <https://docs.noctalia.dev/v5/theming/#custom_palette> for more information.
      '';
      example = lib.literalExpression ''
        cherry-blossom = {
          dark = {
            mPrimary = "#F2C1D4";
            mOnPrimary = "#2A1B21";
            mSecondary = "#FFD6E2";
            ...
          };
        };
      '';
    };
  };

  config = mkIf cfg.enable {
    packages = optional (cfg.package != null) cfg.package;

    systemd.services.noctalia = mkIf (cfg.systemd.enable) {
      description = "Noctalia - A lightweight Wayland shell and bar";
      documentation = [ "https://docs.noctalia.dev/v5/" ];
      partOf = [ cfg.systemd.target ];
      after = [ cfg.systemd.target ];
      wantedBy = [ cfg.systemd.target ];
      # without this the service will have the default
      # Environment="PATH=coreutils:…", clobbering the PATH that the DE
      # imported into the user manager.
      enableDefaultPath = false;
      restartTriggers = [
        cfg.package
      ]
      ++ optional (cfg.settings != { }) configToml
      ++ attrValues paletteFiles;
      serviceConfig = {
        ExecStart = lib.getExe cfg.package;
        Restart = "on-failure";
      };
    };

    xdg.config.files = lib.mkMerge [
      (mkIf (cfg.settings != { }) {
        "noctalia/config.toml".source = configToml;
      })
      (mapAttrs' (
        name: source: lib.nameValuePair "noctalia/palettes/${name}.json" { inherit source; }
      ) paletteFiles)
    ];

    assertions = [
      {
        assertion = !cfg.systemd.enable || cfg.package != null;
        message = "programs.noctalia.package cannot be null when programs.noctalia.systemd.enable is true";
      }
    ];
  };

  _class = "hjem";
}
