#!/usr/bin/env bash
# Sync docs/user/**/*.mdx into ../noctalia-docs/src/content/docs/noctalia/.
# The source files remain MDX so the docs site can keep using its components.
# Documentation assets are copied from docs/assets/ into the docs site's
# src/assets/. Existing generated pages are replaced atomically, and stale
# generated pages are removed only after a non-empty source sync succeeds.
# Usage: tools/sync-docs.sh [docs-site-root]
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
site_root="${1:-"$repo_root/../noctalia-docs"}"
source_dir="$repo_root/docs/user"
dest_dir="$site_root/src/content/docs/noctalia"
asset_source="$repo_root/docs/assets"
asset_dest="$site_root/src/assets"
plugin_api_source="$repo_root/docs/plugin-api.json"
plugin_api_dest="$site_root/src/data/plugin-api.json"

[[ -d "$source_dir" ]] || {
    printf 'sync-docs: source directory does not exist: %s\n' "$source_dir" >&2
    exit 1
}
[[ -d "$site_root" ]] || {
    printf 'sync-docs: docs site does not exist: %s\n' "$site_root" >&2
    exit 1
}
[[ -d "$asset_source" ]] || {
    printf 'sync-docs: asset directory does not exist: %s\n' "$asset_source" >&2
    exit 1
}
[[ -f "$plugin_api_source" ]] || {
    printf 'sync-docs: plugin API metadata does not exist: %s\n' "$plugin_api_source" >&2
    exit 1
}

mkdir -p "$dest_dir" "$asset_dest" "$(dirname "$plugin_api_dest")"

declare -A expected_mdx=()
source_count=0

while IFS= read -r -d '' mdx; do
    relative="${mdx#"$source_dir"/}"
    destination="$dest_dir/$relative"
    mkdir -p "$(dirname "$destination")"

    sed \
        -e "s|from '../../assets/|from '../../../../assets/|g" \
        -e "s|](../../assets/|](../../../../assets/|g" \
        -e "s|from '../../../assets/|from '../../../../../assets/|g" \
        -e "s|](../../../assets/|](../../../../../assets/|g" \
        "$mdx" > "$destination.tmp"
    mv "$destination.tmp" "$destination"
    expected_mdx["$destination"]=1
    source_count=$((source_count + 1))
    printf 'synced %s -> %s\n' "$relative" "${destination#"$site_root"/}"
done < <(find "$source_dir" -type f -name '*.mdx' -print0 | sort -z)

if (( source_count == 0 )); then
    printf 'sync-docs: no MDX files found under %s\n' "$source_dir" >&2
    exit 1
fi

cp -a "$asset_source"/. "$asset_dest"/

cp "$plugin_api_source" "$plugin_api_dest.tmp"
mv "$plugin_api_dest.tmp" "$plugin_api_dest"

while IFS= read -r -d '' mdx; do
    if [[ -n "${expected_mdx["$mdx"]+present}" ]]; then
        continue
    fi
    rm "$mdx"
    printf 'removed stale %s\n' "${mdx#"$site_root"/}"
done < <(find "$dest_dir" -type f -name '*.mdx' -print0 | sort -z)
