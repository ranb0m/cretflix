#!/bin/bash
# ==============================================================================
# NVMe Autonomous Power State Transition (APST) Override
# ------------------------------------------------------------------------------
# This script forces the NVMe drive to stay in its full-power operating state
# (PS0) at all times, eliminating the 5-50ms wake-up latency that comes with
# deep sleep states.
#
# Inverted from the prior version: the old script set tolerance to 50000us,
# which PERMITS deep sleep states. For a low-latency streaming workload, that
# is exactly backwards -- the wake stalls it creates are the very thing the
# dispatcher's prefetch logic was previously contorting itself to mitigate.
# Disable the sleep, mitigation isn't needed.
#
# Thermal throttling justification from the prior script doesn't hold for a
# read-heavy server: throttling triggers under sustained writes on consumer
# drives with weak thermal pads, not steady-state reads. Concentrating work
# into bursts also makes thermals slightly worse, not better.
#
# MUST BE RUN AS ROOT.
# ==============================================================================

set -euo pipefail

# In a standard Lenovo single-drive setup, the controller is nvme0.
# The namespace (the partition) is nvme0n1, but power states belong to
# the controller.
CTRL="/dev/nvme0"
NVME_NAME="nvme0"

# Verify root privileges
if [[ $EUID -ne 0 ]]; then
    echo "Error: Modifying NVMe power states requires root privileges."
    exit 1
fi

# Verify the nvme-cli tool exists in the Slackware environment
if [[ ! -x "/usr/sbin/nvme" ]]; then
    echo "Error: /usr/sbin/nvme does not exist."
    exit 1
fi

echo "Interrogating NVMe Controller: $CTRL"
echo "------------------------------------------------------"

# 1. Extract the Power State Descriptors directly from the controller firmware.
#    For each PSn, we get max-power, entry latency, and exit latency. The
#    *exit latency* is what matters: how long it takes to wake from that state
#    when an IO arrives.
/usr/sbin/nvme id-ctrl "$CTRL" | grep -E -i '^ps[[:space:]]+[0-9]' | awk '{
    state=$1$2;
    max_power=$5;
    entry_lat=$8;
    exit_lat=$10;
    print "State: " state " | Max Power: " max_power " | Entry: " entry_lat "us | Exit: " exit_lat "us"
}' || true

echo "------------------------------------------------------"

# 2. Show the kernel-module-level default first. If `nvme_core.default_ps_max_latency_us`
#    was set on the kernel cmdline at boot, APST is already constrained system-wide.
KMOD_DEFAULT="/sys/module/nvme_core/parameters/default_ps_max_latency_us"
if [[ -r "$KMOD_DEFAULT" ]]; then
    BOOT_DEFAULT=$(cat "$KMOD_DEFAULT")
    echo "Boot-time default_ps_max_latency_us : ${BOOT_DEFAULT} us"
    if [[ "$BOOT_DEFAULT" -eq 0 ]]; then
        echo "  -> APST already disabled at module load (PS0 only)."
    else
        echo "  -> APST permits states with up to ${BOOT_DEFAULT}us exit latency."
        echo "  -> Recommended persistent fix: append the following to your"
        echo "     kernel cmdline (LILO/GRUB) and reboot:"
        echo "       nvme_core.default_ps_max_latency_us=0"
    fi
fi

echo "------------------------------------------------------"

# 3. The Goal: keep the drive awake.
#
#    pm_qos_latency_tolerance_us is the *maximum exit latency* the kernel is
#    permitted to choose from the controller's power-state table when picking
#    an idle state. Setting it to 0 means "no idle state is acceptable" --
#    the drive stays in PS0.
#
#    The previous version of this script set this to 50000 (50ms), which
#    permitted PS3/PS4 deep sleep with multi-ms wake costs. Inverted now.
TARGET_LATENCY_US=0

SYSFS_PM_QOS="/sys/class/nvme/${NVME_NAME}/power/pm_qos_latency_tolerance_us"
if [[ -w "$SYSFS_PM_QOS" ]]; then
    CURRENT_LATENCY=$(cat "$SYSFS_PM_QOS")
    echo "Current PM QoS latency tolerance    : ${CURRENT_LATENCY} us"

    echo "Setting tolerance to ${TARGET_LATENCY_US} us (drive will stay in PS0)..."
    echo "$TARGET_LATENCY_US" > "$SYSFS_PM_QOS"

    NEW_LATENCY=$(cat "$SYSFS_PM_QOS")
    if [[ "$NEW_LATENCY" -eq "$TARGET_LATENCY_US" ]]; then
        echo "SUCCESS: PM QoS latency tolerance bound to ${TARGET_LATENCY_US} us."
    else
        echo "FAIL: Kernel rejected the latency parameter."
        echo "  -> If the file rejected the write, the boot-time default may"
        echo "     already be at 0 (no states allowed, including PS0 idle)."
        echo "  -> Or APST may be locked by the BIOS."
        exit 1
    fi
else
    echo "FAIL: $SYSFS_PM_QOS is not writable or does not exist."
    echo "  -> The nvme_core module may have been loaded with APST already disabled."
    echo "     Check $KMOD_DEFAULT -- if it reads 0, APST is off and there's"
    echo "     nothing for this script to do."
    echo "  -> Persistent fix: append to kernel cmdline (LILO/GRUB):"
    echo "       nvme_core.default_ps_max_latency_us=0"
    exit 1
fi

echo "------------------------------------------------------"

# 4. Verification: query the firmware-level APST feature directly. Feature
#    ID 0x0C is the Autonomous Power State Transition feature. Field 0
#    (APSTE) being 0 means APST is disabled at the controller.
echo "Firmware APST feature state (feature 0x0C):"
/usr/sbin/nvme get-feature -f 0x0c "$CTRL" 2>/dev/null || echo "  (could not query; firmware may not expose feature)"

echo "------------------------------------------------------"
echo "Done. Drive is now configured to stay in PS0 under all load conditions."
echo "If you observe wake-stall behavior in benchmarks despite this, the"
echo "drive's firmware is ignoring host PM hints -- some consumer NVMe"
echo "controllers do this. At that point the floor is hardware-imposed."
