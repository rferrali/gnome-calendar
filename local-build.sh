#!/usr/bin/env bash
# Personal helper (not for upstream): build the current checkout and install it
# as org.gnome.Calendar (user installation) — Romain's main calendar, replacing
# the Flathub build. Flathub never updates it: rebase the fork on new releases
# and rebuild.
#
#   ./local-build.sh          build + install
#   ./local-build.sh run      build + install + run
#
# Generates a copy of the upstream manifest that:
#  - uses the app ID org.gnome.Calendar and the default (non-development)
#    profile, without tracing;
#  - builds on the stable GNOME runtime matching the release the fork is based on;
#  - pins evolution-data-server to the host's release (instead of git main);
#  - disables tests;
#  - grants read access to Stamp's mail cache (guest suggestions).
#
# After rebuilding, `flatpak kill org.gnome.Calendar` so the running instance
# (it stays in the background after its window closes) picks up the new build.
set -euo pipefail
cd "$(dirname "$0")"

out=_flatpak
runtime=51
eds_tag=3.62.0
mkdir -p "$out"
jq --arg eds "$eds_tag" --arg runtime "$runtime" '
    ."app-id" = "org.gnome.Calendar"
  | ."runtime-version" = $runtime
  | del(.tags)
  | (.modules[] | select(.name == "evolution-data-server") | .sources[0]) += {"tag": $eds}
  | (.modules[] | select(.name == "gnome-calendar")) |= (
        del(."run-tests")
      | ."config-opts" = ["-Dprofile=default", "-Dtracing=false"])
  | ."finish-args" += ["--filesystem=~/.var/app/org.tabos.stamp/cache/stamp:ro"]' \
  build-aux/org.gnome.Calendar.json > "$out/org.gnome.Calendar.local.json"

flatpak-builder --user --install --force-clean --ccache \
  --state-dir="$out/state" \
  "$out/build" "$out/org.gnome.Calendar.local.json"

if [[ "${1:-}" == "run" ]]; then
  flatpak run org.gnome.Calendar
fi
