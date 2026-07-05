#!/bin/bash
# Start the MGW in the background and the STP in the foreground.
set -e
osmo-mgw -c /osmo-mgw.cfg &
echo "osmo-mgw started (MGCP 2427)"
exec osmo-stp -c /osmo-stp.cfg
