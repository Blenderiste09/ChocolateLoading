#!/bin/bash
# Usage: re/query.sh <proj> <cmdfile>   -> prints results (see ghidra_scripts/P2Query.java for commands)
P="$1"; C="$(cygpath -w "$(realpath "$2")")"
O="/d/OpenCode/ChocolateLoading/re/out/q_${P}_$$.txt"; OW="$(cygpath -w "$O")"
NAME="$(cat /d/OpenCode/ChocolateLoading/re/ghidra_projects/$P.progname)"
export GHIDRA_HEADLESS_MAXMEM=4G
R="D:/OpenCode/ChocolateLoading/re"
cmd //c "D:\ghidra\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat $R/ghidra_projects/$P $P -process $NAME -noanalysis -readOnly -scriptPath $R/ghidra_scripts -postScript P2Query.java $C $OW" > "/d/OpenCode/ChocolateLoading/re/out/query_$P.log" 2>&1
if [ -f "$O" ]; then cat "$O"; rm -f "$O"; else echo "NO OUTPUT"; tail -20 "/d/OpenCode/ChocolateLoading/re/out/query_$P.log"; fi
