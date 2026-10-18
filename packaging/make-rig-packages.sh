#!/usr/bin/env bash
# Builds the HiveOS and mmpOS packages of a hashwarp executable:
#   packaging/make-rig-packages.sh <hashwarp executable> <version> <output directory>
# HiveOS: <output>/hashwarp-<version>.tar.gz   (name and top directory are required by HiveOS)
# mmpOS:  <output>/hashwarp-<version>-mmpos.tgz
set -euo pipefail

exe=$1
version=$2
out=$3
here=$(cd "$(dirname "$0")" && pwd)

# HiveOS splits the archive name on its last '-'
[[ $version == *-* ]] && { echo "version must not contain '-': $version" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

mkdir -p "$work/hiveos/hashwarp" "$work/mmpos/hashwarp" "$out"

cp "$exe" "$work/hiveos/hashwarp/hashwarp"
cp "$here"/hiveos/h-*.sh "$work/hiveos/hashwarp/"
sed "s/@VERSION@/$version/" "$here/hiveos/h-manifest.conf" > "$work/hiveos/hashwarp/h-manifest.conf"
chmod +x "$work"/hiveos/hashwarp/*.sh "$work/hiveos/hashwarp/hashwarp"
tar -C "$work/hiveos" -czf "$out/hashwarp-$version.tar.gz" hashwarp

cp "$exe" "$work/mmpos/hashwarp/hashwarp"
cp "$here"/mmpos/mmp-*.sh "$work/mmpos/hashwarp/"
sed "s/@VERSION@/$version/" "$here/mmpos/mmp-external.conf" > "$work/mmpos/hashwarp/mmp-external.conf"
chmod +x "$work"/mmpos/hashwarp/*.sh "$work/mmpos/hashwarp/hashwarp"
tar -C "$work/mmpos" -czf "$out/hashwarp-$version-mmpos.tgz" hashwarp

echo "$out/hashwarp-$version.tar.gz"
echo "$out/hashwarp-$version-mmpos.tgz"
