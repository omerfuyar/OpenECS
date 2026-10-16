# Installs what the Windows build needs: LLVM-MinGW with the UCRT runtime, whose clang shuild calls; and cmake and ninja for SDL.
# Each tool's folder is added to the PATH of the workflow's later steps, as winget changes the PATH of new sessions only.
$ErrorActionPreference = "Stop"

foreach ($id in "MartinStorsjo.LLVM-MinGW.UCRT", "Kitware.CMake", "Ninja-build.Ninja") {
    winget install --exact --id $id --silent --accept-source-agreements --accept-package-agreements --disable-interactivity
    if ($LASTEXITCODE -ne 0) { throw "winget cannot install $id" }
}

# winget puts LLVM-MinGW and ninja in its packages folder, and cmake in Program Files
$packages = Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Packages"
$clang = Get-ChildItem -Path $packages -Recurse -Filter clang.exe | Where-Object { $_.FullName -like "*LLVM-MinGW*\bin\clang.exe" } | Select-Object -First 1
$ninja = Get-ChildItem -Path $packages -Recurse -Filter ninja.exe | Select-Object -First 1
$cmake = Join-Path $env:ProgramFiles "CMake\bin"

if ($null -eq $clang -or $null -eq $ninja) { throw "LLVM-MinGW's clang or ninja is not found in $packages" }

$clang.DirectoryName | Out-File -FilePath $env:GITHUB_PATH -Encoding utf8 -Append
$ninja.DirectoryName | Out-File -FilePath $env:GITHUB_PATH -Encoding utf8 -Append
$cmake | Out-File -FilePath $env:GITHUB_PATH -Encoding utf8 -Append
