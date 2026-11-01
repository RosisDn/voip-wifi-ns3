#!/usr/bin/env bash
# run_all.sh - kör alla experiment för VoIP över WiFi.
#
# Lägg filen i ns-3-mappen (~/ns-allinone-3.48/ns-3.48) och kör:
#   chmod +x run_all.sh
#   ./run_all.sh
#
# Resultatet hamnar i voip-results.csv i samma mapp.
# Varje körning lägger till två rader (nedlänk och upplänk).

set -e

# Gå till mappen där skriptet ligger (ns-3-mappen)
cd "$(dirname "$0")"

RESULTS="$PWD/voip-results.csv"
N_VALUES="0 2 5 10 20"     # antal nedladdare
RUNS=10                    # repetitioner per N
QOS_VALUES="0"           # 0 = best effort, 1 = röstkö (AC_VO)

# Bygg en gång, så att varje körning går snabbt
./ns3 build

total=$(( $(echo $N_VALUES | wc -w) * RUNS * $(echo $QOS_VALUES | wc -w) ))
count=0

for q in $QOS_VALUES; do
  for n in $N_VALUES; do
    for r in $(seq 1 $RUNS); do
      count=$((count + 1))
      # Hoppa över körningar som redan finns (om skriptet startats om)
      if [ -f "$RESULTS" ] && grep -q "^$n,$r,$q,down," "$RESULTS"; then
        echo "[$count/$total] N=$n run=$r qos=$q finns redan, hoppar över"
        continue
      fi
      echo "[$count/$total] N=$n run=$r qos=$q"
      ./ns3 run --no-build "voip-wifi --nDownloaders=$n --RngRun=$r --qos=$q --outFile=$RESULTS"
    done
  done
done

echo "Klart! Resultat i $RESULTS"
