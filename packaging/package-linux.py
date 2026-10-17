#!/usr/bin/env python3
"""Package the verified static client with notices, symbols and exact sources."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile

root = Path(__file__).resolve().parent.parent
build, revision = Path(sys.argv[1]).resolve(), sys.argv[2]
out = root / 'out'
package = out / 'looking-glass'
if package.exists():
    shutil.rmtree(package)
package.mkdir(parents=True)
exe = package / 'looking-glass-client'
shutil.copy2(build / 'bin/looking-glass-client', exe)
exe.chmod(0o755)
(out / 'looking-glass-client.debug').unlink(missing_ok=True)
shutil.copy2(build / 'bin/.debug/looking-glass-client.debug', out / 'looking-glass-client.debug')
for args, forbidden in [(['-lW'], 'INTERP'), (['-dW'], '(NEEDED)')]:
    result = subprocess.check_output(['readelf', *args, str(exe)], text=True)
    if forbidden in result:
        raise SystemExit('Client is not fully static')
subprocess.run([str(exe), '--help'], check=True, stdout=subprocess.DEVNULL)
sources = Path(subprocess.check_output(['nix-build', 'packaging/nix', '-A', 'sourceInputs', '--no-out-link'], cwd=root, text=True).strip())
notices = package / 'licenses'
notices.mkdir(exist_ok=True)
shutil.copy2(root / 'LICENSE', notices / 'LookingGlass-LICENSE')
# License texts are kept verbatim, including for dependencies whose source is a tarball.
for dependency in sources.iterdir():
    destination = notices / dependency.name
    destination.mkdir(exist_ok=True)
    source = dependency / 'source'
    if source.is_dir():
        for file in source.rglob('*'):
            if file.is_file() and file.name.lower().startswith(('copying', 'copyright', 'license', 'licence')):
                target = destination / file.relative_to(source)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(file, target)
    elif tarfile.is_tarfile(source):
        with tarfile.open(source) as archive:
            for member in archive.getmembers():
                if member.isfile() and Path(member.name).name.lower().startswith(('copying', 'copyright', 'license', 'licence')):
                    target = destination / Path(member.name).name
                    target.write_bytes(archive.extractfile(member).read())
(package / 'SOURCES.txt').write_text(
    f'Looking Glass source revision: {revision}\n'
    'The matching looking-glass-sources.tar.gz includes this application, submodules,\n'
    'dependency sources, patches, and the pinned Nix build recipe. To rebuild, extract\n'
    f'the archive and run nix-build packaging/nix -A client --argstr revision {revision} in LookingGlass.\n'
    'Host OpenGL/EGL drivers are loaded at runtime through SoLo; they are not bundled.\n')
files = {str(p.relative_to(package)): hashlib.sha256(p.read_bytes()).hexdigest()
         for p in package.rglob('*') if p.is_file() and p.name != 'manifest.json'}
(package / 'manifest.json').write_text(json.dumps({
    'schemaVersion': 1, 'sourceRevision': revision, 'architecture': 'x64',
    'component': 'client', 'libc': 'musl', 'elfInterpreter': None, 'elfNeeded': [], 'files': files,
}, indent=2) + '\n')
with tarfile.open(out / 'looking-glass-linux-x64.tar.gz', 'w:gz') as archive:
    archive.add(package, arcname='looking-glass')
with tarfile.open(out / 'looking-glass-linux-x64-symbols.tar.gz', 'w:gz') as archive:
    archive.add(out / 'looking-glass-client.debug', arcname='looking-glass-client.debug')
with tarfile.open(out / 'looking-glass-sources.tar.gz', 'w:gz') as archive:
    def source_filter(info):
        return None if any(p in ('.git', 'build', 'out', 'result', '__pycache__') for p in Path(info.name).parts) else info
    archive.add(root, arcname='LookingGlass', filter=source_filter)
    archive.add(sources, arcname='dependencies')
print(json.dumps({'archiveSha256': hashlib.sha256((out / 'looking-glass-linux-x64.tar.gz').read_bytes()).hexdigest(), 'executableSha256': files['looking-glass-client']}))
