#!/usr/bin/env bash
#
# Everything that can be checked without a host, in one go, in the order that
# fails fastest.
#
#   tools/verify.sh
#
# Each check answers a question none of the others can:
#
#   build         a FRESH universal Release build. Not the dev build: CMake
#                 latches the architecture list at the first target, so the
#                 only build worth measuring is one configured from nothing.
#   shaders       does every shader compile, through a real GLSL compiler
#                 (tools/check-shaders.sh, which CI runs too), on the exact
#                 strings `cctest --dump-shaders` writes.
#   demo          the browser demo's copy of every shader is still the
#                 plugin's, character for character (demo/tools/check_shaders.py).
#   offline       the checks that need no GL:
#                   --laws        every control law against its statement,
#                                 the OETF, the matrices and the drift
#                   --names       nothing a host will silently truncate; the
#                                 host reads SW CCU / CC01 / effect
#   physics       every rendering check, at TWO rasters: 320x180, which is
#                 what CI renders at, and 1280x720. Each is measured out of
#                 the picture, against a tolerance derived per operation:
#                   --identity    every stage at its null returns the input,
#                                 to the OETF round trip's float bound; alpha
#                                 bitwise; and again after a resize mid-run
#                   --detail      a step's overshoot is Detail Level x h / 4
#                                 and lasts the spacing, whole and
#                                 fractional, H and V
#                   --coring      an edge below Coring gets no detail; one
#                                 above gets the overshoot less the dead zone
#                   --knee        above the point a ramp's slope is Knee
#                                 Slope, continuous at the point, per channel
#                   --gamma       a ramp follows the OETF at three exponents;
#                                 Black Gamma lifts only below its level
#                   --order       WB then knee compresses a warm white in R;
#                                 knee-first predicts otherwise; knee off, R
#                                 clips first
#                   --skin        detail gain 1 - Skin Detail inside the hue
#                                 window, 1 outside, 1 on a neutral
#                   --negative    every one of those FAILS on a perturbed
#                                 chain
#                   --cpu         the OpenFX build's CPU copy of the two
#                                 passes agrees with the GPU in float, at
#                                 the defaults, at settings that move every
#                                 stage, at a drifting frame and under every
#                                 perturbation; and a control case differs
#   pipe          the fleet's --pipe contract: whole frames only, a cue naming
#                 no control refused, and exit 1 -- not a silent 0, not a
#                 SIGPIPE 141 -- on a failed render or a closed stdout, the
#                 last proved with `| head -c 1`.
#   sweep         does every control change the picture.
#   bench         the render cost, for the record. Not pass/fail.
#   registration  does the bundle contain a plugin at all -- a file-scope
#                 CFFGLPluginInfo nothing names, which a linker may drop while
#                 still producing a bundle that loads and exports plugMain.
#   lipo          is the build really universal.
#   plist         does CFBundleExecutable name the binary that is on disk.
#   codesign      the exact command the release job runs, against a copy.
#   oxbow         a real FFGL host loads the bundle and reports the name, id
#                 and type it sees -- the name field is not null-terminated
#                 and a host truncates silently past 16 characters.
#   openfx        the CCU.ofx bundle: Info.plist names the binary on disk, it
#                 exports OfxGetPlugin, it is universal, it ad-hoc signs (the
#                 release step); an OFX host (ofxprobe) finds THIS build under
#                 the identifier, loads it and renders; and the FFGL and OFX
#                 builds render the same picture byte for byte through their
#                 real entry points (tools/ofx_agree.py), with a control case
#                 that must differ. With OFXHOST set to a test host that has
#                 `--quirks fusion`: the bundle renders with no frame rate
#                 anywhere (stricter than Resolve's Fusion page, which gives
#                 the effect one) and equals the 24 fps render.
#
set -uo pipefail

cd "$(dirname "$0")/.."

