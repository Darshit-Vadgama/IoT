"""
Per-Device QR Code Generator for IoT BLE Provisioning
───────────────────────────────────────────────────────
Each ESP32 gets its own QR code encoding its unique device ID
(derived from the last 3 bytes of the factory MAC address).

USAGE:
  python generate_qr.py <DEVICE_ID>

  Where DEVICE_ID is printed to Serial Monitor at ESP32 boot, e.g.:
    [ID]   Device name : IoT-BE0911
    [QR]   Generate QR for this device:
             python generate_qr.py IoT-BE0911

EXAMPLES:
  python generate_qr.py IoT-BE0911
  python generate_qr.py IoT-A4CF12

BATCH MODE (multiple devices at once):
  python generate_qr.py IoT-BE0911 IoT-A4CF12 IoT-002233

INSTALL DEPENDENCIES:
  pip install qrcode[pil]
"""

import qrcode
from qrcode.constants import ERROR_CORRECT_H
import sys
import os

# ─────────────────────────────────────────────────────────────────
# ✏️  REPLACE THIS with your actual hosted provisioning page URL
PAGE_BASE_URL = "https://darshit-vadgama.github.io/IoT/"
# ─────────────────────────────────────────────────────────────────

def generate_qr(device_id: str):
    url = f"{PAGE_BASE_URL}?device={device_id}"
    output = f"iot_setup_qr_{device_id}.png"

    qr = qrcode.QRCode(
        version=None,
        error_correction=ERROR_CORRECT_H,  # 30% damage recovery — good for stickers
        box_size=12,
        border=4,
    )
    qr.add_data(url)
    qr.make(fit=True)

    img = qr.make_image(fill_color="black", back_color="white")
    img.save(output)

    print(f"  ✅  {device_id}  →  {os.path.abspath(output)}")
    print(f"      URL: {url}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage:  python generate_qr.py <DEVICE_ID> [DEVICE_ID ...]")
        print()
        print("Device ID is printed to Serial Monitor at ESP32 boot:")
        print("  [ID]   Device name : IoT-BE0911")
        print("  [QR]   Generate QR for this device:")
        print("           python generate_qr.py IoT-BE0911")
        sys.exit(1)

    if "YOUR_USERNAME" in PAGE_BASE_URL:
        print("⚠️  Please edit generate_qr.py and set PAGE_BASE_URL first.")
        sys.exit(1)

    device_ids = sys.argv[1:]
    print(f"\nGenerating {len(device_ids)} QR code(s)...\n")
    for did in device_ids:
        generate_qr(did.strip())
    print()
