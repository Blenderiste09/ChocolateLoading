# Builds build\chocolateloading.dll (x86, static CRT) and build\tools\*.exe (the chocoload launcher and
# the diagnostic tools) with the Visual Studio 18 toolchain.
#   .\build.ps1            build
#   .\build.ps1 -Test      also build and run the arena stress test (no game needed)
#   .\build.ps1 -Deploy    also copy the DLL (+ .pdb) and chocoload.exe to the Portal 2 folder
param([switch]$Deploy, [switch]$Test, [string]$GameDir = "E:\Program Files (x86)\Steam\steamapps\common\Portal 2")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

# Import the x86 MSVC environment once per shell.
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vcvars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"
    if (-not (Test-Path $vcvars)) {
        $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
        $inst = & $vswhere -latest -products * -property installationPath
        $vcvars = Join-Path $inst "VC\Auxiliary\Build\vcvars32.bat"
    }
    cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
    }
}

$out = Join-Path $root "build"
New-Item -ItemType Directory -Force $out | Out-Null
$src = Get-ChildItem (Join-Path $root "src") -Filter *.cpp | ForEach-Object { $_.FullName }
# /Oy- keeps frame pointers: vatrack walks the call stack (RtlCaptureStackBackTrace) out of its hook.
$common = @("/nologo", "/O2", "/Oy-", "/MT", "/EHsc", "/W3", "/Zi", "/std:c++17", "/DWIN32_LEAN_AND_MEAN")

Push-Location $out
try {
    & cl.exe @common /LD @src /Fe:chocolateloading.dll /Fd:chocolateloading_cl.pdb /link /DEBUG /OPT:REF /OPT:ICF kernel32.lib /PDB:chocolateloading.pdb
    if ($LASTEXITCODE -ne 0) { throw "build failed" }

    $toolsOut = Join-Path $out "tools"
    New-Item -ItemType Directory -Force $toolsOut | Out-Null
    Push-Location $toolsOut
    try {
        foreach ($t in Get-ChildItem (Join-Path $root "tools") -Filter *.cpp) {
            $log = & cl.exe /nologo /O2 /MT /W3 /EHsc /D_CRT_SECURE_NO_WARNINGS $t.FullName "/Fe:$($t.BaseName).exe" /link psapi.lib
            $log | Select-String -Pattern "warning|error" | ForEach-Object { Write-Host $_.Line }
            if ($LASTEXITCODE -ne 0) { throw "tool build failed: $($t.Name)" }
        }
    } finally { Pop-Location }

    if ($Test) {
        & cl.exe @common (Join-Path $root "tests\arena_test.cpp") (Join-Path $root "src\arena.cpp") /Fe:arena_test.exe /link /LARGEADDRESSAWARE
        if ($LASTEXITCODE -ne 0) { throw "test build failed" }
        & .\arena_test.exe
        if ($LASTEXITCODE -ne 0) { throw "arena test failed" }
    }
} finally { Pop-Location }

if ($Deploy) {
    Copy-Item (Join-Path $out "chocolateloading.dll") (Join-Path $GameDir "chocolateloading.dll") -Force
    Copy-Item (Join-Path $out "chocolateloading.pdb") (Join-Path $GameDir "chocolateloading.pdb") -Force
    Copy-Item (Join-Path $out "tools\chocoload.exe") (Join-Path $GameDir "chocoload.exe") -Force
    Write-Host "deployed to $GameDir"
}
