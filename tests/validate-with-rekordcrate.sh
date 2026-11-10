#!/usr/bin/env bash
# Builds a sample stick with the Pioneer writer (cdj_tests --make-stick) and
# reads every Pioneer file on it back with rekordcrate
# (github.com/Holzhaus/rekordcrate), an independent Rust parser of export.pdb,
# ANLZ files and *SETTING.DAT. Fails on any parse error, and on any value
# rekordcrate reads back that differs from what was written (titles,
# artists, paths, tempo, playlists in order, beat grid, cue times, names,
# colours and loops).
#
#   tests/validate-with-rekordcrate.sh [work folder]
#
# Needs: a configured build folder (build/), Rust (mise: `mise use -g
# rust@latest`) and python3. rekordcrate is cloned into ~/src/rekordcrate
# (or $REKORDCRATE_DIR) and built the first time.

set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
work="${1:-$(mktemp -d -t cdj-rekordcrate-XXXXXX)}"
rc_dir="${REKORDCRATE_DIR:-$HOME/src/rekordcrate}"
rc="$rc_dir/target/release/rekordcrate"

if [[ ! -x "$rc" ]]; then
    [[ -d "$rc_dir" ]] || git clone https://github.com/Holzhaus/rekordcrate.git "$rc_dir"
    (cd "$rc_dir" && mise exec rust@latest -- cargo build --release)
fi

cmake --build "$here/build" --target cdj_tests -j8 > /dev/null
tests="$(find "$here/build" -type f -name cdj_tests -perm -u+x | head -n 1)"

mkdir -p "$work"
"$tests" --make-stick "$work/stick" "$work/expected.json"
pioneer="$work/stick/PIONEER"
mkdir -p "$work/dump"

echo "== export.pdb"
"$rc" dump-pdb "$pioneer/rekordbox/export.pdb" > "$work/dump/export.pdb.txt"
"$rc" dump-pdb --parse-unknown-tables "$pioneer/rekordbox/export.pdb" > /dev/null
"$rc" dump-pdb -f json "$pioneer/rekordbox/export.pdb" > "$work/dump/export.pdb.json"
"$rc" list-playlists "$pioneer/rekordbox/export.pdb"
echo "parsed: export.pdb (all tables, also the unknown ones)"

echo "== ANLZ"
while IFS= read -r f; do
    rel="${f#"$pioneer"/}"
    out="$work/dump/${rel//\//_}.json"
    "$rc" dump-anlz -f json "$f" > "$out"
    echo "parsed: $rel"
done < <(find "$pioneer/USBANLZ" -type f -name 'ANLZ*' | sort)

echo "== settings"
for f in DEVSETTING MYSETTING MYSETTING2 DJMMYSETTING; do
    "$rc" dump-setting -f json "$pioneer/$f.DAT" > "$work/dump/$f.json"
    echo "parsed: $f.DAT"
    # rekordbox's own defaults, from the empty rekordbox export in rekordcrate's test data.
    reference="$rc_dir/data/complete_export/empty/PIONEER/$f.DAT"

    if [[ -f "$reference" && "$f" == MYSETTING ]]; then
        # Ours differs on purpose in one setting: HOT CUE COLOR is On.
        "$rc" dump-setting -f json "$reference" > "$work/dump/$f.reference.json"
        python3 -I - "$work/dump/$f.json" "$work/dump/$f.reference.json" <<'PY'
import json, sys
ours, ref = (json.load(open(p)) for p in sys.argv[1:3])
def flat(d, prefix=""):
    out = {}
    for k, v in (d.items() if isinstance(d, dict) else []):
        out.update(flat(v, prefix + k + ".") if isinstance(v, dict) else {prefix + k: v})
    return out
