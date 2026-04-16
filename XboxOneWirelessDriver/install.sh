#!/usr/bin/env bash
# install.sh — Load / unload the Xbox One Wireless DriverKit extension
#
# Usage:
#   ./install.sh load    — activate the .dext
#   ./install.sh unload  — deactivate the .dext
#   ./install.sh status  — check current status
#
# The .dext must first be copied into /Applications/YourApp.app/
# and the host app must call OSSystemExtensionRequest to activate it.
# This script is for development convenience via systemextensionsctl.

DEXT_ID="com.yourcompany.XboxOneWirelessDriver"
DEXT_PATH="$(dirname "$0")/build/XboxOneWirelessDriver.dext"

case "$1" in
  load)
    echo "Loading $DEXT_ID ..."
    sudo systemextensionsctl developer on 2>/dev/null || true
    # In production: use OSSystemExtensionRequest from a host app.
    # For development with SIP disabled or in a VM:
    pluginkit -a "$DEXT_PATH"
    echo "Done. Check Console.app for kernel_log output."
    ;;

  unload)
    echo "Unloading $DEXT_ID ..."
    systemextensionsctl uninstall - "$DEXT_ID"
    ;;

  status)
    echo "=== System Extensions ==="
    systemextensionsctl list
    echo ""
    echo "=== Matched USB devices ==="
    system_profiler SPUSBDataType | grep -A8 "045e"
    echo ""
    echo "=== IORegistry ==="
    ioreg -r -c XboxOneWirelessDriver 2>/dev/null || echo "(not loaded)"
    ;;

  log)
    echo "Streaming driver log (Ctrl-C to stop)..."
    log stream --predicate 'process == "XboxOneWirelessDriver" OR \
        subsystem == "com.yourcompany.XboxOneWirelessDriver"' \
        --style compact
    ;;

  *)
    echo "Usage: $0 {load|unload|status|log}"
    exit 1
    ;;
esac
