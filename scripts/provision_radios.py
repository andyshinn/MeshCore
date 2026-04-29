#!/usr/bin/env python3
"""
Radio provisioning script for MeshCore firmware.

Builds firmware once, then flashes the same binary to each radio.
Tracks per-device status in a CSV.

Usage:
    python3 scripts/provision_radios.py flash
    python3 scripts/provision_radios.py cards
"""
import argparse
import csv
import glob
import os
import serial
import shutil
import subprocess
import sys
import time
from pathlib import Path

# Allow importing generate_card from the same directory
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from generate_card import generate_card

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(SCRIPT_DIR)
CSV_FILE = os.path.join(SCRIPT_DIR, "provision_radios.csv")
CARDS_DIR = Path(SCRIPT_DIR) / "cards"

PIO_ENV = "austec"
UPLOAD_PORT = "/dev/cu.usbmodem4101"
UF2_ERASE_FILE = os.path.expanduser("~/Downloads/FLASH_ERASE_nrf52_softdevice_v6.uf2")
UF2_VOLUME_PATTERNS = ["/Volumes/HT-n5262"]

CSV_FIELDS = ["index", "name", "status"]


# ═════════════════════════════════════════════════════════════════
#  Shared helpers
# ═════════════════════════════════════════════════════════════════

def generate_csv(prefix, count):
    """Generate a CSV file with all device configurations."""
    devices = []
    for i in range(1, count + 1):
        devices.append({
            "index": i,
            "name": f"{prefix}-{i:03d}",
            "status": "pending",
        })

    with open(CSV_FILE, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerows(devices)

    print(f"Generated {CSV_FILE} with {count} devices.")
    return devices


def load_csv():
    """Load existing CSV file."""
    devices = []
    with open(CSV_FILE, "r", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            row["index"] = int(row["index"])
            devices.append(row)
    return devices


def save_csv(devices):
    """Save devices back to CSV."""
    with open(CSV_FILE, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerows(devices)



def generate_info_card():
    """Generate the static info card PDF. Returns the output path."""
    CARDS_DIR.mkdir(parents=True, exist_ok=True)
    card_path = CARDS_DIR / "radio_card.pdf"
    generate_card(card_path)
    return card_path


def print_status(devices):
    """Print current provisioning status."""
    flashed = [d for d in devices if d["status"] == "flashed"]
    print(f"\n{'='*60}")
    print(f"Provisioning Status: {len(flashed)}/{len(devices)} complete")
    print(f"{'='*60}")
    for d in devices:
        marker = "[x]" if d["status"] == "flashed" else "[ ]"
        print(f"  {marker} {d['name']}")
    print()


# ═════════════════════════════════════════════════════════════════
#  Flash helpers
# ═════════════════════════════════════════════════════════════════

def enter_bootloader():
    """Send 1200bps touch to reset device into UF2 bootloader mode."""
    print(f"  Sending 1200bps touch to {UPLOAD_PORT}...")
    try:
        s = serial.Serial(UPLOAD_PORT, 1200)
        s.dtr = True
        time.sleep(0.1)
        s.dtr = False
        time.sleep(0.1)
        s.close()
    except serial.SerialException as e:
        print(f"  ERROR: Could not open {UPLOAD_PORT}: {e}")
        return False
    # Give the device time to reset and mount as UF2
    print("  Waiting for device to enter bootloader...")
    time.sleep(3)
    return True


def find_uf2_volume():
    """Find the mounted UF2 bootloader volume."""
    for pattern in UF2_VOLUME_PATTERNS:
        matches = glob.glob(pattern)
        for m in matches:
            # Check it looks like a UF2 bootloader (has INFO_UF2.TXT or CURRENT.UF2)
            if os.path.isdir(m) and (
                os.path.exists(os.path.join(m, "INFO_UF2.TXT")) or
                os.path.exists(os.path.join(m, "CURRENT.UF2"))
            ):
                return m
    return None


def flash_uf2_erase():
    """Flash the UF2 erase file to wipe the device. Returns True on success."""
    if not os.path.exists(UF2_ERASE_FILE):
        print(f"  ERROR: UF2 erase file not found at {UF2_ERASE_FILE}")
        return False

    volume = find_uf2_volume()
    if not volume:
        # Try entering bootloader via 1200bps touch
        if not enter_bootloader():
            return False
        volume = find_uf2_volume()
        if not volume:
            print("  ERROR: UF2 volume did not appear after 1200bps touch.")
            print("  Try double-tap reset manually, then press Enter.")
            input()
            volume = find_uf2_volume()
            if not volume:
                print("  ERROR: Still no UF2 volume found. Skipping erase.")
                return False

    print(f"  Found UF2 volume: {volume}")
    print("  Copying erase firmware...")
    try:
        shutil.copy2(UF2_ERASE_FILE, os.path.join(volume, "flash_erase.uf2"))
    except OSError as e:
        # Device disconnects mid-copy as it begins processing the UF2 file.
        # This is expected behavior — the copy got far enough to trigger the erase.
        if e.errno in (5, 6):  # EIO, ENXIO (Input/output error, Device not configured)
            print(f"  Device disconnected during copy (expected).")
        else:
            raise
    print("  Erase firmware sent. Device will erase and reboot...")

    # Wait for the volume to disappear then reappear.
    # The device may disconnect so fast we never see it gone,
    # so we track whether it left and accept a new volume either way.
    saw_disconnect = False
    print("  Waiting for device to reboot into bootloader...")
    for _ in range(40):
        time.sleep(0.2)
        if not saw_disconnect:
            if not os.path.exists(volume):
                saw_disconnect = True
        else:
            new_volume = find_uf2_volume()
            if new_volume:
                print(f"  Device ready: {new_volume}")
                return True

    if not saw_disconnect:
        # Volume never disappeared — it may have remounted too fast to notice.
        # Check if it's actually a fresh bootloader volume.
        new_volume = find_uf2_volume()
        if new_volume:
            print(f"  Device ready: {new_volume}")
            return True

    print("  WARNING: UF2 volume did not reappear. Device may need manual reset.")
    return False


def build_firmware():
    """Build the firmware once. Returns True on success."""
    cmd = ["pio", "run", "-e", PIO_ENV]
    print(f"Building firmware: {' '.join(cmd)}")
    print("-" * 60)
    result = subprocess.run(cmd, cwd=PROJECT_DIR)
    return result.returncode == 0


def upload_firmware():
    """Upload the already-built firmware to the connected device."""
    cmd = ["pio", "run", "-e", PIO_ENV, "-t", "upload", "--upload-port", UPLOAD_PORT]
    print(f"Uploading: {' '.join(cmd)}")
    print("-" * 60)
    result = subprocess.run(cmd, cwd=PROJECT_DIR)
    return result.returncode == 0



# ═════════════════════════════════════════════════════════════════
#  Subcommands
# ═════════════════════════════════════════════════════════════════

def cmd_flash(args):
    """Flash subcommand — build once, then flash each radio."""
    print("=" * 60)
    print("  MeshCore Radio Provisioning Tool")
    print("=" * 60)

    # Generate or load CSV
    if not os.path.exists(CSV_FILE):
        print("\nNo provisioning CSV found. Let's create one.\n")
        prefix = input("Device name prefix (e.g. 'ATXMC'): ").strip()
        if not prefix:
            print("Prefix cannot be empty.")
            sys.exit(1)
        count = int(input("Number of devices to provision: ").strip())
        if count < 1:
            print("Count must be at least 1.")
            sys.exit(1)
        devices = generate_csv(prefix, count)
    else:
        devices = load_csv()
        print(f"\nLoaded existing CSV with {len(devices)} devices.")

    print_status(devices)

    # Find next pending device
    pending = [d for d in devices if d["status"] == "pending"]
    if not pending:
        print("All devices have been flashed!")
        return

    # Build firmware once
    print("\nBuilding firmware...")
    if not build_firmware():
        print("ERROR: Firmware build failed.")
        sys.exit(1)
    print("\nFirmware built successfully. Ready to flash devices.\n")

    # Ask about erase mode
    do_erase = False
    if os.path.exists(UF2_ERASE_FILE):
        erase_choice = input("Erase devices before flashing via UF2? (Y/n): ").strip().lower()
        do_erase = erase_choice != "n"
    else:
        print(f"Note: UF2 erase file not found at {UF2_ERASE_FILE}")
        print("Skipping erase step. Devices will be flashed without erasing first.")

    # Flash each device with the same firmware
    for device in pending:
        print(f"\n{'='*60}")
        print(f"  Next: {device['name']} (#{device['index']})")
        print(f"{'='*60}")

        response = input("\nConnect the radio and press Enter to flash (or 'q' to quit): ").strip().lower()
        if response == "q":
            print("\nQuitting. Progress saved. Run again to continue.")
            break

        # Erase via UF2 if enabled
        if do_erase:
            print("\n  Step 1/2: Erasing device...")
            if not flash_uf2_erase():
                retry = input("  Erase failed. Press Enter to skip this device, or 'q' to quit: ").strip().lower()
                if retry == "q":
                    break
                continue
            print("\n  Step 2/2: Uploading firmware...")
        else:
            print("\n  Uploading firmware...")

        # Upload pre-built firmware
        if upload_firmware():
            device["status"] = "flashed"
            save_csv(devices)
            print(f"\n  Successfully flashed {device['name']}!")
        else:
            print(f"\n  FAILED to flash {device['name']}. Status not updated.")
            retry = input("  Press Enter to continue to next device, or 'q' to quit: ").strip().lower()
            if retry == "q":
                break

    print_status(devices)

    remaining = [d for d in devices if d["status"] == "pending"]
    if not remaining:
        print("All devices provisioned!")


def cmd_cards(args):
    """Cards subcommand — generate the static PDF info card."""
    card_path = generate_info_card()
    print(f"Card → {card_path}")


# ═════════════════════════════════════════════════════════════════
#  CLI
# ═════════════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(description="MeshCore Radio Provisioning Tool")
    subparsers = parser.add_subparsers(dest="command", required=True)

    # flash subcommand
    flash_parser = subparsers.add_parser("flash", help="build and flash radios")
    flash_parser.set_defaults(func=cmd_flash)

    # cards subcommand
    cards_parser = subparsers.add_parser("cards", help="generate the static PDF info card")
    cards_parser.set_defaults(func=cmd_cards)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