a, b = flat(ours), flat(ref)
diff = sorted(k for k in set(a) | set(b) if a.get(k) != b.get(k))
hot = [k for k in diff if k.endswith("hotcue_color")]
assert hot and all(str(a[k]) == "On" for k in hot), "HOT CUE COLOR is not On: %r" % diff
assert all(k.endswith("hotcue_color") or k.endswith("checksum") for k in diff), "other settings differ: %r" % diff
PY
        echo "        rekordbox's default MYSETTING.DAT except HOT CUE COLOR = On"
    elif [[ -f "$reference" ]]; then
        cmp "$pioneer/$f.DAT" "$reference"
        echo "        byte for byte the same as rekordbox's default $f.DAT"
    fi
done
"$rc" list-settings "$work/stick" > /dev/null

echo "== values"
python3 -I - "$work/expected.json" "$work/dump" "$pioneer" <<'EOF'
import json, math, os, sys

expected = json.load(open(sys.argv[1]))
dump, pioneer = sys.argv[2], sys.argv[3]
errors = []

def fail(message):
    errors.append(message)

def same(what, ours, theirs):
    if ours != theirs:
        fail(f"{what}: wrote {ours!r}, rekordcrate read {theirs!r}")

pdb = json.load(open(os.path.join(dump, "export.pdb.json")))
rows = {}
for table in pdb["tables"]:
    for page in table["pages"]:
        content = page["content"]
        if "Data" not in content:
            continue
        for row in content["Data"]["rows"].values():
            (kind, value), = row["Plain"].items()
            rows.setdefault(kind, []).append(value)

artists = {a["id"]: a["offsets"]["inner"]["name"] for a in rows.get("Artist", [])}
tracks = {t["id"]: t for t in rows.get("Track", [])}
same("track count", len(expected["tracks"]), len(tracks))

for e in expected["tracks"]:
    t = tracks.get(e["id"])
    if t is None:
        fail(f"track {e['id']} missing")
        continue
    s = t["offsets"]["inner"]
    name = e["title"]
    same(f"{name}: title", e["title"], s["title"])
    same(f"{name}: artist", e["artist"], artists.get(t["artist_id"], ""))
    same(f"{name}: file path", e["path"], s["file_path"])
    same(f"{name}: file name", e["fileName"], s["filename"])
    same(f"{name}: analyze path", e["anlz"], s["analyze_path"])
    same(f"{name}: tempo", e["tempo"], t["tempo"])
    same(f"{name}: sample rate", e["sampleRate"], t["sample_rate"])
    same(f"{name}: bitrate", e["bitrate"], t["bitrate"])
    same(f"{name}: file size", e["bytes"], t["file_size"])
    same(f"{name}: duration", math.floor(e["seconds"]), t["duration"])
    same(f"{name}: file type", "Mp3" if e["fileType"] == "mp3" else "Wav", t["file_type"])

nodes = sorted(rows.get("PlaylistTreeNode", []), key=lambda n: n["sort_order"])
same("playlists", [p["name"] for p in expected["playlists"]], [n["name"] for n in nodes])
for p, n in zip(expected["playlists"], nodes):
    entries = sorted((r for r in rows.get("PlaylistEntry", []) if r["playlist_id"] == n["id"]),
                     key=lambda r: r["entry_index"])
    same(f"playlist {p['name']}: order", p["ids"], [r["track_id"] for r in entries])

history = rows.get("History", [{}])[0]
same("Songs count on the property row", len(expected["tracks"]), history.get("num_tracks"))

def sections(path):
    name = path.lstrip("/").removeprefix("PIONEER/").replace("/", "_") + ".json"
    data = json.load(open(os.path.join(dump, name)))
    return [(s["header"]["kind"], s["content"]) for s in data["sections"]]

