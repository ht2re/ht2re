#!/usr/bin/env bash
# Headless verification of ht2editor (docs/editor.md).
#
# It is a SEPARATE harness: the game's tools/shot_test.py is neither modified nor
# run here (run it yourself for the game's own regression suite).  Every check
# drives the real editor / game binaries and reads their real output.
#
#   usage: tools/editor_check.sh [ht2editor] [ht2re] [data-dir]
#
# Set HT2_EDITOR_SHOTS=DIR to keep the rendered frames (default: a temp dir).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ED="${1:-$ROOT/build/ht2editor}"
GAME="${2:-$ROOT/build/ht2re}"
DATA="${3:-$ROOT/original}"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/ht2editor.XXXXXX")"
OUT="${HT2_EDITOR_SHOTS:-$TMP}"
mkdir -p "$OUT"

pass=0
fail=0
ok()   { printf '  ok   %s -- %s\n' "$1" "$2"; pass=$((pass+1)); }
bad()  { printf '  FAIL %s -- %s\n' "$1" "$2"; fail=$((fail+1)); }
check(){ if [ "$2" = "1" ]; then ok "$1" "$3"; else bad "$1" "$3"; fi; }
s256() { shasum -a 256 "$1" 2>/dev/null | cut -d' ' -f1; }

# --- BMP helpers (24bpp, bottom-up, BGR - the format Renderer::screenshot writes)
bmp_diff() { # fileA fileB -> differing pixel count
  python3 - "$1" "$2" <<'PY'
import struct, sys
def rd(p):
    d=open(p,'rb').read()
    assert d[:2]==b'BM', p+" is not a BMP"
    off=struct.unpack_from('<I',d,10)[0]
    w,h=struct.unpack_from('<ii',d,18); bpp=struct.unpack_from('<H',d,28)[0]
    assert bpp==24, "unexpected bpp %d"%bpp
    flip=h>0; h=abs(h); stride=(w*3+3)&~3
    rows=[]
    for y in range(h):
        s=off+(h-1-y if flip else y)*stride; raw=d[s:s+w*3]
        rows.append([(raw[i*3+2],raw[i*3+1],raw[i*3+0]) for i in range(w)])
    return w,h,rows
wa,ha,A=rd(sys.argv[1]); wb,hb,B=rd(sys.argv[2])
assert (wa,ha)==(wb,hb), "size mismatch"
n=0
for y in range(ha):
    ra,rb=A[y],B[y]
    for x in range(wa):
        a,b=ra[x],rb[x]
        if abs(a[0]-b[0])>8 or abs(a[1]-b[1])>8 or abs(a[2]-b[2])>8: n+=1
print(n)
PY
}
bmp_colors() { # file -> distinct colour buckets (a rough "this frame has content")
  python3 - "$1" <<'PY'
import struct, sys
d=open(sys.argv[1],'rb').read()
off=struct.unpack_from('<I',d,10)[0]
w,h=struct.unpack_from('<ii',d,18); flip=h>0; h=abs(h); stride=(w*3+3)&~3
seen=set()
for y in range(0,h,2):
    s=off+(h-1-y if flip else y)*stride
    for x in range(0,w,2):
        i=s+x*3
        seen.add((d[i+2]//16, d[i+1]//16, d[i]//16))
print(len(seen))
PY
}

echo "ht2editor harness"
echo "editor: $ED"
echo "game:   $GAME"
echo "data:   $DATA"
echo "shots:  $OUT"
echo

# ---------------------------------------------------------------- binaries ---
check "both targets were built" \
  "$([ -x "$ED" ] && [ -x "$GAME" ] && echo 1 || echo 0)" "$ED + $GAME"

# The editor must never write to the shipped game data - prove it by hash.
WAY_H0=$(s256 "$DATA/ENV/aa.way")
B3D_H0=$(s256 "$DATA/COMMON/TRUCKS.B3D")

# ------------------------------------------------------------------- map -----
EMPTY="$TMP/empty.ovl"
MAP_LOG="$TMP/map_list.txt"
"$ED" --data "$DATA" --map aa --overlay "$EMPTY" --list >"$MAP_LOG" 2>&1
NODES=$(grep -c '^\[ht2re\] editor: node ' "$MAP_LOG")
SEGS=$(grep -c '^\[ht2re\] editor: seg ' "$MAP_LOG")
check "map: the node list is non-empty" "$([ "$NODES" -ge 10 ] && echo 1 || echo 0)" "$NODES nodes"
check "map: the segment list is non-empty" "$([ "$SEGS" -ge 10 ] && echo 1 || echo 0)" "$SEGS segments"
check "map: every node line carries name/position/flags" \
  "$(grep -q 'editor: node .* x=.* y=.* z=.* flags=0x' "$MAP_LOG" && echo 1 || echo 0)" \
  "$(grep -m1 'editor: node ' "$MAP_LOG" | sed 's/^\[ht2re\] //')"
check "map: the report line names the tile and the counts" \
  "$(grep -q 'editor: mode=map tile=aa nodes=' "$MAP_LOG" && echo 1 || echo 0)" \
  "$(grep -o 'editor: mode=map.*' "$MAP_LOG" | head -1)"

# --- 2D / 3D frames are not blank and differ from each other
"$ED" --data "$DATA" --map aa --overlay "$EMPTY" --blank --shot "$OUT/map_blank.bmp" >/dev/null 2>&1
"$ED" --data "$DATA" --map aa --overlay "$EMPTY" --select STO_AA --shot "$OUT/map_2d.bmp" >/dev/null 2>&1
"$ED" --data "$DATA" --map aa --overlay "$EMPTY" --view 3d \
      --cam 3000,1400,-3500,0,-45 --shot "$OUT/map_3d.bmp" >/dev/null 2>&1
D2=$(bmp_diff "$OUT/map_2d.bmp" "$OUT/map_blank.bmp")
D3=$(bmp_diff "$OUT/map_3d.bmp" "$OUT/map_blank.bmp")
D23=$(bmp_diff "$OUT/map_2d.bmp" "$OUT/map_3d.bmp")
C3=$(bmp_colors "$OUT/map_3d.bmp")
check "map: the 2D frame differs from a blank render" "$([ "$D2" -gt 5000 ] && echo 1 || echo 0)" "$D2 px"
check "map: the 3D frame differs from a blank render" "$([ "$D3" -gt 5000 ] && echo 1 || echo 0)" "$D3 px"
check "map: the 3D toggle changes the frame" "$([ "$D23" -gt 5000 ] && echo 1 || echo 0)" "$D23 px"
check "map: the 3D frame carries the tile's own colours" "$([ "$C3" -ge 8 ] && echo 1 || echo 0)" "$C3 colour buckets"

# --- edit + save + reload
OVL="$TMP/map.ovl"
cat > "$TMP/map.cmd" <<EOF
select STO_AA
moveto 4444.25 451.5 -5222.75
setflags 0x10700
addnode editor_new_node 4300 452 -5100
connect editor_new_node STO_AA 12.5
selectseg 0
setseg 0 width 22.25
save
EOF
"$ED" --data "$DATA" --map aa --overlay "$OVL" --script "$TMP/map.cmd" > "$TMP/map_edit.txt" 2>&1
check "map: the overlay file is created" "$([ -s "$OVL" ] && echo 1 || echo 0)" "$OVL"
check "map: the file carries the HT2EDIT header" \
  "$(head -1 "$OVL" | grep -q '^HT2EDIT 1$' && echo 1 || echo 0)" "$(head -1 "$OVL")"
check "map: the moved node is in the overlay" \
  "$(grep -q '^NODEPOS aa STO_AA 4444.2500 451.5000 -5222.7500$' "$OVL" && echo 1 || echo 0)" \
  "$(grep -m1 '^NODEPOS' "$OVL")"
check "map: the added node is in the overlay" \
  "$(grep -q '^NODEADD aa .* editor_new_node ' "$OVL" && echo 1 || echo 0)" \
  "$(grep -m1 '^NODEADD' "$OVL")"
check "map: the new connection is in the overlay" \
  "$(grep -q '^SEGADD aa editor_new_node__STO_AA ' "$OVL" && echo 1 || echo 0)" \
  "$(grep -m1 '^SEGADD' "$OVL" | cut -c1-52)"
check "map: the segment width edit is in the overlay" \
  "$(grep -q '^SEGEDIT aa 0 2 22.2500 8.0000$' "$OVL" && echo 1 || echo 0)" \
  "$(grep -m1 '^SEGEDIT' "$OVL")"

# reload: the moved node, the added node and the extra segment must come back
"$ED" --data "$DATA" --map aa --overlay "$OVL" --list > "$TMP/map_reload.txt" 2>&1
check "map: the saved move is re-loaded" \
  "$(grep -q 'editor: node STO_AA *x=4444.250 y=451.500 z=-5222.750' "$TMP/map_reload.txt" && echo 1 || echo 0)" \
  "$(grep -m1 'editor: node STO_AA ' "$TMP/map_reload.txt" | sed 's/^\[ht2re\] //')"
check "map: the saved node addition is re-loaded" \
  "$(grep -q 'editor: node editor_new_node ' "$TMP/map_reload.txt" && echo 1 || echo 0)" \
  "$(grep -m1 'editor: node editor_new_node ' "$TMP/map_reload.txt" | sed 's/^\[ht2re\] //')"
check "map: the saved width edit is re-loaded" \
  "$(grep -q 'editor: seg 0 .* w=22.25/' "$TMP/map_reload.txt" && echo 1 || echo 0)" \
  "$(grep -m1 'editor: seg 0 ' "$TMP/map_reload.txt" | sed 's/^\[ht2re\] //')"
RN=$(grep -m1 -o 'editor: mode=map tile=aa nodes=[0-9]*' "$TMP/map_reload.txt" | grep -o '[0-9]*$')
RS=$(grep -m1 -o 'live_segs=[0-9]*' "$TMP/map_reload.txt" | grep -o '[0-9]*')
check "map: the re-loaded tile has one more node and one more live segment" \
  "$([ "$RN" = "$((NODES+1))" ] && [ "$RS" = "$((SEGS+1))" ] && echo 1 || echo 0)" \
  "nodes=$RN (was $NODES), live_segs=$RS (was $SEGS)"
ABLINE=$("$ED" --data "$DATA" --map ab --overlay "$OVL" --list 2>&1 | grep -o 'editor: mode=map tile=ab.*' | head -1)
check "map: a tile the overlay does not name is untouched" \
  "$(printf '%s' "$ABLINE" | grep -q 'tile=ab nodes=' && echo 1 || echo 0)" "$ABLINE"

# --- the engine reads the same file
GLOG="$TMP/game_map.txt"
"$GAME" --data "$DATA" --scene world --screenshot --out "$TMP/game_map.bmp" \
        --spawn "4750,-5675,0" --warmup 2 --map --traffic 0 --overlay "$OVL" > "$GLOG" 2>&1
check "engine: --overlay loads and applies the map records" \
  "$(grep -qE 'overlay: tile aa: [1-9][0-9]* node record\(s\), [1-9][0-9]* segment record\(s\) applied' "$GLOG" && echo 1 || echo 0)" \
  "$(grep -m1 -o 'overlay: tile aa:.*' "$GLOG")"
"$GAME" --data "$DATA" --scene world --screenshot --out "$TMP/game_map0.bmp" \
        --spawn "4750,-5675,0" --warmup 2 --map --traffic 0 > "$TMP/game_map0.txt" 2>&1
check "engine: without --overlay nothing is applied" \
  "$(grep -q 'overlay: tile' "$TMP/game_map0.txt" && echo 0 || echo 1)" "no overlay log line"
SN=$(grep -o 'named .way nodes, [0-9]* road segments' "$TMP/game_map.txt" | grep -o '[0-9]* road' | grep -o '[0-9]*')
SN0=$(grep -o 'named .way nodes, [0-9]* road segments' "$TMP/game_map0.txt" | grep -o '[0-9]* road' | grep -o '[0-9]*')
check "engine: the added segment reaches the game's road graph" \
  "$([ "$SN" -gt "$SN0" ] && echo 1 || echo 0)" "$SN vs $SN0 road segments"
GD=$(bmp_diff "$TMP/game_map.bmp" "$TMP/game_map0.bmp")
check "engine: the overlay changes the rendered world frame" \
  "$([ "$GD" -gt 100 ] && echo 1 || echo 0)" "$GD px differ"

# ------------------------------------------------------------------ model ----
MODEL_LOG="$TMP/model_list.txt"
"$ED" --data "$DATA" --vehicle ZilR --overlay "$EMPTY" --list > "$MODEL_LOG" 2>&1
PARTS=$(grep -c '^\[ht2re\] editor: part ' "$MODEL_LOG")
ANCH=$(grep -c '^\[ht2re\] editor: anchor ' "$MODEL_LOG")
TRIS=$(grep -m1 -o 'mode=model vehicle=ZilR parts=[0-9]* submeshes=[0-9]* tris=[0-9]*' "$MODEL_LOG" | grep -o 'tris=[0-9]*' | grep -o '[0-9]*')
check "model: the part list is non-empty" "$([ "$PARTS" -ge 20 ] && echo 1 || echo 0)" "$PARTS parts"
check "model: the submesh list carries name/material/tris/bbox" \
  "$(grep -q 'editor: submesh .*material=.*tris=.*bbox x\[' "$MODEL_LOG" && echo 1 || echo 0)" \
  "$(grep -m1 'editor: submesh ' "$MODEL_LOG" | sed 's/^\[ht2re\] //' | cut -c1-88)"
check "model: the anchor list is non-empty" "$([ "$ANCH" -ge 50 ] && echo 1 || echo 0)" "$ANCH anchors"
check "model: the triangle count is reported" "$([ "${TRIS:-0}" -gt 1000 ] && echo 1 || echo 0)" "${TRIS:-?} tris"

"$ED" --data "$DATA" --vehicle ZilR --overlay "$EMPTY" --blank --shot "$OUT/model_blank.bmp" >/dev/null 2>&1
"$ED" --data "$DATA" --vehicle ZilR --overlay "$EMPTY" --angle 205,-18 --shot "$OUT/model_a.bmp" >/dev/null 2>&1
"$ED" --data "$DATA" --vehicle ZilR --overlay "$EMPTY" --angle 25,-10  --shot "$OUT/model_b.bmp" >/dev/null 2>&1
MA=$(bmp_diff "$OUT/model_a.bmp" "$OUT/model_blank.bmp")
MB=$(bmp_diff "$OUT/model_b.bmp" "$OUT/model_blank.bmp")
MAB=$(bmp_diff "$OUT/model_a.bmp" "$OUT/model_b.bmp")
CA=$(bmp_colors "$OUT/model_a.bmp")
check "model: the textured model differs from a blank render" "$([ "$MA" -gt 5000 ] && echo 1 || echo 0)" "$MA px"
check "model: the second angle also differs from blank" "$([ "$MB" -gt 5000 ] && echo 1 || echo 0)" "$MB px"
check "model: the two angles render differently" "$([ "$MAB" -gt 5000 ] && echo 1 || echo 0)" "$MAB px"
check "model: the frame is textured (many colour buckets)" "$([ "$CA" -ge 20 ] && echo 1 || echo 0)" "$CA colour buckets"

# --- part toggle + material swap + anchor edit, saved and re-loaded
MOVL="$TMP/model.ovl"
cat > "$TMP/model.cmd" <<'EOF'
part Kapot_0 hide
partmat Kapot_1 Zil_parts1
selectanchor ZilRWheel0Space
anchormove ZilRWheel0Space 0.25 0 -0.5
anchorrot ZilRWheel0Space 15 0 0
save
EOF
"$ED" --data "$DATA" --vehicle ZilR --overlay "$MOVL" --script "$TMP/model.cmd" > "$TMP/model_edit.txt" 2>&1
check "model: the overlay file is created" "$([ -s "$MOVL" ] && echo 1 || echo 0)" "$MOVL"
check "model: the hidden part is in the overlay" \
  "$(grep -q '^PART ZilR Kapot_0 hidden$' "$MOVL" && echo 1 || echo 0)" "$(grep -m1 '^PART ' "$MOVL")"
check "model: the material swap is in the overlay" \
  "$(grep -q '^PARTMAT ZilR Kapot_1 Zil_parts1$' "$MOVL" && echo 1 || echo 0)" "$(grep -m1 '^PARTMAT ' "$MOVL")"
check "model: the anchor translation is in the overlay" \
  "$(grep -q '^ANCHORT ZilR ZilRWheel0Space ' "$MOVL" && echo 1 || echo 0)" "$(grep -m1 '^ANCHORT ' "$MOVL")"
check "model: the anchor rotation is in the overlay" \
  "$(grep -q '^ANCHORM ZilR ZilRWheel0Space ' "$MOVL" && echo 1 || echo 0)" "$(grep -m1 '^ANCHORM ' "$MOVL" | cut -c1-60)"

"$ED" --data "$DATA" --vehicle ZilR --overlay "$MOVL" --list > "$TMP/model_reload.txt" 2>&1
check "model: the hidden part is re-loaded as hidden" \
  "$(grep -q 'editor: part Kapot_0 .*hidden=1' "$TMP/model_reload.txt" && echo 1 || echo 0)" \
  "$(grep -m1 'editor: part Kapot_0 ' "$TMP/model_reload.txt" | sed 's/^\[ht2re\] //')"
check "model: the material swap is re-loaded" \
  "$(grep -q 'editor: submesh Kapot_1 .*material=Zil_parts1' "$TMP/model_reload.txt" && echo 1 || echo 0)" \
  "$(grep -m1 'editor: submesh Kapot_1 ' "$TMP/model_reload.txt" | sed 's/^\[ht2re\] //' | cut -c1-72)"
check "model: the anchor move is re-loaded" \
  "$(grep -q 'editor: anchor ZilRWheel0Space *t=(-0.600 2.100 -0.890)' "$TMP/model_reload.txt" && echo 1 || echo 0)" \
  "$(grep -m1 'editor: anchor ZilRWheel0Space' "$TMP/model_reload.txt" | sed 's/^\[ht2re\] //' | cut -c1-52)"

# --- the engine reads the same file
GMLOG="$TMP/game_model.txt"
"$GAME" --data "$DATA" --scene vehicle --vehicle ZilR --screenshot \
        --out "$TMP/game_model.bmp" --time 0 --overlay "$MOVL" > "$GMLOG" 2>&1
check "engine: --overlay applies the vehicle anchor edits" \
  "$(grep -q 'overlay: vehicle ZilR: [1-9][0-9]* anchor(s) overridden' "$GMLOG" && echo 1 || echo 0)" \
  "$(grep -m1 -o 'overlay: vehicle ZilR:.*' "$GMLOG")"
check "engine: --overlay applies the part/material edits" \
  "$(grep -q 'overlay: vehicle ZilR: .*recoloured, [1-9][0-9]* hidden' "$GMLOG" && echo 1 || echo 0)" \
  "$(grep -m1 -o 'overlay: vehicle ZilR:.*' "$GMLOG")"
"$GAME" --data "$DATA" --scene vehicle --vehicle ZilR --screenshot \
        --out "$TMP/game_model0.bmp" --time 0 > /dev/null 2>&1
MD=$(bmp_diff "$TMP/game_model.bmp" "$TMP/game_model0.bmp")
check "engine: the vehicle overlay changes the rendered frame" \
  "$([ "$MD" -gt 100 ] && echo 1 || echo 0)" "$MD px differ"

# --- the intact/damaged skin toggle the container is built around
FOVL="$TMP/fiat.ovl"
FLOG="$TMP/fiat_edit.txt"
cat > "$TMP/fiat.cmd" <<'EOF'
part Capot_2 hide
part FR_2 hide
part BackC_2 hide
partmat Capot_0 Fiat_alld1
save
EOF
"$ED" --data "$DATA" --vehicle Fiat --overlay "$FOVL" --script "$TMP/fiat.cmd" > "$FLOG" 2>&1
NHIDE=$(grep -c '^PART Fiat .* hidden$' "$FOVL")
check "skin: the dented variant parts are hidden in the overlay" \
  "$(grep -q '^PART Fiat Capot_2 hidden$' "$FOVL" && [ "$NHIDE" -ge 3 ] && echo 1 || echo 0)" \
  "$NHIDE hidden parts"
FSUB0=$("$ED" --data "$DATA" --vehicle Fiat --overlay "$EMPTY" --list 2>&1 | grep -m1 -o 'mode=model vehicle=Fiat parts=[0-9]* submeshes=[0-9]*' | grep -o 'submeshes=[0-9]*' | grep -o '[0-9]*')
FSUB=$(grep -o 'mode=model vehicle=Fiat parts=[0-9]* submeshes=[0-9]*' "$FLOG" | tail -1 | grep -o 'submeshes=[0-9]*' | grep -o '[0-9]*')
check "skin: hiding them removes submeshes from the assembly" \
  "$([ "${FSUB:-0}" -lt "${FSUB0:-0}" ] && echo 1 || echo 0)" \
  "$FSUB submeshes, was $FSUB0" 
"$GAME" --data "$DATA" --scene vehicle --vehicle Fiat --screenshot \
        --out "$TMP/game_fiat.bmp" --time 0 --overlay "$FOVL" > "$TMP/game_fiat.txt" 2>&1
"$GAME" --data "$DATA" --scene vehicle --vehicle Fiat --screenshot \
        --out "$TMP/game_fiat0.bmp" --time 0 > /dev/null 2>&1
FD=$(bmp_diff "$TMP/game_fiat.bmp" "$TMP/game_fiat0.bmp")
check "skin: the game renders the intact-only skin from the overlay" \
  "$([ "$FD" -gt 1000 ] && echo 1 || echo 0)" "$FD px differ"

# ------------------------------------------------------- shipped data intact -
if [ "$OUT" != "$TMP" ]; then
  cp "$OVL" "$OUT/map.ovl" 2>/dev/null
  cp "$MOVL" "$OUT/model.ovl" 2>/dev/null
  cp "$FOVL" "$OUT/fiat.ovl" 2>/dev/null
fi
check "the shipped .way is byte-identical after every edit" \
  "$([ "$(s256 "$DATA/ENV/aa.way")" = "$WAY_H0" ] && echo 1 || echo 0)" "sha256 ${WAY_H0:0:16}..."
check "the shipped TRUCKS.B3D is byte-identical after every edit" \
  "$([ "$(s256 "$DATA/COMMON/TRUCKS.B3D")" = "$B3D_H0" ] && echo 1 || echo 0)" "sha256 ${B3D_H0:0:16}..."

echo
if [ "$fail" -ne 0 ]; then
  echo "FAILED: $fail check(s) failed, $pass passed"
  exit 1
fi
echo "all editor checks passed ($pass ok, 0 FAIL)"
[ -n "${HT2_EDITOR_SHOTS:-}" ] && echo "frames written to $OUT"
exit 0
