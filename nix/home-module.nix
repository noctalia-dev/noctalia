{
  config,
  pkgs,
  lib,
  ...
}:
let
  cfg = config.programs.noctalia;
  jsonFormat = pkgs.formats.json { };
  tomlFormat = pkgs.formats.toml { };

  finalPackage =
    if cfg.package == null || cfg.extraPackages == [ ] then
      cfg.package
    else
      pkgs.symlinkJoin {
        name = "${cfg.package.name}-wrapped";
        paths = [ cfg.package ];
        nativeBuildInputs = [ pkgs.makeWrapper ];
        postBuild = ''
          wrapProgram $out/bin/noctalia \
            --prefix PATH : ${lib.makeBinPath cfg.extraPackages}
        '';
        meta = cfg.package.meta;
      };

  generateConfig =
    format: name: value:
    if lib.isString value then
      pkgs.writeText name value
    else if builtins.isPath value || lib.isStorePath value then
      value
    else
      format.generate name value;

  generateToml = generateConfig tomlFormat;
  generateJson = generateConfig jsonFormat;
in
{
  # Disable the home-manager module to avoid conflicts
  # The module got turned into a folder on 2026-09-30. Disable both old and new.
  disabledModules = [
    "programs/noctalia.nix"
    "programs/noctalia"
  ];

  imports = [
    (lib.mkRenamedOptionModule
      [ "programs" "noctalia" "validateConfig" ]
      [ "programs" "noctalia" "checkConfig" ]
    )
  ];

  options.programs.noctalia = {
    enable = lib.mkEnableOption "noctalia, a lightweight Wayland shell and bar";

    systemd.enable = lib.mkEnableOption "a systemd user service for noctalia";

    package = lib.mkPackageOption pkgs "noctalia" { nullable = true; };

    extraPackages = lib.mkOption {
      type = lib.types.listOf lib.types.package;
      default = [ ];
      example = lib.literalExpression "[ pkgs.gpu-screen-recorder pkgs.slurp ]";
      description = ''
        Packages added to Noctalia's PATH, for example commands used by plugins.
        The module wraps the selected package without rebuilding Noctalia.
        Requires programs.noctalia.package to be non-null.
      '';
    };

    checkConfig = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Validate the configuration file at build time.";
    };

    settings = lib.mkOption {
      type =
        with lib.types;
        oneOf [
          tomlFormat.type
          str
          path
        ];
      default = { };
      description = ''
        Default settings for noctalia, Can be written as:
          - A Nix attrset (converted to TOML via nixpkgs' tomlFormat)
          - A raw TOML string
          - A path to a `.toml` file

        See <https://docs.noctalia.dev/noctalia/configuration/> for more information and examples.

        Note: these settings can still be overwritten at runtime via the settings menu.
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

    customPalettes = lib.mkOption {
      type =
        with lib.types;
        attrsOf (oneOf [
          jsonFormat.type
          str
          path
        ]);
      default = { };
      description = ''
        Custom color pallete options.

        See <https://docs.noctalia.dev/noctalia/theming/palette/#custom-palette-files>.
      '';
    };
  };

  config = lib.mkIf cfg.enable {
    systemd.user.services.noctalia = lib.mkIf cfg.systemd.enable {
      Unit = {
        Description = "Noctalia - A lightweight Wayland shell and bar";
        Documentation = "https://docs.noctalia.dev/noctalia/";
        PartOf = [ config.wayland.systemd.target ];
        After = [ config.wayland.systemd.target ];
        Before = lib.remove config.wayland.systemd.target [ "graphical-session.target" ];
        X-Restart-Triggers =
          lib.optional (cfg.settings != { }) "${config.xdg.configFile."noctalia/config.toml".source}"
          ++ lib.mapAttrsToList (
            name: _: "${config.xdg.configFile."noctalia/palettes/${name}.json".source}"
          ) cfg.customPalettes;
      };

      Service = {
        Type = "notify";
        ExecStart = lib.getExe finalPackage;
        Restart = "on-failure";
      };

      Install.WantedBy = [ config.wayland.systemd.target ];
    };

    home.packages = lib.optional (finalPackage != null) finalPackage;

    xdg = {
      configFile = lib.mkMerge [
        (lib.mkIf (cfg.settings != { }) {
          "noctalia/config.toml".source =
            let
              rawConfig = generateToml "config.toml" cfg.settings;
            in
            if cfg.checkConfig && cfg.package != null then
              pkgs.runCommand "noctalia-config.toml" { } ''
                ${lib.getExe finalPackage} config validate ${rawConfig}
                cp ${rawConfig} $out
              ''
            else
              rawConfig;
        })
        (lib.mapAttrs' (
          name: palette:
          lib.nameValuePair "noctalia/palettes/${name}.json" {
            source = generateJson "${name}-palette.json" palette;
          }
        ) cfg.customPalettes)
      ];
    };

    assertions = [
      {
        assertion = cfg.extraPackages == [ ] || cfg.package != null;
        message = "programs.noctalia.package cannot be null when programs.noctalia.extraPackages is non-empty";
      }
      {
        assertion = !cfg.systemd.enable || cfg.package != null;
        message = "programs.noctalia.package cannot be null when programs.noctalia.systemd.enable is true";
      }
    ];
  };

  _class = "homeManager";
}