# resolume-ofx-bridge, for ofxprobe. It sits beside this repo's checkout --
# and from a git worktree `..` is the worktrees folder, not Projects/resolume,
# so the main checkout is found through git's common dir as well.
# CCU_BRIDGE overrides both, and OFXPROBE the probe itself.
BRIDGE="${CCU_BRIDGE:-}"
if [ -z "$BRIDGE" ]; then
	for candidate in "../resolume-ofx-bridge" \
	                 "$(dirname "$(git rev-parse --path-format=absolute --git-common-dir 2>/dev/null)")/../resolume-ofx-bridge"; do
		if [ -d "$candidate/build" ]; then
			BRIDGE="$candidate"
			break
		fi
	done
fi
BRIDGE="${BRIDGE:-../resolume-ofx-bridge}"

BUILD="${BUILD:-build-universal}"
failures=0

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
pass() { printf '   \033[32mok\033[0m   %s\n' "$1"; }
fail() { printf '   \033[31mFAIL\033[0m %s\n' "$1"; failures=$(( failures + 1 )); }

step "build (fresh universal Release, $BUILD)"
rm -rf "$BUILD"
if cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 \
   && cmake --build "$BUILD" --parallel 4 >/dev/null 2>&1; then
	pass "builds"
else
	fail "build failed -- run: cmake -B $BUILD -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD"
	exit 1
fi

CCTEST="$BUILD/cctest"

step "shaders"
if out=$(tools/check-shaders.sh "$CCTEST" 2>&1); then
	pass "$( printf '%s\n' "$out" | tail -1 | sed 's/^ *//' )"
else
	fail "a shader does not compile"
	printf '%s\n' "$out"
fi

step "demo: the browser copy of the shaders"
# demo/plugin.js cannot include a C++ file, so it carries its own copy of every
# shader, and two copies drift quietly: the plugin keeps working, the page keeps
# working, and they stop being the same chain. Character for character --
# reformatting counts. It says nothing about the page's PORT of the control
# laws, the OETF's constants, the matrices or the drift; only a reader checks
# that.
if [ -f demo/tools/check_shaders.py ]; then
	if out=$(python3 demo/tools/check_shaders.py 2>&1); then
		pass "$( printf '%s\n' "$out" | tail -1 )"
	else
		fail "the demo's shaders have drifted from source/Shaders.cpp"
		printf '%s\n' "$out" | tail -12
	fi
else
	printf '   skipped: no demo/\n'
fi

step "offline (no GL)"
for check in laws names; do
	if out=$("$CCTEST" --$check 2>&1); then
		summary=$( printf '%s\n' "$out" | grep -v '^$' | grep -v 'checks,' | grep -v '^   ' | tail -1 )
		pass "cctest --$check: $summary"
	else
		fail "cctest --$check"
		printf '%s\n' "$out" | sed 's/^/      /'
	fi
done

for size in 320x180 1280x720; do
	step "physics at $size"
	for check in identity detail coring knee gamma order skin negative cpu; do
		if out=$("$CCTEST" --$check --size $size 2>&1); then
			pass "cctest --$check: $( printf '%s\n' "$out" | grep -v '^$' | grep -v 'checks,' | grep -v '(note' | tail -1 | sed 's/^ *//' )"
		else
			fail "cctest --$check at $size"
			printf '%s\n' "$out" | sed 's/^/      /'
		fi
	done
done

#---------------------------------------------------------------------------
# --pipe, in the fleet's frame format.
#---------------------------------------------------------------------------
step "pipe"
frame=$(( 64 * 36 * 4 ))
raw=$( mktemp ); many=$( mktemp ); cues=$( mktemp )
head -c $(( frame * 5 / 2 )) /dev/zero > "$raw"
head -c $(( frame * 40 )) /dev/zero > "$many"

got=$( "$CCTEST" --pipe --size 64x36 < "$raw" 2>/dev/null | wc -c | tr -d ' ' )
status=${PIPESTATUS[0]}
if [ "$status" -eq 0 ] && [ "$got" = "$(( frame * 2 ))" ]; then
	pass "2.5 frames in, exactly 2 frames out, clean exit"
