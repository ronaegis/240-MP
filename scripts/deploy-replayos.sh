#!/usr/bin/env bash
# Install the bundle from scripts/build-replayos.sh on a ReplayOS box over SSH,
# using the same installer as releases (scripts/install-replayos.sh --local).
#
#   scripts/deploy-replayos.sh [user@host]      (default root@replay)
set -euo pipefail
HOST="${1:-root@replay}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

scp -q "$ROOT/build-replayos/240mp-replayos.tar.gz" "$ROOT/scripts/install-replayos.sh" "$HOST:/tmp/"
ssh "$HOST" 'bash /tmp/install-replayos.sh --local /tmp/240mp-replayos.tar.gz; status=$?
             rm -f /tmp/240mp-replayos.tar.gz /tmp/install-replayos.sh; exit $status'
