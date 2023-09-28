{ stdenv }: stdenv.mkDerivation {
  pname = "inotify-info";
  version = "0.0.1";

  src = ./.;

  installPhase = ''
    mkdir -p "$out/bin"
    cp "_release/inotify-info" "$out/bin/"
  '';
}
