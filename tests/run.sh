#!/usr/bin/env bash
# Everything that can be tested without Windows or Palworld: the base64 and the
# Socket.IO framing, plus a byte-for-byte interop check against Node's base64.
#
#   bash tests/run.sh
set -euo pipefail
cd "$(dirname "$0")"

g++ -std=c++17 -Wall -Wextra -I../src test_protocol.cpp -o test_protocol
g++ -std=c++17 -Wall -Wextra -I../src test_envelope.cpp -o test_envelope

./test_protocol
node envelope_interop.js emit | ./test_envelope | node envelope_interop.js check

echo "OK"
