# Installs what the Windows build needs: LLVM-MinGW with the UCRT runtime, whose clang shuild calls; and cmake and ninja for SDL.
# cmake or ninja on the PATH already, as cmake is on GitHub's runners, is kept; LLVM-MinGW is always installed, as another clang,
# such as Visual Studio's, compiles for MSVC. Each installed tool's folder goes first on the PATH of the workflow's later steps,
# as winget changes the PATH of new sessions only.
$ErrorActionPreference = "Stop"

function Install-Tool([string]$id, [string]$command, [string]$filter, [bool]$keep)
{
    if ($keep -and $null -ne (Get-Command $command -ErrorAction SilentlyContinue))
    {
        Write-Output "$command is on the PATH already."
        return
    }

    winget install --exact --id $id --silent --accept-source-agreements --accept-package-agreements --disable-interactivity
    if ($LASTEXITCODE -ne 0) { throw "winget cannot install $id" }

    # winget puts packages in its packages folder, or in Program Files
    $folders = @((Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Packages"), $env:ProgramFiles)
    $found = Get-ChildItem -Path $folders -Recurse -Filter "$command.exe" -ErrorAction SilentlyContinue | Where-Object { $_.FullName -like $filter } | Select-Object -First 1
    if ($null -eq $found) { throw "$command.exe of $id is not found" }

    $found.DirectoryName | Out-File -FilePath $env:GITHUB_PATH -Encoding utf8 -Append
}

Install-Tool "MartinStorsjo.LLVM-MinGW.UCRT" "clang" "*llvm-mingw*\bin\clang.exe" $false
Install-Tool "Kitware.CMake" "cmake" "*\bin\cmake.exe" $true
Install-Tool "Ninja-build.Ninja" "ninja" "*\ninja.exe" $true