for e in expected["tracks"]:
    name = e["title"]
    base = e["anlz"][:-len(".DAT")]
    dat, ext, two = (sections(base + x) for x in (".DAT", ".EXT", ".2EX"))

    for label, file in (("DAT", dat), ("EXT", ext), ("2EX", two)):
        path = [c["Path"]["path"] for k, c in file if k == "Path"]
        same(f"{name}: {label} PPTH", [e["path"]], path)

    beats = [c["BeatGrid"]["beats"] for k, c in dat if k == "BeatGrid"][0]
    if e["tempo"] == 0:
        same(f"{name}: no beat grid", 0, len(beats))
    else:
        downbeat = next(b for b in beats if b["beat_number"] == 1)
        same(f"{name}: first downbeat", round(e["firstBeatMs"]), downbeat["time"])
        same(f"{name}: grid tempo", {e["tempo"]}, {b["tempo"] for b in beats})
        period = 6000000 / e["tempo"]
        for a, b in zip(beats, beats[1:]):
            if abs(b["time"] - a["time"] - period) > 1 or b["beat_number"] != a["beat_number"] % 4 + 1:
                fail(f"{name}: beat grid not even at {a['time']} ms")
                break
        expected_beats = e["seconds"] * e["tempo"] / 6000
        if abs(len(beats) - expected_beats) > 2:
            fail(f"{name}: {len(beats)} beats, about {expected_beats:.1f} expected")

    hot = sorted((c for c in e["cues"] if c["hot"] > 0), key=lambda c: c["hot"])
    memory = sorted((c for c in e["cues"] if c["hot"] == 0), key=lambda c: c["start"])

    def loop_end(cue):
        return 0xFFFFFFFF if cue["end"] < 0 else cue["end"]

    def plain(cues):
        return [(c["hot"], c["start"], loop_end(c), "Loop" if c["end"] >= 0 else "Point") for c in cues]

    def pcpt(file, list_type):
        lists = [c["CueList"] for k, c in file if k == "CueList" and c["CueList"]["list_type"] == list_type]
        return [(c["hot_cue"], c["time"], c["loop_time"], c["cue_type"]) for c in lists[0]["cues"]]

    same(f"{name}: .DAT hot cues A-C", plain([c for c in hot if c["hot"] <= 3]), pcpt(dat, "HotCues"))
    same(f"{name}: .EXT hot cues D-H", plain([c for c in hot if c["hot"] > 3]), pcpt(ext, "HotCues"))
    same(f"{name}: .DAT memory cues", plain(memory), pcpt(dat, "MemoryCues"))

    def pcp2(list_type):
        lists = [c["ExtendedCueList"] for k, c in ext if k == "ExtendedCueList"
                 and c["ExtendedCueList"]["list_type"] == list_type]
        return [(c["hot_cue"], c["time"], c["loop_time"], c["comment"], c["hot_cue_color_index"])
                for c in lists[0]["cues"]]

    same(f"{name}: PCO2 hot cues (names, colours)",
         [(c["hot"], c["start"], loop_end(c), c["name"], c["colour"]) for c in hot], pcp2("HotCues"))
    same(f"{name}: PCO2 memory cues",
         [(0, c["start"], loop_end(c), c["name"], 0) for c in memory], pcp2("MemoryCues"))

    kinds = [k if isinstance(k, str) else "Unknown" for k, _ in dat + ext + two]
    for wanted in ("VBR", "WaveformPreview", "TinyWaveformPreview", "WaveformDetail", "WaveformColorPreview",
                   "WaveformColorDetail", "Waveform3BandPreview", "Waveform3BandDetail", "Waveform3BandCalibration"):
        if wanted not in kinds:
            fail(f"{name}: no {wanted}")

    detail = [c["WaveformDetail"]["data"] for k, c in ext if k == "WaveformDetail"][0]
    columns = math.ceil(e["seconds"] * 150)
    if abs(len(detail) - columns) > 150:
        fail(f"{name}: {len(detail)} detail columns, about {columns} expected")
    three_band = [c["Waveform3BandDetail"]["data"] for k, c in two if k == "Waveform3BandDetail"][0]
    same(f"{name}: 3-band detail columns", len(detail), len(three_band))
    print(f"ok: {name}: {len(beats)} beats, {len(hot)} hot + {len(memory)} memory cues, {len(detail)} waveform columns")

if errors:
    print("\n".join("FAIL: " + e for e in errors))
    sys.exit(1)
print("rekordcrate reads back everything as written")
EOF

echo "All Pioneer files parsed by rekordcrate. Stick and dumps in $work"
