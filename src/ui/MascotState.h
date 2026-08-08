


#pragma once

enum MascotState {
    MASCOT_STANDBY = 0,
    MASCOT_BOOT_ATTENTION,
    MASCOT_LORA_RECON,
    MASCOT_WIFI_RECON,
    MASCOT_SCANNING = MASCOT_WIFI_RECON,
    MASCOT_PWNY,
    MASCOT_PWNAGOTCHI = MASCOT_PWNY,   // v2 alias
    MASCOT_BAD_USB,
    MASCOT_HOMELAB_SYNC,
    MASCOT_LOW_BATTERY,
    MASCOT_ALERT,
    MASCOT_TRANSMIT,
    MASCOT_RECON_WALK,
    MASCOT_PREFLIGHT,
    MASCOT_ERROR,
    // Dedicated states so these screens stop borrowing STANDBY / HOMELAB_SYNC:
    // the mesh screen and the uplink mission each get their own identity.
    MASCOT_MESHTASTIC,
    MASCOT_UPLINK,
    MASCOT_COUNT
};




