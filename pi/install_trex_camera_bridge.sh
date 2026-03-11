#!/usr/bin/env bash
set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if ! command -v rpicam-hello >/dev/null 2>&1; then
  echo "rpicam-hello is not installed or not in PATH."
  echo "Install Raspberry Pi camera apps first, then re-run this installer."
  exit 1
fi

if ! python3 - <<'PY' >/dev/null 2>&1
import gpiozero
PY
then
  echo "gpiozero not found; installing python3-gpiozero..."
  sudo apt update
  sudo apt install -y python3-gpiozero
fi

sudo mkdir -p /opt/trex-camera
sudo install -m 755 "$SRC_DIR/trex_camera_gpio_bridge.py" /opt/trex-camera/trex_camera_gpio_bridge.py
sudo install -m 644 "$SRC_DIR/motion_detect.json" /opt/trex-camera/motion_detect.json
sudo install -m 644 "$SRC_DIR/trex-camera-bridge.service" /etc/systemd/system/trex-camera-bridge.service

sudo systemctl daemon-reload
sudo systemctl enable trex-camera-bridge.service
sudo systemctl restart trex-camera-bridge.service

echo
echo "Service status:"
sudo systemctl --no-pager --full status trex-camera-bridge.service || true
