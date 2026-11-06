#!/usr/bin/env bash
# Fetches the pinned Skia revision into third_party/skia, and the GN binary
# its build uses. FreeType, HarfBuzz, ICU and the image codecs come from the
# system, so none of Skia's own third-party checkouts are needed.
set -Eeuo pipefail

SKIA_URL=https://skia.googlesource.com/skia
SKIA_BRANCH=chrome/m156
SKIA_REVISION=ea1e651d7a105f5ca327dfd65d28957957279e2e

mkdir -p "$(dirname "$0")/../third_party"
cd "$(dirname "$0")/../third_party"

if [[ ! -d skia/.git ]]; then
  git clone --quiet --filter=blob:none --no-checkout --depth 1 --branch "$SKIA_BRANCH" "$SKIA_URL" skia
fi
cd skia
if [[ "$(git rev-parse --verify --quiet HEAD)" != "$SKIA_REVISION" ]]; then
  git fetch --quiet --filter=blob:none --depth 1 origin "$SKIA_REVISION"
fi
git sparse-checkout set --cone bin build_overrides experimental/ffmpeg gn include modules src \
    third_party
git checkout --quiet --detach "$SKIA_REVISION"

[[ -x bin/gn ]] || python3 bin/fetch-gn
