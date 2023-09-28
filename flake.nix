{
  inputs.nixpkgs.url = "github:NixOS/nixpkgs";
  inputs.flake-utils.url = "github:numtide/flake-utils";

  outputs = { self, nixpkgs, flake-utils }: flake-utils.lib.eachSystem [ "x86_64-linux" ] (localSystem:
    let
      pkgs = import nixpkgs { inherit localSystem; };

      inherit (pkgs) nil mkShell callPackage;


      package = callPackage ./package.nix { };
    in
    {

      packages.default = package;

      devShells.default = mkShell {
        packages = [ nil ];
      };
    });
}
