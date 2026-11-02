#!/usr/bin/env bash
# Sync docs/user/**/*.mdx into ../noctalia-docs/src/content/docs/noctalia/.
# The source files remain MDX so the docs site can keep using its components.
# Documentation assets are copied from docs/assets/ into the docs site's
# src/assets/. Existing generated pages are replaced atomically, and stale
# generated pages are removed only after a non-empty source sync succeeds.
# Each vX.Y.Z release tag is archived once, from the tag itself, into
# noctalia/<tag>/ with its assets in src/assets/<tag>/ and its plugin API
# metadata in src/data/plugin-api/<tag>.json; src/data/versions.json lists
# them. FORCE_SYNC=1 re-archives every release.
# Usage: [FORCE_SYNC=1] tools/sync-docs.sh [docs-site-root]
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
site_root="${1:-"$repo_root/../noctalia-docs"}"
source_dir="$repo_root/docs/user"
dest_dir="$site_root/src/content/docs/noctalia"
asset_source="$repo_root/docs/assets"
asset_dest="$site_root/src/assets"
plugin_api_source="$repo_root/docs/plugin-api.json"
plugin_api_dest="$site_root/src/data/plugin-api.json"
versions_dest="$site_root/src/data/versions.json"

active_extract_dir=""
active_staging_dir=""
active_asset_staging_dir=""
active_versions_tmp=""
cleanup_temp_dirs() {
    [[ -n "$active_extract_dir" ]] && rm -rf "$active_extract_dir"
    [[ -n "$active_staging_dir" ]] && rm -rf "$active_staging_dir"
    [[ -n "$active_asset_staging_dir" ]] && rm -rf "$active_asset_staging_dir"
    [[ -n "$active_versions_tmp" ]] && rm -f "$active_versions_tmp"
    return 0
}
trap cleanup_temp_dirs EXIT

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
command -v jq > /dev/null 2>&1 || {
    printf 'sync-docs: jq is required but not installed\n' >&2
    exit 1
}
git -C "$repo_root" rev-parse --is-inside-work-tree > /dev/null 2>&1 || {
    printf 'sync-docs: %s is not a git work tree\n' "$repo_root" >&2
    exit 1
}