else
	fail "2.5 frames in gave $got bytes out (want $(( frame * 2 ))), exit $status"
fi

# Read from a file, not a pipe: a writer killed by SIGPIPE would fail the
# pipeline whatever cctest did, and the refusal would pass for the wrong reason.
printf '0 No Such Control 0.5\n' > "$cues"
"$CCTEST" --pipe --size 64x36 --script "$cues" < "$raw" >/dev/null 2>&1
status=$?
if [ "$status" -eq 2 ]; then
	pass "a cue naming no parameter is refused (exit 2)"
else
	fail "a cue naming no parameter gave exit $status, not 2"
fi

# A failed render stops the stream with exit 1 and nothing after it. The
# failure is injected by the harness (--fail-render-at), because the plugin
# only fails on input no ffmpeg would send.
got=$( "$CCTEST" --pipe --size 64x36 --fail-render-at 1 < "$raw" 2>/dev/null | wc -c | tr -d ' ' )
status=${PIPESTATUS[0]}
if [ "$status" -eq 1 ] && [ "$got" = "$frame" ]; then
	pass "a failed render at frame 1: exit 1, one frame out"
else
	fail "a failed render at frame 1 gave exit $status and $got bytes (want 1 and $frame)"
fi

# A reader that takes one byte and goes away: forty frames is far more than a
# pipe buffer holds, so a write after head leaves must fail. Exit 1, said on
# stderr -- not the 141 of a process SIGPIPE killed before it could say anything.
"$CCTEST" --pipe --size 64x36 < "$many" 2>/dev/null | head -c 1 >/dev/null
status=${PIPESTATUS[0]}
if [ "$status" -eq 1 ]; then
	pass "a closed stdout (| head -c 1): exit 1"
else
	fail "a closed stdout gave exit $status, not 1"
fi

# An option or a boolean STEPS between cues; a slider ramps. Twelve identical
# frames with an edge down the middle and Drift off. Script 1 cues Show
# Detail 0 at frame 0 and 1 at frame 8: frames 0 and 4 must be byte-identical
# (the boolean held at 0) and frame 8 must differ (it has stepped). A ramped
# boolean would read 0.5 at frame 4 and switch the view early. Script 2 cues
# Master Black 0.25 at 0 and 1 at 8: frames 0, 4 and 8 must all differ (the
# slider ramps through 0.625 at frame 4).
edge=$( mktemp ); stepped=$( mktemp ); ramped=$( mktemp )
python3 -c "import sys; row = bytes([40,40,40,255]) * 32 + bytes([200,200,200,255]) * 32; sys.stdout.buffer.write(row * 36 * 12)" > "$edge"
printf '0 Show Detail 0\n8 Show Detail 1\n' > "$cues"
"$CCTEST" --pipe --size 64x36 --set "Drift=0" --script "$cues" < "$edge" > "$stepped" 2>/dev/null
status=$?
f0=$( dd if="$stepped" bs=$frame skip=0 count=1 2>/dev/null | shasum | cut -c1-16 )
f4=$( dd if="$stepped" bs=$frame skip=4 count=1 2>/dev/null | shasum | cut -c1-16 )
f8=$( dd if="$stepped" bs=$frame skip=8 count=1 2>/dev/null | shasum | cut -c1-16 )
if [ "$status" -eq 0 ] && [ "$f0" = "$f4" ] && [ "$f0" != "$f8" ]; then
	pass "a boolean cue steps, not ramps: frame 4 is frame 0, frame 8 is not"
else
	fail "a boolean cue did not step (exit $status; frames 0/4/8: $f0 $f4 $f8)"
