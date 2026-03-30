{ pkgs }: {
	deps = [
   pkgs.openssl
   pkgs.curl
		pkgs.clang
		pkgs.ccls
		pkgs.gdb
		pkgs.gnumake
	];
}