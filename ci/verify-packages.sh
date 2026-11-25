#!/usr/bin/env bash
#
# Verify that each supported architecture has exactly one valid package.

set -euo pipefail

package_dir=${1:-releases}
packages=("$package_dir"/*.eap)

if [ "${#packages[@]}" -ne 2 ]; then
	echo "Error: expected exactly two .eap packages in ${package_dir}, found ${#packages[@]}" >&2
	exit 1
fi

for arch in aarch64 armv7hf; do
	matches=("$package_dir"/*_"$arch".eap)
	if [ "${#matches[@]}" -ne 1 ] || [ ! -f "${matches[0]}" ]; then
		echo "Error: expected exactly one ${arch} package in ${package_dir}" >&2
		exit 1
	fi

	package=${matches[0]}
	echo "==> Verifying ${package}"
	tar -tzf "$package" >/dev/null
	manifests=()
	while IFS= read -r manifest; do
		manifests+=("$manifest")
	done < <(tar -tzf "$package" | grep -E '(^|/)manifest\.json$' || true)
	if [ "${#manifests[@]}" -ne 1 ]; then
		echo "Error: expected one manifest.json in ${package}" >&2
		exit 1
	fi

	if ! tar -xOzf "$package" "${manifests[0]}" |
		jq -e --arg arch "$arch" '.acapPackageConf.setup.architecture == $arch' >/dev/null; then
		echo "Error: ${package} manifest does not declare architecture ${arch}" >&2
		exit 1
	fi
done