fi
printf '0 Master Black 0.25\n8 Master Black 1\n' > "$cues"
"$CCTEST" --pipe --size 64x36 --set "Drift=0" --script "$cues" < "$edge" > "$ramped" 2>/dev/null
status=$?
r0=$( dd if="$ramped" bs=$frame skip=0 count=1 2>/dev/null | shasum | cut -c1-16 )
r4=$( dd if="$ramped" bs=$frame skip=4 count=1 2>/dev/null | shasum | cut -c1-16 )
r8=$( dd if="$ramped" bs=$frame skip=8 count=1 2>/dev/null | shasum | cut -c1-16 )
if [ "$status" -eq 0 ] && [ "$r0" != "$r4" ] && [ "$r4" != "$r8" ] && [ "$r0" != "$r8" ]; then
	pass "a slider cue ramps: frames 0, 4 and 8 all differ"
else
	fail "a slider cue did not ramp (exit $status; frames 0/4/8: $r0 $r4 $r8)"
fi
rm -f "$raw" "$many" "$cues" "$edge" "$stepped" "$ramped"

step "sweep"
if out=$(python3 tools/sweep.py --binary "$CCTEST" 2>/dev/null); then
	pass "$( printf '%s\n' "$out" | tail -1 )"
else
	fail "tools/sweep.py reports a dead control"
	printf '%s\n' "$out" | grep -E '^DEAD|DEAD CONTROLS' | sed 's/^/      /'
fi

step "bench (for the record)"
"$CCTEST" --bench --frames 60 2>&1 | sed -n '3,6p' | sed 's/^/   /'
"$CCTEST" --bench-cpu 2>&1 | grep -E '^(resolution|1920x1080)|fps, an hour' | sed 's/^/   /'

BUNDLE="$BUILD/CCU.bundle"
BIN="$BUNDLE/Contents/MacOS/CCU"

if [ "$(uname)" = "Darwin" ] && [ -d "$BUNDLE" ]; then
	step "registration"
	# `nm ... | grep -q X` FAILS when grep FINDS its match under `set -o pipefail`:
	# grep exits at once, nm takes SIGPIPE, and the pipeline reports failure.
	# Capture and match instead of piping.
	syms=$(nm -gU "$BIN" 2>/dev/null)
	case "$syms" in
		*_plugMain*) pass "exports plugMain" ;;
		*) fail "no plugMain -- the bundle contains no plugin" ;;
	esac

	step "lipo"
	archs=$(lipo -archs "$BIN" 2>/dev/null)
	case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 (got: $archs)" ;; esac
	case "$archs" in *x86_64*) pass "x86_64 present" ;; *) fail "no x86_64 (got: $archs) -- a universal build was asked for" ;; esac

	step "plist"
	exe=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	ident=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	if [ -n "$exe" ] && [ -f "$BUNDLE/Contents/MacOS/$exe" ]; then
		pass "CFBundleExecutable ($exe) is on disk"
	else
		fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign will fail after the tag"
	fi
	if [ "$ident" = "com.stoatworks.ffgl.ccu" ]; then
		pass "CFBundleIdentifier is $ident"
	else
		fail "CFBundleIdentifier is '$ident'"
	fi

	step "codesign"
	tmp=$(mktemp -d)
	cp -R "$BUNDLE" "$tmp/" 2>/dev/null
	if codesign --force --sign - --timestamp=none "$tmp/CCU.bundle" >/dev/null 2>&1; then
		pass "ad-hoc signs (the command the release job runs)"
	else
		fail "ad-hoc signing failed"
	fi
	rm -rf "$tmp"

	step "oxbow"
	OXBOW="${OXBOW:-../oxbow/build/oxbow}"
	[ -x "$OXBOW" ] || OXBOW="$HOME/Projects/resolume/oxbow/build/oxbow"
	if [ -x "$OXBOW" ]; then
		probe=$("$OXBOW" probe "$BUNDLE" 2>&1)
		for want in "name:        SW CCU" "id:          CC01" "type:        effect"; do
			case "$probe" in
				*"$want"*) pass "host sees '$want'" ;;
				*) fail "host does not see '$want' -- see: $OXBOW probe $BUNDLE" ;;
			esac
		done
		self=$("$OXBOW" selftest "$BUNDLE" 2>&1)
		case "$self" in
			*"selftest:    PASS"*) pass "instantiates through plugMain and renders 120 frames" ;;
			*) fail "oxbow selftest did not pass -- see: $OXBOW selftest $BUNDLE" ;;
		esac
	else
		printf '   skipped: oxbow not built at %s\n' "$OXBOW"
	fi
