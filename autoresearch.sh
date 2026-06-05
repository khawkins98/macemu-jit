#!/bin/bash
set -euo pipefail

# Autoresearch runner compatibility wrapper.
# Canonical harness lives at BasiliskII/jit-test/run.sh.
exec "$(cd "$(dirname "$0")" && pwd)/BasiliskII/jit-test/run.sh"
