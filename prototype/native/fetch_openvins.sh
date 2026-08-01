#!/bin/sh -e
# Fetch OpenVINS into third_party/openvins (shallow clone)
# Usage: ./fetch_openvins.sh

set -eux
BASE_DIR=$(cd $(dirname "$0") && pwd)
THIRD_DIR="$BASE_DIR/third_party"
OPENVINS_DIR="$THIRD_DIR/openvins"

if [ -d "$OPENVINS_DIR" ]; then
  echo "OpenVINS already exists at $OPENVINS_DIR"
  exit 0
fi

mkdir -p "$THIRD_DIR"
cd "$THIRD_DIR"
# Using shallow clone to reduce download size
git clone --depth 1 https://github.com/rpng/open_vins.git openvins || {
  echo "Shallow clone failed, trying full clone"
  rm -rf openvins || true
  git clone https://github.com/rpng/open_vins.git openvins
}

echo "OpenVINS fetched to $OPENVINS_DIR"
