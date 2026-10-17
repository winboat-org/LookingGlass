#!/usr/bin/env python3
"""Keep host graphics libraries on the same display connections as the client."""
import re
import subprocess
import sys
from pathlib import Path

output = Path(sys.argv[1])
providers = []
for spec in sys.argv[2:]:
    soname, archive = spec.split('=', 1)
    symbols = subprocess.check_output(['nm', '-g', '--defined-only', archive], text=True)
    names = set()
    for line in symbols.splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[1] in ('T', 'D', 'R', 'B', 'W') and re.fullmatch(r'[A-Za-z_]\w*', fields[2]):
            names.add(fields[2])
    if not names:
        raise SystemExit(f'No provider symbols found in {archive}')
    providers.append((soname, sorted(names)))
lines = ['#include <solo/dlfcn.h>']
for symbol in sorted({name for _, names in providers for name in names}):
    lines.append(f'extern char {symbol};')
lines.append('__attribute__((constructor)) static void register_display_providers(void) {')
for soname, names in providers:
    for name in names:
        lines.append(f'  stub_dlregister("{soname}", "{name}", &{name});')
lines.append('}')
output.write_text('\n'.join(lines) + '\n')
