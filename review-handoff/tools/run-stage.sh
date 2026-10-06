#!/bin/bash
# usage: run-stage.sh <name>   (builds and tests $S/<name> in Debug then Release)
S=/tmp/claude-0/-home-user-Inventatory-Software/12d62aab-f424-56dd-b06c-ff8666ba88ef/scratchpad
N=$1
$S/fulltest.sh $S/$N $N Debug > $S/$N-debug.out 2>&1
$S/fulltest.sh $S/$N $N Release > $S/$N-release.out 2>&1
echo ALLDONE >> $S/$N-release.out
