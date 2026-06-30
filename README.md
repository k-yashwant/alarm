# ESP32S2 Alarm Clock – Project Log

This document serves as a summary of features and system integrations implemented in this project.

---

##  What has been implemented

### 1. Power Optimization & Sleep States
- **Deep Sleep**: Configured the ESP32S2 to enter deep sleep (conserving battery) for up to 2 hours between scheduled alarms.
- **Light Sleep & Wakeups**: Wakes up early before the scheduled alarm to calibrate clock drift and handle immediate triggers.
- **State Preservation**: Utilized `RTC_DATA_ATTR` variables to preserve alarm states, calibration status, and nearest alarm timestamps across deep sleeps.

### 2. Networking & Time Sync
- **Dual Wi-Fi Options**: Implemented standard home Wi-Fi connection alongside enterprise/campus Wi-Fi credentials for WPA2-Enterprise networks (IIT KGP campus network).
- **Time Synchronization**: Integrated SNTP time synchronization to fetch local Indian Standard Time (IST) after booting.

### 3. Firebase RTDB Integration
- **Alarm Fetching**: Fetches the alarm schedule dynamically from Firebase RTDB in JSON format, parsing the active hours, minutes, and target days.
- **Event Logging**: Implemented real-time event logging to Firebase (`/logs`), uploading state events like `ALARM_TRIGGERED`, `ALARM_SNOOZED`, and `ALARM_DISMISSED` with Unix timestamps.

### 4. Alert & Control Mechanism
- **Synchronized Alerts**: Configured an active buzzer (GPIO 33) and vibration motor (GPIO 38) to pulse in structured alert patterns.
- **Snooze System**: Implemented a physical button snooze function that pauses the alarm for **10 minutes**.
- **Verification Dismissal**: Enabled USB-Serial monitoring so that the alarm can only be fully dismissed by typing a verification message (e.g., *"I am awake!"*). (Smartphone has maximum snooze limits, which were being crossed repeatedly)

### 5. Battery Monitoring & Peripherals
- **OLED Status Display**: Integrated an SSD1306 OLED display showing the clock time, alarm status, and system logs.
- **Battery Measurement**: Configured ADC voltage divider measurement (GPIO 12/11) to display the battery percentage on the top-right corner of the OLED.
- **TinyUSB Console**: Initialized TinyUSB for USB CDC communication, allowing serial interaction over a single USB cable and flushing buffers safely before entering sleep modes.

### 6. Repository & Security Cleanup
- **Credentials Isolation**: Separated Wi-Fi, campus, and Firebase credentials into local, git-ignored header files (`wifi_secrets.h`, `firebase_secrets.h`) and provided template files (`.example`) for the repository structure.
- **Git History Purging**: Rewrote all local branches to completely purge all leaked credentials from the repository history.
