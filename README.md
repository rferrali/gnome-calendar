> [!WARNING]
> **Personal fork. Not maintained for anyone but me.**
> This is my own copy of [GNOME Calendar](https://gitlab.gnome.org/GNOME/gnome-calendar) with a few small, AI-assisted changes for my own setup. Use upstream instead.
> Issues are off and I won't review pull requests. If anything here is useful, especially to upstream, feel free to take it.
> My changes live on the `mine` branch, on top of the upstream stable branch `gnome-51` (not `main`): [see the whole diff](https://github.com/rferrali/gnome-calendar/compare/fork-base...mine).

## About this fork

`mine` is upstream `gnome-51` plus one merge per topic branch. Each branch holds a single change, so it can be read, dropped or sent upstream on its own. `fork-base` is the upstream commit they all sit on, and each link below shows just that branch's diff. The changes are mainly for meetings on Exchange (evolution-ews), and they work together with my [Stamp fork](https://github.com/rferrali/stamp).

| Branch | Change |
|---|---|
| [`feature/meeting-invitations`](https://github.com/rferrali/gnome-calendar/compare/fork-base...feature/meeting-invitations) | Meeting invitations, end to end. **Reply:** Accept / Maybe / Decline in the event popover and the editor, sent as a real reply to the organizer (Exchange AcceptItem etc.), with a choice of occurrence or whole series for recurring events. **Guests:** a guest list showing each person's reply, with Organizer / You / Optional / Room tags. **Invite:** add or remove guests on events you organize, and the backend sends the invitations. **Suggestions:** typing a guest suggests people from every address book (including the Exchange Global Address List) and from Stamp's mail history. **Confirmations:** a prompt before discarding unsaved changes, and before saving a meeting that will email guests, saying who gets what. |
| [`feature/meeting-room-button`](https://github.com/rferrali/gnome-calendar/compare/fork-base...feature/meeting-room-button) | A camera button in an empty Location field that fills in your meeting room link. Set it with `gsettings set org.gnome.calendar meeting-room-url 'https://…'`. It has no UI, and the button stays hidden while the setting is empty. |
| [`local/fork`](https://github.com/rferrali/gnome-calendar/compare/fork-base...local/fork) | Fork-only, never meant for upstream: `local-build.sh`, which builds and installs this checkout as `org.gnome.Calendar` in place of the Flathub build, and lets it read Stamp's mail cache for the guest suggestions; `local/rsvp-test.py`, which tests invitation replies through EDS; this README; and `local-rebuild-mine.sh`. |

### Working on the fork

Each change gets its own branch, cut from `fork-base` and named `feature/…`, `fix/…`, or `local/…` for fork-only material:

```sh
git switch -c fix/something fork-base
# … commit …
./local-rebuild-mine.sh
```

`./local-rebuild-mine.sh` rebuilds `mine` from scratch, which also syncs it with upstream:

1. Fetches `upstream` and `origin`, and moves `fork-base` to the latest commit of the base branch. The base comes from `git config fork.base`, which is local, untracked configuration. Here it is `upstream/gnome-51`; when a new stable branch comes out, run `git config fork.base upstream/gnome-52` before the script. Without the setting the base is `upstream/main`.
2. Rebases every `feature/*`, `fix/*` and `local/*` branch onto `fork-base`. Commits upstream has already taken drop out. A branch left with no commits is skipped, and the script prints how to delete it.
3. Tags the old `mine` as `mine-backup-<date-time>`, resets `mine` to `fork-base`, and merges each remaining branch with `--no-ff`, `local/*` last. `git log --first-parent fork-base..mine` then shows one line per change.
4. Pushes nothing. At the end it prints the `git push --force-with-lease` command for `fork-base`, `mine` and the topic branches.

If a rebase conflicts, resolve it, run `git rebase --continue`, and run the script again. Don't commit directly on `mine`: the next rebuild throws those commits away. Rebuild and reinstall with `./local-build.sh` afterwards.

<div align="center">
  <img src="data/icons/hicolor/scalable/apps/org.gnome.Calendar.svg" width="128" height="128">

  # GNOME Calendar

  Manage your schedule

  <img width="922" src="https://static.gnome.org/appdata/gnome-50/org.gnome.Calendar/month-view.png">
</div>

## Installation

[![Flatpak](https://flathub.org/api/badge?svg&locale=en)](https://flathub.org/apps/details/org.gnome.Calendar)

## Links

- Homepage: [apps.gnome.org/Calendar]
- Report an issue: [gitlab.gnome.org/GNOME/gnome-calendar/issues]
- Donate: [donate.gnome.org]
- Translate: [l10n.gnome.org/module/gnome-calendar]
- Contact: [matrix.to/#/#gnome-calendar:gnome.org]

## Contributing

For information on how to contribute, please check [CONTRIBUTING.md](CONTRIBUTING.md) and [HACKING.md](HACKING.md).


[apps.gnome.org/Calendar]: https://apps.gnome.org/Calendar
[gitlab.gnome.org/GNOME/gnome-calendar/issues]: https://gitlab.gnome.org/GNOME/gnome-calendar/issues/
[donate.gnome.org]: https://donate.gnome.org/
[l10n.gnome.org/module/gnome-calendar]: https://l10n.gnome.org/module/gnome-calendar/
[matrix.to/#/#gnome-calendar:gnome.org]: https://matrix.to/#/#gnome-calendar:gnome.org/
