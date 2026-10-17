let pkgs = import (builtins.fetchTree (builtins.fromJSON (builtins.readFile ./nixpkgs.json))) { system = builtins.currentSystem; };
in pkgs.mkShell { packages = [ pkgs.python3 pkgs.binutils pkgs.git pkgs.gnutar pkgs.gzip ]; }
