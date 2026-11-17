#!/bin/bash
set -euo pipefail
echo "Legacy container wrapper: prefer build.py or Actions -> Build packages."
source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
engine=${CONTAINER_ENGINE:-}
if [ -z "$engine" ]; then
    if command -v docker >/dev/null; then engine=docker
    elif command -v podman >/dev/null; then engine=podman
    else echo "Install Docker or Podman, or use build.py directly." >&2; exit 1
    fi
fi
"$engine" run --rm \
    -v "${source_dir}:/accelerator" -w /accelerator \
    ghcr.io/alliedmodders/build-containers/debian11-clang22:latest \
    bash dockerbuild/_accelerator_docker_build_internal.sh "$@"
