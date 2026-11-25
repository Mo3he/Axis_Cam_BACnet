#!/usr/bin/env sh
#
# Build the ACAP package for every architecture into ./releases.
#
#   ./build.sh                 # aarch64 and armv7hf
#   ARCHES=aarch64 ./build.sh  # one architecture
#   RUNTIME=podman ./build.sh  # force a container runtime
#   BACNET_VENDOR_ID=123 BACNET_VENDOR_NAME="Example" ./build.sh

set -eu

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RUNTIME=${RUNTIME:-}
ARCHES=${ARCHES:-"aarch64 armv7hf"}

if [ -z "$RUNTIME" ]; then
	if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
		RUNTIME=docker
	elif command -v podman >/dev/null 2>&1; then
		RUNTIME=podman
	elif command -v docker >/dev/null 2>&1; then
		RUNTIME=docker
	else
		echo "Error: neither Docker nor Podman is available" >&2
		exit 1
	fi
fi

# Remove previous unsigned output only; signed packages must survive a rebuild.
mkdir -p "$REPO_ROOT/releases"
find "$REPO_ROOT/releases" -maxdepth 1 -type f -name '*.eap' ! -name 'signed_*' -delete

build_arch() {
	arch=$1
	tag=$(echo "BACnet_Server-${arch}" | tr '[:upper:]' '[:lower:]')
	output_dir="$REPO_ROOT/.build/${arch}"
	echo "==> Building ${arch} with ${RUNTIME}"
	rm -rf "$output_dir"
	mkdir -p "$output_dir"
	container=''
	trap '[ -z "$container" ] || "$RUNTIME" rm -f "$container" >/dev/null 2>&1 || true' 0
	trap 'exit 1' HUP INT TERM
	set -- --build-arg ARCH="$arch"
	# Optional: BACNET_VENDOR_ID=123 BACNET_VENDOR_NAME="Example" ./build.sh
	[ -z "${BACNET_VENDOR_ID:-}" ] || set -- "$@" --build-arg BACNET_VENDOR_ID="$BACNET_VENDOR_ID"
	[ -z "${BACNET_VENDOR_NAME:-}" ] || set -- "$@" --build-arg BACNET_VENDOR_NAME="$BACNET_VENDOR_NAME"
	"$RUNTIME" build "$@" --target package -t "$tag" "$REPO_ROOT"
	container=$("$RUNTIME" create "$tag")
	"$RUNTIME" cp "$container:/opt/app/." "$output_dir/"
	"$RUNTIME" rm "$container" >/dev/null
	container=''
	package_count=$(find "$output_dir" -type f -name "*_${arch}.eap" | wc -l | tr -d '[:space:]')
	if [ "$package_count" -ne 1 ]; then
		echo "Error: expected one ${arch} package in ${output_dir}, found ${package_count}" >&2
		return 1
	fi
}

arches=''
for arch in $ARCHES; do
	case " $arches " in
	*" $arch "*) continue ;;
	esac
	arches="$arches $arch"
done

pids=''
for arch in $arches; do
	build_arch "$arch" &
	pids="$pids $!"
done

failed=0
for pid in $pids; do
	if ! wait "$pid"; then
		echo "Error: build worker $pid failed" >&2
		failed=1
	fi
done

if [ "$failed" -ne 0 ]; then
	rm -rf "$REPO_ROOT/.build"
	exit 1
fi

for arch in $arches; do
	find "$REPO_ROOT/.build/$arch" -type f -name '*.eap' -exec mv {} "$REPO_ROOT/releases/" \;
done
rm -rf "$REPO_ROOT/.build"

ls -lh "$REPO_ROOT"/releases/*.eap
