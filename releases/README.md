# Releases

OTA manifests + firmware binaries for the New Horizons Hub, served straight
from this repo's `main` branch via `raw.githubusercontent.com` — there is no
separate CDN or GitHub Releases step. Pushing a new manifest to `main` is
what makes it live: `newhorizons_hub.ino`'s `kHubUpdateManifestUrl` points
directly at `releases/hub-gcu-v23d-lts-latest.json` on this branch.

- `releases/hub-gcu-v23d-lts-latest.json` and `releases/hub-gcu-v23d-lts-vX.Y.Z.json`
  — manifest for `TIA-CTL v2.3.D GCU LTS (Hub)`. Generated automatically by
  `VERSION=vX.Y.Z scripts/build_release.sh`, which also produces the
  matching `.bin` under `releases/artifacts/`.
- `model` in the manifest is deliberately `VD-CTL/R v2.3.D GCU LTS (Hub)`,
  not the plain board name the device firmware (`New-Horizons-OS` repo)
  uses for the same physical board — `OtaManager::parseManifest()`'s
  `model_mismatch` check relies on that difference to refuse a manifest URL
  pointed at the wrong repo/product. It also keeps the board's pre-rename
  `VD-CTL/R` name even though the Hub now reports `TIA-CTL v2.3.D GCU LTS
  (Hub)`: Hub firmware older than the rename accepts only an exact match,
  while firmware since then accepts either (`kOtaManifestModel`).
- `releases/notes/vX.Y.Z.md` — a changelog note, required before cutting a
  release (`build_release.sh` refuses to run without one, matching
  `New-Horizons-OS`'s and `New-Horizons-Gateway`'s own release process).
  `build_release.sh` embeds its URL into the manifest as `changelog_url` —
  `OtaManager`/`UpdateInfo` on the firmware side already parses and reports
  this field, mechanically copied from the device firmware's own
  `OtaManager.cpp`.

## Update channels (stable / beta)

A Hub follows `stable` (`hub-gcu-v23d-lts-latest.json`) unless switched to
`beta` (`hub-gcu-v23d-lts-beta.json`) from the Desktop's Manage Hub panel
(`set_update_channel`). Beta versions carry a prerelease suffix
(`v0.9.1-beta.1`) and sort below their release; leaving beta never
downgrades. A factory reset puts the Hub back on stable.

```bash
CHANNEL=beta VERSION=v0.9.1-beta.1 scripts/build_release.sh   # only -beta.json
VERSION=v0.9.1 scripts/build_release.sh                      # -latest.json (+ -beta.json)
```

`-latest.json` is all a Hub older than v0.9.0 reads, so it never carries a
prerelease (the generator refuses).

## Cutting a release

```bash
# 1. Write releases/notes/v0.2.0.md first -- build_release.sh will refuse
#    to run without it.
VERSION=v0.2.0 scripts/build_release.sh
git add -A
git commit -m "Release v0.2.0: <summary>"
# -a (annotated) matters here -- `git push --follow-tags` silently skips
# lightweight tags, so a plain `git tag v0.2.0` would never actually reach
# origin even though the push itself reports no error.
git tag -a v0.2.0 -m "Release v0.2.0: <summary>"
git push origin main --follow-tags
```
