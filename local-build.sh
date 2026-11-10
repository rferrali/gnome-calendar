#!/usr/bin/env bash
# Personal helper (not for upstream): build the current checkout as the
# org.gnome.Calendar.Devel Flatpak, next to the stable org.gnome.Calendar.
#
#   ./local-build.sh          build + install
#   ./local-build.sh run      build + install + run
#
# Generates a copy of the upstream manifest with evolution-data-server pinned to
# the host's release (instead of git main), tests disabled, and read access to
# Stamp's mail cache (guest suggestions from the mail history).
set -euo pipefail
cd "$(dirname "$0")"

out=_flatpak
eds_tag=3.62.0
mkdir -p "$out"
jq --arg eds "$eds_tag" '
    (.modules[] | select(.name == "evolution-data-server") | .sources[0]) += {"tag": $eds}
  | (.modules[] | select(.name == "gnome-calendar")) |= (del(."run-tests"))
  | ."finish-args" += ["--filesystem=~/.var/app/org.tabos.stamp/cache/stamp:ro"]' \
  build-aux/org.gnome.Calendar.json > "$out/org.gnome.Calendar.local.json"

flatpak-builder --user --install --force-clean --ccache \
  --state-dir="$out/state" \
  "$out/build" "$out/org.gnome.Calendar.local.json"

if [[ "${1:-}" == "run" ]]; then
  flatpak run org.gnome.Calendar.Devel
fi
