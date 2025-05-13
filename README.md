# Updated to support current libaries

- Added support for a new button for wifi config mode
- Added support for potentiometer for volume control (still a bit noisy, keeps updating volume, ideally would use a better one or make it less sensitive.)
- Wifi config mode allows the user to configure and add new radio stations
- Changed audio package to use default "Audio" package for esp32 to fix broken play function

## Setup

**3D Files:** [Onshape project link](https://cad.onshape.com/documents/a49b0da92358af82e52b2462/w/a694426a2e309911855f3e7b/e/cfa827aa1b3785606bd24166)

### Install packages

This program is meant to be used with the TTGO-T-Display V1.1

**ArduinoJson by Benoit**: v7.4.1 or latest

**Button2 by Lennart**: v2.3.4 or latest

**ESP32-audiol2S-master by schreibfaul1**: v2.0.0 (V3 WILL NOT WORK without refactor)

**[TFT_eSPI by Bodmer](https://github.com/Xinyuan-LilyGO/TTGO-T-Display/tree/master/TFT_eSPI):** V2.2.20 (download from the TTGO T-Display git repo by zipping the tft_eSPI folder and loading it as a zip library)

**[BluetoothA2DPSink](https://github.com/pschatzmann/ESP32-A2DP):** Have to download this from github only it looks like. Load as a zip library.

## Usage

---

## 1. TTGO Bottom Button (Backlight / Mode)

- **Physical Location:** Bottom button on the TTGO board (connected to GPIO 0).
- **Tap Action (Short Press):**
  - Cycles through predefined backlight brightness levels for the TFT screen.
- **Long Press Action (Hold for approx. 1 second):**
  - Toggles the audio mode between **WiFi Radio** and **Bluetooth Sink**.
    - The screen will indicate the mode change.

---

## 2. TTGO Top Button (Visualizer / Config)

- **Physical Location:** Top button on the TTGO board (connected to GPIO 35).
- **Tap Action (Short Press):**
  - Cycles through the available audio visualizer animations (e.g., Bars, Circles, Particles).
    - If a visualizer is active, a tap will turn it off and return to the main information screen.
    - If the visualizer is off, a tap will turn on the first visualizer.
- **Long Press Action (Hold for approx. 3 seconds):**
  - Puts the device into **Configuration Portal Mode** on the next reboot.
    - This allows you to connect to the device's WiFi Access Point (AP) to configure WiFi credentials and radio stations via a web browser.
    - The device will display "Rebooting to Config Mode..." and then restart.

---

## 3. External Button C (Play / Stop - WiFi Radio Only)

- **Physical Location:** External button connected to GPIO 12.

- **Tap Action (Short Press):**
  - **If in WiFi Radio Mode:**
        - If a station is selected and not playing: Starts playback of the current WiFi radio station.
        - If a station is playing: Stops playback of the current WiFi radio station.
    - **If in Bluetooth Mode:** This button has no function.

---

## 4. External Button D (Next Station - WiFi Radio Only)

- **Physical Location:** External button connected to GPIO 17.
- **Tap Action (Short Press):**
  - **If in WiFi Radio Mode AND playback is currently STOPPED:**
        - Selects the next radio station in the stored list. The station name will update on the display.
        - This does *not* automatically start playback; use the "Play/Stop" button to start the newly selected station.
    - **If in WiFi Radio Mode AND playback is active:** This button has no function (you must stop playback first to change stations this way).
    - **If in Bluetooth Mode:** This button has no function.

---

**Note on Volume Control:**
Volume is controlled by the potentiometer (connected to `POT_PIN` / GPIO 32). Turning the potentiometer will adjust the volume for both WiFi Radio and Bluetooth modes. The display shows the volume as a percentage.
