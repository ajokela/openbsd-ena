#!/bin/sh
# Fetch BSD-licensed reference sources at pinned commits. Linux = read-only.
# Uses sparse checkout to avoid downloading full multi-GB trees.
set -eu
cd "$(dirname "$0")"

# Usage: sparse_clone url dir ref path...
sparse_clone() {
  url="$1"; dir="$2"; ref="$3"; shift 3
  if [ ! -d "$dir/.git" ]; then
    git clone --filter=blob:none --sparse "$url" "$dir"
  fi
  git -C "$dir" sparse-checkout set "$@"
  git -C "$dir" fetch origin "$ref"
  git -C "$dir" checkout "$ref"
}

sparse_clone https://github.com/amzn/amzn-drivers.git \
  amzn-drivers "$AMZN_REF" \
  "kernel/linux/ena" "kernel/linux/common/ena_com"

sparse_clone https://github.com/freebsd/freebsd-src.git \
  freebsd "$FREEBSD_REF" \
  "sys/dev/ena" "sys/contrib/ena-com"

sparse_clone https://github.com/NetBSD/src.git \
  netbsd "$NETBSD_REF" \
  "sys/dev/pci"

sparse_clone https://github.com/torvalds/linux.git \
  linux-readonly "$LINUX_REF" \
  "drivers/net/ethernet/amazon/ena"

printf 'DO NOT COPY FROM THIS TREE (GPL). READ-ONLY REFERENCE.\n' > linux-readonly/DO-NOT-COPY

echo "Fetched at: amzn=$AMZN_REF freebsd=$FREEBSD_REF netbsd=$NETBSD_REF linux=$LINUX_REF"
