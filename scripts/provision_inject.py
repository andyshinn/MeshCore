"""
PlatformIO extra_script (pre:) for per-device provisioning.

Reads provision_current.json and injects per-device build flags.
This file is referenced from platformio_override.ini during provisioning.
"""
Import("env")
import json
import os

project_dir = env.subst("$PROJECT_DIR")
config_path = os.path.join(project_dir, "scripts", "provision_current.json")

if os.path.exists(config_path):
    with open(config_path) as f:
        config = json.load(f)

    print(f"[Provision] Injecting config: {config['name']}")

    # Add per-device flags
    flags = [
        f"""-DADVERT_NAME='"{config['name']}"'""",
        f"-DLORA_FREQ={config['lora_freq']}",
        f"-DLORA_BW={config['lora_bw']}",
        f"-DLORA_SF={config['lora_sf']}",
        f"-DLORA_CR={config['lora_cr']}",
    ]
    if "firmware_version" in config:
        flags.append(f"""-DFIRMWARE_VERSION='"{config['firmware_version']}"'""")
    if "firmware_build_date" in config:
        flags.append(f"""-DFIRMWARE_BUILD_DATE='"{config['firmware_build_date']}"'""")
    env.Append(BUILD_FLAGS=flags)
else:
    print("[Provision] No provision_current.json found, using default build flags")
