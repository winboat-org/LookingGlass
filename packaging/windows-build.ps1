# Use the existing Windows MSBuild/WDK pipeline for the IDD and helper.
param([Parameter(Mandatory)][string]$Source, [Parameter(Mandatory)][string]$Revision)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
Set-Location $Source
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'
$vc = (Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
$env:PATH = "$vc\bin\Hostx64\x64;$env:PATH"
& nuget restore idd\LGIdd.sln
if ($LASTEXITCODE -ne 0) { throw 'NuGet restore failed' }
@'
<Project>
  <ItemDefinitionGroup Condition="'$(Configuration)'=='Release'">
    <ClCompile><RuntimeLibrary>MultiThreaded</RuntimeLibrary></ClCompile>
  </ItemDefinitionGroup>
</Project>
'@ | Set-Content idd\Directory.Build.targets
& $msbuild idd\LGIdd.sln /m /p:Configuration=Release /p:Platform=x64 /p:SignMode=Off /p:Inf2CatUseLocalTime=true /v:minimal
if ($LASTEXITCODE -ne 0) { throw 'IDD build failed' }
$out = Join-Path $Source 'out\looking-glass'
New-Item -ItemType Directory -Force $out, "$Source\out\symbols" | Out-Null
foreach ($name in @('LGIdd.dll','LGInput.dll','LGIdd.inf','LGInput.inf','LGIddHelper.exe','LGIddInstall.exe','lgidd.cat','LGInput.cat')) {
  $file = Get-ChildItem idd\x64\Release -Recurse -File | Where-Object Name -eq $name | Select-Object -First 1
  if (!$file) { throw "Missing output: $name" }
  Copy-Item $file.FullName $out
}
Get-ChildItem idd\x64\Release -Recurse -Filter '*.pdb' | ForEach-Object { Copy-Item $_.FullName "$Source\out\symbols" -Force }
Invoke-WebRequest https://dl.quantum2.xyz/ivshmem.tar.gz -OutFile out\ivshmem.tar.gz
if ((Get-FileHash out\ivshmem.tar.gz -Algorithm SHA256).Hash.ToLowerInvariant() -ne '017ee595ef0e8b0d3be5e5e06e8b58382a2eca645f71a0e12e147834575877a7') { throw 'IVSHMEM checksum mismatch' }
& tar -xzf out\ivshmem.tar.gz -C $out
Remove-Item out\ivshmem.tar.gz
Copy-Item LICENSE $out
Copy-Item packaging\ivshmem-LICENSE "$out\ivshmem\LICENSE"
# A test certificate is sufficient for WinBoat's test-signing guest workflow.
# The private key never leaves the ephemeral runner; only the public certificate ships.
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=WinBoat Looking Glass' -CertStoreLocation Cert:\CurrentUser\My -HashAlgorithm SHA256
Export-Certificate -Cert $cert -FilePath "$out\looking-glass.cer" | Out-Null
$sign = (Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter signtool.exe | Where-Object FullName -Match '\\x64\\' | Select-Object -First 1).FullName
foreach ($name in @('LGIdd.dll','LGInput.dll','LGIddHelper.exe','LGIddInstall.exe')) {
  $imports = & "$vc\bin\Hostx64\x64\dumpbin.exe" /dependents "$out\$name"
  if ($LASTEXITCODE -ne 0 -or ($imports -match '(?i)(vcruntime|msvcp|ucrtbase|api-ms-win-crt).*\.dll')) { throw "Dynamic CRT found in $name" }
  & $sign sign /fd SHA256 /sha1 $cert.Thumbprint "$out\$name"
  if ($LASTEXITCODE -ne 0) { throw "Signing $name failed" }
}
# Regenerate catalogs after signing the binaries (their hashes have changed).
$inf2cat = (Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter Inf2Cat.exe | Where-Object FullName -Match '\\x64\\' | Select-Object -First 1).FullName
& $inf2cat "/driver:$out" /os:10_X64 /uselocaltime
if ($LASTEXITCODE -ne 0) { throw 'Inf2Cat failed' }
foreach ($name in @('lgidd.cat','LGInput.cat')) {
  & $sign sign /fd SHA256 /sha1 $cert.Thumbprint "$out\$name"
  if ($LASTEXITCODE -ne 0) { throw "Signing $name failed" }
}
$files = @{}
Get-ChildItem $out -Recurse -File | ForEach-Object {
  $relative = [IO.Path]::GetRelativePath($out,$_.FullName).Replace('\','/')
  $files[$relative] = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
}
@{schemaVersion=1; sourceRevision=$Revision; architecture='x64'; component='idd'; crt='static'; files=$files} | ConvertTo-Json -Depth 5 | Set-Content "$out\manifest.json" -Encoding utf8NoBOM
@"
Looking Glass: https://github.com/winboat-org/LookingGlass/tree/$Revision
Corresponding source and symbols are included in the portable-build CI run.
IVSHMEM: https://github.com/virtio-win/kvm-guest-drivers-windows/tree/master/ivshmem
Built using the Windows 2022 runner MSBuild and WDK, with static CRT.
"@ | Set-Content "$out\SOURCES.txt"
& tar -czf out\looking-glass-windows-x64.tar.gz -C out looking-glass
& tar -czf out\looking-glass-windows-x64-symbols.tar.gz -C out symbols
