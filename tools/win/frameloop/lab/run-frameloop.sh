#!/usr/bin/env bash
# One bounded set of frameloop runs on unit A. The work is in run-frameloop.py; this exists so that the call
# is the same one line as the other lab runners in this workspace.
#
#   bash tools/win/frameloop/lab/run-frameloop.sh quick|mt|gap [--trial NAME] [--dry-run]
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec python "$here/run-frameloop.py" "$@"
