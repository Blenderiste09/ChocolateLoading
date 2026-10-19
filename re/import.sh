#!/bin/bash
# Usage: re/import.sh <proj> <windows-path-without-spaces> [cpus] [maxmem]
# Imports + auto-analyzes one binary into re/ghidra_projects/<proj> (one project per binary).
P="$1"; F="$2"; CPU="${3:-4}"; export GHIDRA_HEADLESS_MAXMEM="${4:-6G}"
R="D:/OpenCode/ChocolateLoading/re"
mkdir -p "/d/OpenCode/ChocolateLoading/re/ghidra_projects/$P"
cmd //c "D:\ghidra\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat $R/ghidra_projects/$P $P -import $F -overwrite -max-cpu $CPU" > "/d/OpenCode/ChocolateLoading/re/out/import_$P.log" 2>&1
tail -3 "/d/OpenCode/ChocolateLoading/re/out/import_$P.log"