generate_versions_manifest() {
    local manifest_entries=()
    manifest_entries+=( "$(jq -n '{version: "main", tag: "main", label: "main (rolling)", path: "/noctalia/", status: "rolling", isDefault: true, isLatest: false}')" )

    local first_release=1
    while IFS= read -r release_tag; do
        [[ "$release_tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || continue
        if [[ -z "$(git -C "$repo_root" ls-tree "$release_tag" docs/user 2>/dev/null)" ]]; then
            continue
        fi
        read -r commit_date commit_hash < <(git -C "$repo_root" log -1 --format='%cs %h' "$release_tag" 2>/dev/null)
        local is_latest=false
        local release_status="archived"
        local release_label="$release_tag"
        if (( first_release )); then
            is_latest=true
            release_status="latest"
            release_label="$release_tag (Latest)"
            first_release=0
        fi
        manifest_entries+=( "$(jq -n \
            --arg version "${release_tag#v}" \
            --arg tag "$release_tag" \
            --arg label "$release_label" \
            --arg path "/noctalia/$release_tag/" \
            --arg releaseDate "$commit_date" \
            --arg commit "$commit_hash" \
            --arg status "$release_status" \
            --argjson isLatest "$is_latest" \
            '{version: $version, tag: $tag, label: $label, path: $path, releaseDate: $releaseDate, commit: $commit, status: $status, isLatest: $isLatest}')" )
    done < <(git -C "$repo_root" tag -l 'v*' --sort=-v:refname 2>/dev/null)

    if (( first_release )); then
        printf 'sync-docs: no vX.Y.Z release tags with docs/user found in %s\n' "$repo_root" >&2
        exit 1
    fi

    active_versions_tmp="$versions_dest.tmp"
    printf '%s\n' "${manifest_entries[@]}" | jq -s '.' > "$active_versions_tmp"
}

mkdir -p "$dest_dir" "$asset_dest" "$(dirname "$plugin_api_dest")" "$(dirname "$versions_dest")"

generate_versions_manifest

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
done < <(find "$dest_dir" -path "$dest_dir/v[0-9]*" -prune -o -type f -name '*.mdx' -print0 | sort -z)

# Archives one release from its extracted tree. Assets go to src/assets/<tag>/ and plugin API
# metadata to src/data/plugin-api/<tag>.json so archived pages never depend on the rolling copies.
# Pages are moved into place last: their directory existing marks the release as fully archived.
sync_version_docs() {
    local version_source_root="$1"
    local version_tag="$2"
    local version_source_dir="$version_source_root/docs/user"
    local version_dest_dir="$dest_dir/$version_tag"
    local version_asset_dest="$asset_dest/$version_tag"
    local version_plugin_api_dest="$site_root/src/data/plugin-api/$version_tag.json"
    local version_source_count=0

    rm -rf "$version_asset_dest.tmp"
    active_asset_staging_dir="$version_asset_dest.tmp"
    cp -a "$version_source_root/docs/assets" "$active_asset_staging_dir"
    rm -rf "$version_asset_dest"
    mv "$active_asset_staging_dir" "$version_asset_dest"
    active_asset_staging_dir=""

    mkdir -p "$(dirname "$version_plugin_api_dest")"
    cp "$version_source_root/docs/plugin-api.json" "$version_plugin_api_dest.tmp"
    mv "$version_plugin_api_dest.tmp" "$version_plugin_api_dest"

    rm -rf "$dest_dir/.$version_tag.tmp"
    active_staging_dir="$dest_dir/.$version_tag.tmp"
    mkdir "$active_staging_dir"

    while IFS= read -r -d '' mdx_file; do
        local relative_path="${mdx_file#"$version_source_dir"/}"
        local destination_file="$active_staging_dir/$relative_path"
        mkdir -p "$(dirname "$destination_file")"

        local relative_slug="${relative_path%.mdx}"
        relative_slug="${relative_slug%/index}"
        local page_slug="noctalia/${version_tag}/${relative_slug}"
        if [[ "$relative_slug" == "index" ]]; then
            page_slug="noctalia/${version_tag}"
        fi

        # Source pages reach docs/assets/ through one ../ per directory level plus one; the archived
        # copy sits three levels deeper (src/content/docs/noctalia/<tag>/) and uses its tag's assets.
        local parent_dirs="${relative_path//[^\/]/}"
        local source_asset_prefix="" archived_asset_prefix="../../../"
        local level
        for (( level = 0; level <= ${#parent_dirs}; level++ )); do
            source_asset_prefix+='\.\./'
            archived_asset_prefix+='../'
        done
        archived_asset_prefix+="assets/${version_tag}/"

        sed \
            -e "1s|^---$|---\nslug: ${page_slug}|" \
            -e "s#\(['\"(]\)${source_asset_prefix}assets/#\1${archived_asset_prefix}#g" \
            -e "s|\.\./components/|../../components/|g" \
            -e "s|](/noctalia/|](/noctalia/${version_tag}/|g" \
            -e "s|href=\"/noctalia/|href=\"/noctalia/${version_tag}/|g" \
            "$mdx_file" > "$destination_file"
        version_source_count=$((version_source_count + 1))
    done < <(find "$version_source_dir" -type f -name '*.mdx' -print0 | sort -z)

    if (( version_source_count == 0 )); then
        printf 'sync-docs: no MDX files found for release %s\n' "$version_tag" >&2
        exit 1
    fi

    rm -rf "$version_dest_dir"
    mv "$active_staging_dir" "$version_dest_dir"
    active_staging_dir=""
}

mapfile -t release_tags < <(jq -r '.[] | select(.tag != "main") | .tag' "$active_versions_tmp")
for release_tag in "${release_tags[@]}"; do
    version_destination="$dest_dir/$release_tag"
    if [[ -d "$version_destination" && "${FORCE_SYNC:-0}" != "1" ]]; then
        continue
    fi

    printf 'syncing release %s -> %s\n' "$release_tag" "${version_destination#"$site_root"/}"
    active_extract_dir="$(mktemp -d)"
    if ! git -C "$repo_root" archive "$release_tag" docs/user/ docs/assets/ docs/plugin-api.json \
        | tar -x -C "$active_extract_dir"; then
        printf 'sync-docs: failed to archive git tag %s\n' "$release_tag" >&2
        exit 1
    fi
    sync_version_docs "$active_extract_dir" "$release_tag"
    rm -rf "$active_extract_dir"
    active_extract_dir=""
done

mv "$active_versions_tmp" "$versions_dest"
active_versions_tmp=""
