#!/usr/bin/env bash
# Launch the emulated/native Spotify Connect receiver: advertise over mDNS and
# run the eSDK-backed ZeroConf server. Ctrl-C stops both.
#
#   ./run.sh [name] [port]
#
# For actual audio out (granted account), pipe PCM to aplay instead:
#   ESDK_PCM_STDOUT=1 ./esdk_server lib/esdk-1.20.0-armhf-v7.so lib/spotify_appkey.key \
#       4 8000 "cspot-esdk-test" 2>run.log | aplay -f cd
set -euo pipefail
cd "$(dirname "$0")"

NAME="${1:-cspot-esdk-test}"
PORT="${2:-8000}"
SO="lib/esdk-1.20.0-armhf-v7.so"
KEY="lib/spotify_appkey.key"

[ -x ./esdk_server ] || { echo "Build first:  make"; exit 1; }
command -v avahi-publish >/dev/null 2>&1 || { echo "Install avahi-utils (provides avahi-publish)"; exit 1; }

avahi-publish -s "$NAME" _spotify-connect._tcp "$PORT" "VERSION=1.0" "CPath=/" &
AV=$!
trap 'kill "$AV" 2>/dev/null || true' EXIT

echo "Advertising '$NAME' on :$PORT via mDNS — pick it in the Spotify app (same LAN)."
exec ./esdk_server "$SO" "$KEY" 4 "$PORT" "$NAME"
