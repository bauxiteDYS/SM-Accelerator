#!/bin/bash
# For the project's root-run Debian 11 / clang-22 container only.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
dpkg --add-architecture i386
apt-get update
apt-get install -y --no-install-recommends --allow-downgrades \
    git python3-venv python3-pip g++-multilib libc6-dev-i386 \
    linux-libc-dev/bullseye linux-libc-dev:i386/bullseye