fi

#---------------------------------------------------------------------------
# The OpenFX bundle.
#
# cmake/InfoOFX.plist.in is copied from repo to repo, and the version it was
# usually copied from had the PREVIOUS plugin's name hardcoded into
# CFBundleExecutable. That does not fail the build: the bundle assembles, lipo
# and nm both pass, an OFX host loads it and renders a correct frame. It fails
# at RELEASE time, in codesign, with a message that names a "subcomponent"
# and never mentions the plist. So: the plist against the binary on disk, and
# the exact codesign the release job runs, against a COPY.
#---------------------------------------------------------------------------
OFXB="$BUILD/CCU.ofx.bundle"
if [ "$(uname)" = "Darwin" ] && [ -d "$OFXB" ]; then
	step "openfx"
	named=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$OFXB/Contents/Info.plist" 2>/dev/null)
	ofxid=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$OFXB/Contents/Info.plist" 2>/dev/null)
	if [ -n "$named" ] && [ -f "$OFXB/Contents/MacOS/$named" ]; then
		pass "CFBundleExecutable ($named) is on disk; CFBundleIdentifier is $ofxid"
	else
		fail "CFBundleExecutable is '$named' but no such binary is in Contents/MacOS"
	fi

	ofxbin="$OFXB/Contents/MacOS/CCU.ofx"
	# Captured and matched, not piped into grep -q: see the registration step.
	ofxsyms=$(nm -gU "$ofxbin" 2>/dev/null)
	case "$ofxsyms" in
		*_OfxGetPlugin*) pass "exports OfxGetPlugin" ;;
		*) fail "no OfxGetPlugin -- no host will see a plugin in this bundle" ;;
	esac

	archs=$(lipo -archs "$ofxbin" 2>/dev/null)
	case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 in CCU.ofx (got: $archs)" ;; esac
	case "$archs" in *x86_64*) pass "x86_64 present" ;; *) fail "no x86_64 in CCU.ofx (got: $archs)" ;; esac

	tmp=$(mktemp -d)
	cp -R "$OFXB" "$tmp/"
	if codesign --force --sign - --timestamp=none "$tmp/CCU.ofx.bundle" >/dev/null 2>&1; then
		pass "ad-hoc signs (the command the release job runs)"
	else
		fail "the OpenFX bundle will not codesign"
		codesign --force --sign - --timestamp=none "$tmp/CCU.ofx.bundle" 2>&1 | sed 's/^/      /'
	fi
	rm -rf "$tmp"

	# An OFX host: ofxprobe, from resolume-ofx-bridge. It scans
	# /Library/OFX/Plugins as well as --dir and the FIRST bundle declaring an
	# identifier wins, so an installed copy would be probed instead of this
	# build and every result below would describe it. Say which one it found.
	OFXPROBE="${OFXPROBE:-$BRIDGE/build/ofxprobe}"
	if [ -x "$OFXPROBE" ]; then
		described=$("$OFXPROBE" --dir "$BUILD" 2>&1)
		case "$described" in
			*"bundle     : $BUILD/CCU.ofx.bundle"*) pass "ofxprobe finds com.stoatworks.ccu in THIS build" ;;
			*) fail "ofxprobe resolves com.stoatworks.ccu to another bundle -- something installed shares the identifier"
			   printf '%s\n' "$described" | grep -E 'bundle|com.stoatworks.ccu' | sed 's/^/      /' ;;
		esac
		out=$(mktemp -d)
		rendered=$("$OFXPROBE" --dir "$BUILD" --render com.stoatworks.ccu --size 640x360 --out "$out/ofx.bmp" 2>&1)
		case "$rendered" in
			*" 0 of "*" bytes differ"*)
				fail "the OpenFX bundle renders its input unchanged" ;;
			*"rendered 640x360"*)
				pass "loads and renders at its defaults ($(printf '%s\n' "$rendered" | grep -oE '[0-9]+ of [0-9]+ bytes differ'))" ;;
			*) fail "the OpenFX bundle does not render"
			   printf '%s\n' "$rendered" | sed 's/^/      /' ;;
		esac
		rm -rf "$out"

		if out=$(python3 tools/ofx_agree.py --build "$BUILD" --ofxprobe "$OFXPROBE" 2>&1); then
			pass "the FFGL and OpenFX builds agree byte for byte through their entry points, and the control differs"
			printf '%s\n' "$out" | sed 's/^   /      /'
		else
			fail "the FFGL and OpenFX builds disagree (tools/ofx_agree.py)"
			printf '%s\n' "$out" | sed 's/^/      /'
		fi
	else
		printf '   skipped: ofxprobe not built at %s -- the OpenFX render is unchecked\n' "$OFXPROBE"
	fi

	#-----------------------------------------------------------------------
	# Resolve's Fusion page reports the frame rate on the effect but not on
	# its clips, and the first build's unguarded clip read threw out of
	# render() and failed every frame there. A test host with `--quirks
	# fusion` is stricter than Fusion: it withholds the effect's rate too.
	# Under it the plugin must render, and render exactly what the normal
	# host does at the 24 fps fallback, at a frame where the drift has moved
	# (frame 300, Drift 1). The stock ofxprobe has no quirks mode: point
	# OFXHOST at one that does.
	#-----------------------------------------------------------------------
	step "openfx under a host with no frame rate (--quirks fusion)"
	QHOST="${OFXHOST:-${OFXPROBE:-}}"
	qhelp=""
	[ -n "$QHOST" ] && [ -x "$QHOST" ] && qhelp=$("$QHOST" --help 2>&1)
	case "$qhelp" in
		*"--quirks"*)
			quirked=$("$QHOST" --no-system-dirs --dir "$BUILD" --render com.stoatworks.ccu --size 320x180 \
			          --quirks fusion --set drift=1 --time 300 2>&1)
			normal=$("$QHOST" --no-system-dirs --dir "$BUILD" --render com.stoatworks.ccu --size 320x180 \
			         --frame-rate 24 --set drift=1 --time 300 2>&1)
			other=$("$QHOST" --no-system-dirs --dir "$BUILD" --render com.stoatworks.ccu --size 320x180 \
			        --frame-rate 30 --set drift=1 --time 300 2>&1)
			qhash=$(printf '%s\n' "$quirked" | sed -n 's/.*out hash *fnv1a64 \([0-9a-f]*\).*/\1/p')
			nhash=$(printf '%s\n' "$normal" | sed -n 's/.*out hash *fnv1a64 \([0-9a-f]*\).*/\1/p')
			ohash=$(printf '%s\n' "$other" | sed -n 's/.*out hash *fnv1a64 \([0-9a-f]*\).*/\1/p')
			if [ -z "$qhash" ]; then
				fail "the OpenFX bundle does not render under --quirks fusion"
				printf '%s\n' "$quirked" | grep -iE 'fail|status' | sed 's/^/      /'
			elif [ "$qhash" = "$nhash" ] && [ -n "$ohash" ] && [ "$ohash" != "$nhash" ]; then
				pass "renders with no frame rate, and is the 24 fps render exactly (frame 300, Drift 1: $qhash; 30 fps differs)"
			else
				fail "under --quirks fusion: $qhash, the normal host at 24 fps: $nhash, at 30: $ohash"
			fi ;;
		*)
			printf '   skipped: no test host with --quirks (set OFXHOST to one) -- the no-frame-rate fallback is unchecked\n' ;;
	esac
fi

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit $(( failures > 0 ? 1 : 0 ))
