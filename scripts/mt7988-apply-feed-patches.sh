#!/usr/bin/env bash
set -euo pipefail

source_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
patch_dir="$source_root/package/mtk/patches/mt7988-luci"
targets=(
    feeds/luci/modules/luci-base/root/usr/share/rpcd/ucode/luci
    feeds/luci/modules/luci-mod-status/htdocs/luci-static/resources/view/status/include/60_wifi.js
    feeds/luci/modules/luci-mod-status/root/usr/share/rpcd/acl.d/luci-mod-status-index.json
)
patches=(
    1000-mtwifi-6.6-luci-status-overview-wifi7-mlo.patch
    1002-mtwifi-6.6-luci-status-overview-rate-mhz-hi.patch
)

cd "$source_root"
for target in "${targets[@]}"; do
    if [[ ! -f "$target" ]]; then
        printf 'MT7988 LuCI patch target is missing: %s\n' "$target" >&2
        exit 1
    fi
done

for patch_name in "${patches[@]}"; do
    patch_file="$patch_dir/$patch_name"
    if [[ ! -s "$patch_file" ]]; then
        printf 'MT7988 source patch is missing: %s\n' "$patch_file" >&2
        exit 1
    fi
done

# Check the whole ordered series on copies of its three feed targets. Reverse
# order is required because 1002 edits context introduced by 1000. A partial
# series never writes back to the real feed tree.
scratch="$(mktemp -d "${TMPDIR:-/tmp}/mt7988-feed-patches.XXXXXXXX")"
trap 'rm -rf -- "$scratch"' EXIT
copy_targets() {
    for target in "${targets[@]}"; do
        mkdir -p -- "$scratch/$(dirname -- "$target")"
        cp -p -- "$source_root/$target" "$scratch/$target"
    done
}
copy_targets

already_applied=true
for ((index=${#patches[@]}-1; index >= 0; index--)); do
    if ! git -C "$scratch" apply --reverse --whitespace=nowarn \
        "$patch_dir/${patches[index]}" >/dev/null 2>&1; then
        already_applied=false
        break
    fi
done
if "$already_applied"; then
    printf 'MT7988 LuCI patch series is already applied\n'
    exit 0
fi

# Discard any partial reverse attempt, then validate and apply the complete
# series on scratch copies. Only after both patches succeed are targets copied
# back into the feed tree.
copy_targets
for patch_name in "${patches[@]}"; do
    if ! git -C "$scratch" apply --whitespace=nowarn "$patch_dir/$patch_name"; then
        printf 'MT7988 LuCI patch series does not match this feed (failed at %s); feed was left unchanged\n' \
            "$patch_name" >&2
        exit 1
    fi
done
for target in "${targets[@]}"; do
    cp -p -- "$scratch/$target" "$source_root/$target"
done
printf 'Applied source-owned MT7988 LuCI patch series\n'
