"""
QR Code Generator for IoT BLE Provisioning
───────────────────────────────────────────
This script generates a QR code that points to your hosted
wifi_provisioning.html page. When a user scans it with their
phone camera, the browser opens the page and guides them through
BLE provisioning.

USAGE:
  1. Host wifi_provisioning.html at a public URL
     (free options: GitHub Pages, Netlify, Vercel)
  2. Set YOUR_URL below to that URL
  3. Run:  python generate_qr.py

INSTALL DEPENDENCIES:
  pip install qrcode[pil]
"""

import qrcode
from qrcode.constants import ERROR_CORRECT_H
import sys
import os

# ─────────────────────────────────────────────────────────────────
# ✏️  REPLACE THIS with your actual hosted URL
# e.g. "https://your-username.github.io/iot-setup/"
YOUR_URL = "https://YOUR_USERNAME.github.io/iot-setup/"
# ─────────────────────────────────────────────────────────────────

OUTPUT_FILE = "iot_setup_qr.png"

def generate_qr(url: str, output: str):
    if "YOUR_USERNAME" in url:
        print("⚠️  Please edit generate_qr.py and set YOUR_URL to your actual hosted page URL.")
        sys.exit(1)

    qr = qrcode.QRCode(
        version=None,                    # auto-size
        error_correction=ERROR_CORRECT_H,  # 30% damage recovery (good for stickers)
        box_size=12,
        border=4,
    )
    qr.add_data(url)
    qr.make(fit=True)

    img = qr.make_image(fill_color="black", back_color="white")
    img.save(output)

    print(f"✅ QR code saved to: {os.path.abspath(output)}")
    print(f"   URL encoded: {url}")
    print(f"   Image size:  {img.pixel_size}×{img.pixel_size} px")
    print()
    print("Next steps:")
    print("  • Print or display iot_setup_qr.png on/near your IoT device")
    print("  • Android: scan with default Camera app → opens Chrome → done")
    print("  • iPhone:  scan with Camera app → opens Safari  ← NOT supported")
    print("             Use the free 'Bluefy' app on iPhone instead")

if __name__ == "__main__":
    generate_qr(YOUR_URL, OUTPUT_FILE)
