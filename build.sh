#!/bin/sh

set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
GAMEDIR="$ROOT/game/valve"
BUILD_DIR="$ROOT/build"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"

cmake -S "$ROOT" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SERVER=ON \
  -DBUILD_CLIENT=OFF \
  -DGAMEDIR="$GAMEDIR"

cmake --build "$BUILD_DIR" -j "$JOBS"

cmake --install "$BUILD_DIR"

echo "Installed:"
echo "  $GAMEDIR/dlls/zpserver_arm64.dylib"

echo
# Start the game (mirrors build.bat). Override with AUTO_LAUNCH=1 to skip the prompt.
if [ "${AUTO_LAUNCH:-0}" = "1" ]; then
	RUN=1
else
	printf 'Build succeeded. Do you want to start the game (y/n)? '
	read -r REPLY
	case "$REPLY" in
		y|Y|yes|YES) RUN=1 ;;
		*) RUN=0 ;;
	esac
fi

if [ "$RUN" = "1" ]; then
	echo "Starting ZPMod on the valve mod..."
	exec "$ROOT/game/xash3d" -game valve +exec listenserver.cfg +coop 0 +map crossfire +deathmatch 1 -log
else
	echo "Skipping game launch."
fi