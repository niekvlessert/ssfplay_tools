#!/usr/bin/env bash
set -euo pipefail

SSF2VGM="/Volumes/EXT_SSD/AI/ssf2vgm/build/ssf2vgm"
OUTDIR="/Volumes/EXT_SSD/AI/ssf2vgm/build/all-vgm-180s"

mkdir -p "$OUTDIR"

while IFS= read -r ssf; do
    base="$(basename "$ssf" .ssf)"
    vgm="${OUTDIR}/${base}.vgm"
    
    echo "Converting: $ssf -> $vgm"
    "$SSF2VGM" --length-ms 180000 "$ssf" "$vgm"
    
    echo "Compressing: $vgm"
    vgm_cmp "$vgm"
    gzip "$vgm"
    echo "Done: ${vgm}.gz"
done < <(find . -name '*.ssf' -not -path './build*' -type f | sort)

echo "All done. Output in $OUTDIR"
