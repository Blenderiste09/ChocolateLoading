# Helper around Ghidra headless for the Portal 2 RE projects.
#   .\re\ghidra.ps1 import <proj> <binary path>        # import + full auto-analysis (slow, once)
#   .\re\ghidra.ps1 query  <proj> <cmdfile> [outfile]  # run P2Query.java commands (see script header)
# Projects live in re\ghidra_projects\<proj>\ (one project per binary so queries can run in parallel).
param(
    [Parameter(Mandatory=$true)][ValidateSet("import","query")][string]$Mode,
    [Parameter(Mandatory=$true)][string]$Proj,
    [Parameter(Mandatory=$true)][string]$Arg1,
    [string]$Arg2
)
$ErrorActionPreference = "Stop"
$root    = Split-Path -Parent $MyInvocation.MyCommand.Path
$ghidra  = "D:\ghidra\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat"
$projDir = Join-Path $root "ghidra_projects\$Proj"
$scripts = Join-Path $root "ghidra_scripts"
New-Item -ItemType Directory -Force $projDir | Out-Null

if ($Mode -eq "import") {
    $env:GHIDRA_HEADLESS_MAXMEM = "6G"
    $log = Join-Path $root "out\import_$Proj.log"
    & $ghidra $projDir $Proj -import $Arg1 -overwrite -max-cpu 4 *> $log
    "import finished (exit $LASTEXITCODE), log: $log"
} else {
    $env:GHIDRA_HEADLESS_MAXMEM = "4G"
    $cmd = (Resolve-Path $Arg1).Path
    if (-not $Arg2) { $Arg2 = Join-Path $root "out\q_$Proj.txt" }
    if (Test-Path $Arg2) { Remove-Item $Arg2 }
    $prog = (Get-ChildItem $projDir -Filter *.rep -Directory | Select-Object -First 1)
    $log = Join-Path $root "out\query_$Proj.log"
    # the program name inside the project is the imported file name
    $name = (Get-Content (Join-Path $root "ghidra_projects\$Proj.progname") -ErrorAction SilentlyContinue)
    if (-not $name) { throw "missing $Proj.progname (file name of the imported binary)" }
    & $ghidra $projDir $Proj -process $name -noanalysis -readOnly -scriptPath $scripts -postScript P2Query.java $cmd $Arg2 *> $log
    if (Test-Path $Arg2) { Get-Content $Arg2 -Raw } else { "no output; see $log"; Get-Content $log -Tail 30 }
}
