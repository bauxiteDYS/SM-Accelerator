#!/bin/bash
set -euo pipefail

# Legacy container entry point. Delegate to the maintained build helper rather
# than force-updating submodules, deleting builds or resetting dependency sources.
cd /accelerator
bash dockerbuild/install-linux-deps.sh

# Trust only the mounted project, not every Git directory in the container.
git config --global --add safe.directory /accelerator
CC=clang-22 CXX=clang++-22 python3 build.py "$@"
