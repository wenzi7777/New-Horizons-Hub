# Releases

OTA manifests + firmware binaries for the New Horizons Hub, served straight
from this repo's `main` branch via `raw.githubusercontent.com` — there is no
separate CDN or GitHub Releases step. Pushing a new manifest to `main` is
what makes it live: `newhorizons_hub.ino`'s `kHubUpdateManifestUrl` points
directly at `releases/hub-gcu-v23d-lts-latest.json` on this branch.

- `releases/hub-gcu-v23d-lts-latest.json` and `releases/hub-gcu-v23d-lts-vX.Y.Z.json`
  — manifest for `VD-CTL/R v2.3.D GCU LTS (Hub)`. Generated automatically by
  `VERSION=vX.Y.Z scripts/build_release.sh`, which also produces the
  matching `.bin` under `releases/artifacts/`.
- `model` in the manifest is deliberately `VD-CTL/R v2.3.D GCU LTS (Hub)`,
  not the plain board name the device firmware (`New-Horizons-OS` repo)
  uses for the same physical board — `OtaManager::parseManifest()`'s
  `model_mismatch` check relies on that difference to refuse a manifest URL
  pointed at the wrong repo/product.

## Cutting a release

```bash
VERSION=v0.2.0 scripts/build_release.sh
git add -A
git commit -m "Release v0.2.0: <summary>"
git tag v0.2.0
git push origin main --follow-tags
```
