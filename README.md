# Steelseries Rival 3 Gen 2 macOS Internal Firmware Bug Fix
Fix for SteelSeries Rival 3 Gen 2 mouse freezing/becoming unresponsive after ~15-20 minutes on macOS (Apple Silicon
  MacBooks with USB-C to USB-A adapters). The mouse RGB lights blink on and off but cursor control is completely lost
  until physically unplugging and replugging. Caused by macOS USB power management triggering a firmware hang in the
  mouse. This tool prevents the issue by keeping the USB connection alive and automatically resets the device if it
  still freezes - no more manual replugging.
