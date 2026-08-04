#!/bin/bash
# tests/fixtures/make.sh
# ----------------------------------------------------------------------------
# Build a fixture set in $1 (or /mnt/media_test by default).
#
# These are REAL containers with a media stream, so libavformat's
# probe_bitrate() returns a meaningful number. Critically NOT what the
# old harness did (dd if=/dev/urandom), which produces unprobeable bytes
# and silently disables pacing in the server.
#
# Files produced (in $TARGET):
#   inception.mkv         -- 30s sine, ~1MB        (basic file)
#   movies/dark_knight.mp4 -- 30s sine, ~1MB       (subdir variant)
#   movies/inception.mkv   -- 30s sine, ~1MB       (same basename as top-level
#                                                   to test path-aware hashing)
#   tv/episode_01.mp4     -- 30s sine, ~1MB        (different subdir)
#   subtitles.srt         -- 64 bytes              (filtered by extension)
#
# All audio-only, AAC; tiny files, fast to generate. For pacing tests
# requiring a meaningful bitrate, we generate larger ones on demand
# inside specific tests.
# ----------------------------------------------------------------------------

set -euo pipefail

TARGET="${1:-/mnt/media_test}"
SHORT_DUR=30   # seconds; gives ~1 MB at AAC 256kbps

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "ffmpeg not found. Install: apt-get install ffmpeg" >&2
    exit 1
fi

mkdir -p "$TARGET" "$TARGET/movies" "$TARGET/tv"

# Cleanup any prior contents so we have a known baseline
find "$TARGET" -mindepth 1 -delete

mkdir -p "$TARGET/movies" "$TARGET/tv"

gen() {
    local out="$1"
    local freq="$2"
    local dur="$3"
    ffmpeg -y -hide_banner -loglevel error \
        -f lavfi -i "sine=frequency=${freq}:duration=${dur}" \
        -c:a aac -b:a 256k \
        "$out"
}

echo "Generating fixtures in $TARGET..."
gen "$TARGET/inception.mkv"            440 "$SHORT_DUR"
gen "$TARGET/movies/dark_knight.mp4"   523 "$SHORT_DUR"
gen "$TARGET/movies/inception.mkv"     587 "$SHORT_DUR"   # same basename, different dir
gen "$TARGET/tv/episode_01.mp4"        659 "$SHORT_DUR"

# Subtitle: garbage content, used to verify extension filter rejects it
printf "1\n00:00:00,000 --> 00:00:02,000\nThis subtitle should be filtered.\n" \
    > "$TARGET/subtitles.srt"

echo "Fixture summary:"
find "$TARGET" -type f -printf "  %p  (%s bytes)\n"
