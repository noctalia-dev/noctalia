# Classic Nix shell for `nix-shell`, without flakes.
# Same environment as `nix develop` (see nix/devshell.nix).
let
  inherit (builtins) fromJSON readFile;

  lock = fromJSON (readFile ./flake.lock);
  namedNode = lock.nodes.${lock.nodes.root.inputs.nixpkgs}.locked;
  nixpkgs = fetchTarball {
    inherit (namedNode) url;
    sha256 = namedNode.narHash;
  };
  pkgs = import nixpkgs { };
  noctalia = pkgs.callPackage ./nix/package.nix { };
in
pkgs.callPackage ./nix/devshell.nix { inherit noctalia; }
