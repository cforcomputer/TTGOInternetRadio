// =========================================================================
// Includes
// =========================================================================
#include <WiFi.h>
#include "Audio.h"        // Use compatible version (e.g., 2.0.3 tag for Core 2.0.14)
#include <TFT_eSPI.h>     // Graphics and font library (Requires configuration)
#include <SPI.h>          // Required by TFT_eSPI
#include <WebServer.h>    // Standard ESP32 Web Server (for Config Portal)
#include <FS.h>           // Filesystem base
#include <SPIFFS.h>       // Filesystem implementation
#include <Preferences.h>  // For NVS storage (WiFi credentials & Config Flag)
#include <ArduinoJson.h>  // For handling station list saving/loading (Install via Library Manager)
#include <Button2.h>      // For button handling (Tap, Long Press) - Install via Library Manager
#include <vector>         // For dynamic station lists

// =========================================================================
// Definitions and Constants
// =========================================================================

// --- TFT Objects and Settings ---
TFT_eSPI tft = TFT_eSPI();  // Invoke library, pins defined in User_Setup.h
#define TFT_GREY 0x5AEB     // Custom grey color (Can still be used for text/lines)
#define TFT_ORANGE 0xFDA0

// --- Screen Dimensions (Assuming TTGO T-Display 240x135 in landscape) ---
#define SCREEN_WIDTH  240
#define SCREEN_HEIGHT 135

// --- PWM Settings (for TFT Backlight) ---
const int pwmFreq = 5000;
const int pwmResolution = 8;
const int pwmLedChannelTFT = 0;

// --- Pin Definitions ---
// I2S Pins (Connect to external DAC like UDA1334A)
#define I2S_DOUT      25  // Data Out (DIN) -> Connect to UDA1334A DIN
#define I2S_BCLK      27  // Bit Clock (BCLK) -> Connect to UDA1334A BCLK
#define I2S_LRC       26  // Left/Right Clock (LRC / WS) -> Connect to UDA1334A WSEL

// Potentiometer Pin (Volume Control)
#define POT_PIN       32  // ADC Pin for potentiometer wiper (GPIO 32 on TTGO T-Display V1.1)

// --- Button Pins (Functions SWAPPED as per request) ---
const int LED = 13;     // Status LED Pin (If available/desired on your board)
const int BTN_BACKLIGHT = 0;  // GPIO Backlight Adjust (TTGO Bottom Button - Was Play/Stop)
const int BTN_INVERT = 35; // GPIO Invert Display (TTGO Top Button - Was Next) - Input Only Pin
const int BTN_PLAY_STOP_CONFIG = 12; // GPIO Play/Pause / Config Trigger (Long Press) (External Button - Was Backlight)
const int BTN_NEXT = 17; // GPIO Next Station (External Button - Was Invert)

// --- Web Configuration Portal ---
const char* AP_SSID = "RadioConfigPortal"; // Name of the WiFi network created by the ESP32
const char* AP_PASSWORD = "password123";   // Password for the configuration network (CHANGE THIS!)
const char* STATIONS_PATH = "/stations.json"; // Filename for station list on SPIFFS

// --- Button Timing ---
#define LONG_PRESS_MS 3000 // 3 seconds for long press trigger

// =========================================================================
// Global Objects
// =========================================================================
Audio audio;                // Instance of the ESP32-audioI2S library
WebServer server(80);       // Web server for configuration
Preferences preferences;    // Object for accessing Non-Volatile Storage
Button2 buttonBacklight;    // Button object for BTN_BACKLIGHT (GPIO 0)
Button2 buttonInvert;       // Button object for BTN_INVERT (GPIO 35)
Button2 buttonPlayStopCfg;  // Button object for BTN_PLAY_STOP_CONFIG (GPIO 12) - External Button
Button2 buttonNext;         // Button object for BTN_NEXT (GPIO 17) - External Button

// =========================================================================
// Global Variables
// =========================================================================

// --- Dynamic Station Data ---
std::vector<String> arrayURL;     // Vector to hold station URLs
std::vector<String> arrayStation; // Vector to hold station names
String currentStationName = "No Stations"; // Holds the name of the currently selected station
String currentStreamTitle = ""; // Holds the current stream title
String currentStatus = "Booting"; // Holds the current status text
String currentBitrate = "";    // Holds the current bitrate text
int sflag = 0;                // Current station index in the vectors

// --- Playback & Control State ---
volatile bool playflag = false;    // Playback state flag (true if audio.isRunning())
int currentVolume = 10;            // Current volume (0-21), potentially set by pot at startup
unsigned long lastPotRead = 0;     // Timer for reading potentiometer
const int potReadInterval = 100;   // Read pot every 100ms
bool ledState = false;             // For blinking status LED

// --- Display & UI State ---
int backlightLevels[5] = {10, 30, 60, 120, 220}; // PWM duty cycles for backlight
byte currentBacklightIndex = 2; // Index for current backlight level (0-4)
bool invertDisplayState = false;// Invert display state
uint16_t statusColor = TFT_YELLOW; // Color for the status message
uint32_t lastBlinkTime = 0;       // Timer for blinking indicator

// =========================================================================
// Function Prototypes
// =========================================================================

// --- Mode Setup ---
void setupRadioMode();
void drawScreenLayout(); // New function to draw static text elements

// --- Configuration Portal ---
void startConfigurationPortal();
void handleRoot();
void handleSaveWifi();
void handleSaveStations();
void handleReboot();
void handleNotFound();

// --- Configuration Storage ---
bool loadConfiguration();
bool saveStationsToSPIFFS();
void loadDefaultStations(); // Helper to load default stations
void connectToWiFi(); // Modified to use stored credentials AND trigger config on fail

// --- Radio Helpers ---
String html_escape(String str); // Function to escape HTML special chars
void updateDisplay(); // New central function to redraw dynamic parts
void clearTextArea(int x, int y, int w, int h); // Helper to clear text areas
void handlePotentiometer();

// --- Button Handlers (for Button2 library) ---
void handlePlayStopTap(Button2& btn);
void enterConfigModeLongClick(Button2& btn);
void handleNextStationTap(Button2& btn);
void handleBacklightTap(Button2& btn);
void handleInvertTap(Button2& btn);

// --- Audio Library Callbacks ---
void audio_info(const char *info);
void audio_id3data(const char *info);
void audio_eof_mp3(const char *info);
void audio_showstation(const char *info);
void audio_showstreamtitle(const char *info);
void audio_bitrate(const char *info);
void audio_commercial(const char *info);
void audio_icyurl(const char *info);
void audio_lasthost(const char *info);
void audio_eof_stream(const char *info);

// =========================================================================
// SETUP FUNCTION - Runs once on boot/reset
// =========================================================================
void setup() {
    Serial.begin(115200);
    delay(200); // Short delay

    Serial.println("\n\n--- Booting ESP32 Text Radio ---");

    // --- Check for Configuration Mode Trigger (NVS Flag) ---
    preferences.begin("config", false); // Open NVS read-write
    bool forceConfig = preferences.getBool("enterConfig", false); // Check if flag was set
    if (forceConfig) {
        preferences.putBool("enterConfig", false); // Clear the flag immediately!
        Serial.println(">>> Configuration Flag Found - Entering Config Mode <<<");
    }
    preferences.end(); // Close NVS

    bool configMode = forceConfig; // Enter config mode if flag was set

    if (configMode) {
        // --- Configuration Portal Mode ---
        Serial.println(">>> Starting Configuration Portal <<<");
        // Initialize TFT just for config message (using text)
        tft.init();
        tft.setRotation(1); // Set horizontal orientation (240x135)
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.setTextSize(2); // Larger font for main message
        tft.setCursor(5, 5); tft.println("Config Mode"); // Use smaller margins
        tft.setTextSize(1); // Smaller font for details

        int yPos = 30; // Starting Y position for details
        tft.setCursor(5, yPos); tft.print("Connect WiFi to:"); yPos += 12;
        tft.setCursor(15, yPos); tft.print(AP_SSID); yPos += 12;
        tft.setCursor(5, yPos); tft.print("Password:"); yPos += 12;
        tft.setCursor(15, yPos); tft.print(AP_PASSWORD); yPos += 12;
        tft.setCursor(5, yPos); tft.print("Browser to:"); yPos += 12;
        tft.setCursor(15, yPos); tft.print("192.168.4.1"); yPos += 18; // Add some space

        // --- Add the Warning Message Here ---
        tft.setTextColor(TFT_ORANGE, TFT_BLACK);
        tft.setCursor(5, yPos); tft.print("NOTE: Page load includes"); yPos += 12;
        tft.setCursor(15, yPos); tft.print("WiFi scan, please wait...");

        // Initialize Filesystem (needed to load stations for display on web page)
        if (!SPIFFS.begin(true)) { // true = format if mount failed
            Serial.println("CRITICAL: SPIFFS Mount Failed in Config Mode!");
            tft.setTextColor(TFT_RED, TFT_BLACK);
            // Position error at bottom right if possible
            tft.drawString("FS Error!", SCREEN_WIDTH - 60, SCREEN_HEIGHT - 15, 2);
            while (1) delay(1000); // Halt on critical error
        }
        // Load current stations to display them on the web page
        loadConfiguration(); // Load station data needed for handleRoot later

        // Now start the portal - the server loop begins AFTER this setup phase
        startConfigurationPortal(); // Enters configuration loop, does not return here

    } else {
        // --- Normal Radio Mode ---
        Serial.println(">>> Starting Radio Mode <<<");
        if (!SPIFFS.begin(true)) {
            Serial.println("CRITICAL: SPIFFS Mount Failed!");
            tft.init(); tft.fillScreen(TFT_BLACK); tft.setTextColor(TFT_RED);
            tft.setRotation(1); // Horizontal
            tft.drawString("FS Error!", 10, 10, 4); while(1) delay(1000);
        }
        loadConfiguration();
        setupRadioMode();
        connectToWiFi();
        Serial.println("--- Setup Complete (Radio Mode) ---");
    }
}

// =========================================================================
// SETUP FUNCTION FOR NORMAL RADIO MODE
// =========================================================================
void setupRadioMode() {
    // Initialize TFT Display
    tft.init();
    tft.setRotation(1); // Set HORIZONTAL orientation
    tft.fillScreen(TFT_BLACK);
    // tft.setSwapBytes(true); // Usually not needed for text

    // Initialize TFT Backlight PWM
    #ifdef TFT_BL // TFT_BL is usually defined in TFT_eSPI User_Setup.h
        Serial.printf("setupRadioMode: TFT_BL defined as GPIO %d\n", TFT_BL);
        ledcSetup(pwmLedChannelTFT, pwmFreq, pwmResolution);
        ledcAttachPin(TFT_BL, pwmLedChannelTFT);
        ledcWrite(pwmLedChannelTFT, backlightLevels[currentBacklightIndex]); // Set initial brightness
        Serial.println("setupRadioMode: TFT backlight PWM configured.");
    #else
        Serial.println("setupRadioMode: WARNING - TFT_BL pin not defined. Backlight control disabled.");
    #endif

    // Draw Initial Static Screen Elements
    drawScreenLayout();

    // Initialize GPIOs (Input only needed if Button2 doesn't set it)
    pinMode(LED, OUTPUT); digitalWrite(LED, HIGH); // LED Off initially
    // Button2 library handles pin mode internally (INPUT_PULLUP usually)

    // Initialize Button2 objects and handlers --- BUTTON SWAP IMPLEMENTED HERE ---
    Serial.println("setupRadioMode: Initializing buttons (Functions Swapped)...");

    // TTGO Bottom Button (GPIO 0) -> Backlight Control
    buttonBacklight.begin(BTN_BACKLIGHT, INPUT_PULLUP); // GPIO 0 - Specify PULLUP
    buttonBacklight.setTapHandler(handleBacklightTap);
    // No long click handler assigned to backlight button currently
    Serial.println(" - Backlight Button (TTGO Btn A / GPIO 0) Initialized");

    // TTGO Top Button (GPIO 35) -> Invert Display
    buttonInvert.begin(BTN_INVERT, INPUT); // GPIO 35 - Input only, no internal pullup/down
    buttonInvert.setTapHandler(handleInvertTap);
    Serial.println(" - Invert Button (TTGO Btn B / GPIO 35) Initialized");

    // External Button C (GPIO 12) -> Play/Stop/Config
    // Assumes external buttons are wired between the GPIO pin and GND
    buttonPlayStopCfg.begin(BTN_PLAY_STOP_CONFIG, INPUT_PULLUP); // GPIO 12 - Enable internal pullup
    buttonPlayStopCfg.setTapHandler(handlePlayStopTap);
    buttonPlayStopCfg.setLongClickHandler(enterConfigModeLongClick);
    buttonPlayStopCfg.setLongClickTime(LONG_PRESS_MS); // Set long press duration
    Serial.println(" - Play/Stop/Config Button (External / GPIO 12) Initialized");

    // External Button D (GPIO 17) -> Next Station
    buttonNext.begin(BTN_NEXT, INPUT_PULLUP); // GPIO 17 - Enable internal pullup
    buttonNext.setTapHandler(handleNextStationTap);
    Serial.println(" - Next Station Button (External / GPIO 17) Initialized");

    Serial.println("setupRadioMode: Buttons initialized.");

    // Initial screen state based on loaded config
    handlePotentiometer(); // Read initial pot position & update volume
    currentStatus = "Ready";
    statusColor = TFT_YELLOW;
    updateDisplay(); // Draw initial dynamic info

    // IP Address will be drawn after WiFi connection attempt in setup()

    // Initialize Audio System
    Serial.println("setupRadioMode: Initializing Audio Output..."); Serial.flush();
    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT); // Set I2S pins
    audio.setVolume(currentVolume); // Set initial volume based on pot reading
    Serial.printf("setupRadioMode: Initial Volume: %d\n", currentVolume);
}

// =========================================================================
// DRAW INITIAL SCREEN LAYOUT (Static Text)
// =========================================================================
void drawScreenLayout() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);

    // Line 1: Status Label + Volume Label
    tft.setCursor(5, 5); tft.print("Status:");
    tft.setCursor(150, 5); tft.print("Vol:"); // Position volume label towards the right

    // Line 2: Station Label
    tft.setCursor(5, 25); tft.print("Station:");

    // Line 3: Title Label
    tft.setCursor(5, 45); tft.print("Title:");

    // Line 4: Bitrate Label (Optional, maybe combined with status)
    tft.setCursor(5, SCREEN_HEIGHT - 25); tft.print("Bitrate:");

    // Bottom Line: IP Address Label
    tft.setCursor(5, SCREEN_HEIGHT - 12); tft.print("IP:");
}

// =========================================================================
// CLEAR TEXT AREA HELPER
// =========================================================================
void clearTextArea(int x, int y, int w, int h) {
    tft.fillRect(x, y, w, h, TFT_BLACK);
}

// =========================================================================
// UPDATE DISPLAY (Draws Dynamic Information)
// =========================================================================
void updateDisplay() {
    // --- Status ---
    int statusX = 5 + 50; // Position after "Status:" label
    int statusY = 5;
    clearTextArea(statusX, statusY, 90, 10); // Clear previous status text area
    tft.setTextColor(statusColor, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(statusX, statusY);
    // Add blinking indicator if playing
    if (playflag && (millis() - lastBlinkTime > 500)) {
        tft.print( (millis()/500)%2 == 0 ? "*" : " " ); // Blinking asterisk
        lastBlinkTime = millis();
    } else if (playflag) {
         tft.print( (millis()/500)%2 == 0 ? "*" : " " ); // Keep drawing the current state
    }
    tft.print(currentStatus.substring(0, 12)); // Print status, truncated if needed

    // --- Volume ---
    int volX = 150 + 30; // Position after "Vol:" label
    int volY = 5;
    clearTextArea(volX, volY, 30, 10); // Clear previous volume area
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(volX, volY);
    tft.print(currentVolume);

    // --- Station Name ---
    int stationX = 5 + 55; // Position after "Station:" label
    int stationY = 25;
    clearTextArea(stationX, stationY, SCREEN_WIDTH - stationX - 5, 10); // Clear rest of line
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(stationX, stationY);
    tft.print(currentStationName.substring(0, 30)); // Truncate if very long

    // --- Stream Title ---
    int titleX = 5; // Start near left edge
    int titleY = 60; // Below "Title:" label
    int titleWidth = SCREEN_WIDTH - 10;
    int titleHeight = 35; // Allow roughly 2-3 lines
    int charWidth = 6; // Approx width of character in size 1 font
    int charsPerLine = titleWidth / charWidth;

    clearTextArea(titleX, titleY, titleWidth, titleHeight); // Clear title area
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(titleX, titleY);

    String titleToDisplay = currentStreamTitle;
    titleToDisplay.trim();

    if (titleToDisplay.length() > 0) {
        int currentLine = 0;
        int maxLines = 2; // Limit to 2 lines for now
        while (titleToDisplay.length() > 0 && currentLine < maxLines) {
            int breakPoint = -1;
            if (titleToDisplay.length() <= charsPerLine) {
                tft.print(titleToDisplay);
                titleToDisplay = ""; // End loop
            } else {
                // Find last space within the limit
                for (int i = charsPerLine -1; i >=0; i--) {
                    if (titleToDisplay.charAt(i) == ' ') {
                        breakPoint = i;
                        break;
                    }
                }
                if (breakPoint != -1) {
                     tft.print(titleToDisplay.substring(0, breakPoint));
                     titleToDisplay = titleToDisplay.substring(breakPoint + 1);
                } else { // No space found, hard break
                    tft.print(titleToDisplay.substring(0, charsPerLine));
                    titleToDisplay = titleToDisplay.substring(charsPerLine);
                }
            }
            currentLine++;
            if (titleToDisplay.length() > 0) { // Move to next line if needed
                 tft.setCursor(titleX, titleY + (currentLine * 12)); // Adjust Y offset per line
            }
        }
    }

    // --- Bitrate ---
    int bitrateX = 5 + 55; // Position after "Bitrate:"
    int bitrateY = SCREEN_HEIGHT - 25;
    clearTextArea(bitrateX, bitrateY, 100, 10); // Clear bitrate area
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(bitrateX, bitrateY);
    tft.print(currentBitrate);

    // --- IP Address (Only draw if connected) ---
    int ipX = 5 + 25; // Position after "IP:"
    int ipY = SCREEN_HEIGHT - 12;
    clearTextArea(ipX, ipY, 150, 10); // Clear IP area
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(ipX, ipY);
    if (WiFi.status() == WL_CONNECTED) {
        tft.print(WiFi.localIP().toString());
    } else {
        tft.print("---.---.---.---");
    }
}


// =========================================================================
// NORMAL RADIO LOOP - Runs continuously in Radio Mode
// =========================================================================
void loop() {
    uint32_t currentTime = millis(); // Get current time once per loop

    // --- Handle Button Events (Swapped Buttons) ---
    buttonBacklight.loop();
    buttonInvert.loop();
    buttonPlayStopCfg.loop();
    buttonNext.loop();

    // --- Handle WiFi Connection Status ---
    if (WiFi.status() != WL_CONNECTED) {
        if (playflag) { // Only stop if it was playing
             audio.stopSong();
             playflag = false; // Manually update flag
             currentStatus = "WiFi Lost"; statusColor = TFT_RED;
             currentStreamTitle = ""; // Clear title
             currentBitrate = ""; // Clear bitrate
             updateDisplay(); // Update screen
             Serial.println("WiFi connection lost. Playback stopped.");
        }
        // No audio processing if not connected
    } else {
        // --- Handle Audio Streaming (Only if WiFi is connected) ---
        audio.loop(); // Process audio buffers, callbacks, etc.
    }

    // Update playflag based on library state AFTER calling audio.loop()
    // Check if state actually changed to minimize display updates
    bool currentPlayState = audio.isRunning();
    if (currentPlayState != playflag) {
        playflag = currentPlayState;
        if (!playflag) { // If just stopped
             // Status might already be set by callbacks or error,
             // but set default if needed
             if (currentStatus != "Stopped" && currentStatus != "File End" && currentStatus != "Stream End" && !currentStatus.startsWith("ERR")) {
                 currentStatus = "Stopped"; statusColor = TFT_RED;
             }
             currentBitrate = "";
        } else {
             // Status is usually set by callback on successful play
        }
        updateDisplay(); // Update screen on state change
    }


    // --- Handle Potentiometer ---
    // Read periodically to avoid blocking loop too much
    if (currentTime - lastPotRead > potReadInterval) {
        handlePotentiometer(); // Reads pot and updates display IF volume changed
        lastPotRead = currentTime;
    }

    // --- Status LED ---
    static uint32_t lastLedToggle = 0;
    if (playflag) {
        if (currentTime - lastLedToggle > 500) { // Blink faster when playing
            lastLedToggle = currentTime;
            ledState = !ledState;
            digitalWrite(LED, ledState ? LOW : HIGH); // Toggle LED (LOW is often ON)
        }
    } else {
        // Ensure LED is off when stopped
        if (ledState) {
             digitalWrite(LED, HIGH); // Turn OFF
             ledState = false;
        }
    }

    // --- Optional: Print RSSI periodically ---
    static uint32_t lastRssiPrint = 0; // Timer for RSSI print
    if (currentTime - lastRssiPrint > 5000) { // Print every 5 seconds
        lastRssiPrint = currentTime;
        if (WiFi.status() == WL_CONNECTED) {
            long rssi = WiFi.RSSI(); Serial.printf("Loop: WiFi RSSI: %ld dBm\n", rssi); Serial.flush();
        } else { Serial.println("Loop: WiFi not connected."); Serial.flush(); }
    }

    // --- Yield CPU ---
    yield(); // Allow other tasks to run, especially WiFi and background tasks

} // End of loop()


// =========================================================================
// Button2 Handler Functions (Assigned to SWAPPED Buttons)
// =========================================================================

// Called on short tap of External Button C (GPIO 12)
void handlePlayStopTap(Button2& btn) {
    Serial.println("Tap: Play/Stop Button (External)");
     if (!playflag) { // If stopped, try to play
         if (WiFi.status() == WL_CONNECTED && !arrayURL.empty()) {
             Serial.println(" -> Start Playing");
             currentStatus = "Connecting"; statusColor = TFT_ORANGE;
             updateDisplay(); // Show "Connecting"
             if (sflag >= arrayURL.size()) sflag = 0; // Bounds check
             char* currentURL = (char*)arrayURL[sflag].c_str(); // Get C-string from String vector
             bool connected = audio.connecttohost(currentURL);
             if (!connected) {
                 currentStatus = "Connect FAIL"; statusColor = TFT_RED;
                 updateDisplay();
             }
             // Status updated further by callbacks on success/failure
         } else if (arrayURL.empty()) {
             Serial.println(" -> No stations to play.");
             currentStatus = "No Stations"; statusColor = TFT_RED;
             updateDisplay();
         } else {
             Serial.println(" -> WiFi not connected.");
             currentStatus = "No WiFi"; statusColor = TFT_RED;
             updateDisplay();
         }
     } else { // If playing, stop
         Serial.println(" -> Stop Playing");
         currentStatus = "Stopped"; statusColor = TFT_RED;
         audio.stopSong();
         // playflag will be updated in loop(), which calls updateDisplay()
         updateDisplay(); // Ensure display updates immediately
     }
}

// Called on long press of External Button C (GPIO 12)
void enterConfigModeLongClick(Button2& btn) {
    Serial.println("Long Click: Enter Config Mode Button (External)");
    audio.stopSong(); // Stop audio if playing
    currentStatus = "Reboot Cfg"; statusColor = TFT_MAGENTA;
    currentStreamTitle = "Rebooting to Config Mode...";
    updateDisplay(); // Show message on screen

    preferences.begin("config", false); // Open NVS read-write
    preferences.putBool("enterConfig", true); // Set the flag
    preferences.end(); // Close NVS
    Serial.println("Config flag set in NVS. Rebooting...");
    delay(2000); // Show message on screen briefly
    ESP.restart(); // Reboot the ESP32
}

// Called on short tap of External Button D (GPIO 17)
void handleNextStationTap(Button2& btn) {
    Serial.println("Tap: Next Station Button (External)");
    if (!playflag && !arrayStation.empty()) { // Change station ONLY if stopped AND list not empty
        sflag = (sflag + 1) % arrayStation.size(); // Cycle index using vector size
        currentStationName = arrayStation[sflag]; // Update global station name String
        Serial.printf(" -> Next Station: %d - %s\n", sflag, currentStationName.c_str());
        currentStatus = "Ready"; statusColor = TFT_YELLOW;
        currentStreamTitle = ""; // Clear title area
        currentBitrate = "";
        updateDisplay(); // Update TFT
    } else if (arrayStation.empty()) {
         Serial.println(" -> Station list empty.");
         currentStatus = "No Stations"; statusColor = TFT_RED;
         updateDisplay();
    } else { // If playing, button does nothing
         Serial.println(" -> No action (player running)");
    }
}

// Called on short tap of TTGO Bottom Button (GPIO 0)
void handleBacklightTap(Button2& btn) {
    Serial.println("Tap: Backlight Button (TTGO Btn A)");
    currentBacklightIndex = (currentBacklightIndex + 1) % 5; // Cycle brightness index 0-4
    #ifdef TFT_BL
        ledcWrite(pwmLedChannelTFT, backlightLevels[currentBacklightIndex]); // Set new PWM duty cycle
        Serial.printf(" -> Backlight level %d, PWM %d\n", currentBacklightIndex, backlightLevels[currentBacklightIndex]);
    #else
         Serial.println(" -> Backlight control disabled (TFT_BL not defined).");
    #endif
}

// Called on short tap of TTGO Top Button (GPIO 35)
void handleInvertTap(Button2& btn) {
    Serial.println("Tap: Invert Button (TTGO Btn B)");
    invertDisplayState = !invertDisplayState; // Toggle invert state
    Serial.printf(" -> Invert Display: %s\n", invertDisplayState ? "ON" : "OFF");
    tft.invertDisplay(invertDisplayState); // Apply inversion to TFT
    // Optional: Redraw if clearing doesn't work well with inversion
    // delay(100); drawScreenLayout(); updateDisplay();
}


// =========================================================================
// HELPER FUNCTIONS (Potentiometer, HTML Escape, Config Loading/Saving)
// =========================================================================

// Reads potentiometer and sets audio volume if changed
void handlePotentiometer() {
    int potValue = analogRead(POT_PIN); // Read ADC value (0-4095 for ESP32 12-bit ADC)

    // Map the 12-bit ADC value (0-4095) to the 0-21 volume range
    int newVolume;
    if (potValue < 50) { // Deadzone at the bottom (adjust if needed)
        newVolume = 0;
    } else {
        // Map the range 50-4095 linearly to 0-21
        newVolume = map(potValue, 50, 4095, 0, 21);
    }
    newVolume = constrain(newVolume, 0, 21); // Ensure value stays within 0-21

    // Update volume only if the mapped value actually changed
    if (newVolume != currentVolume) {
        currentVolume = newVolume;       // Update global volume variable
        audio.setVolume(currentVolume);  // Set volume in the audio library
        updateDisplay();                 // Update the display (volume part)
        Serial.printf("Volume set by pot: %d (ADC: %d)\n", currentVolume, potValue);
    }
}

// Helper function to escape characters for safe HTML embedding
String html_escape(String str) {
    str.replace("&", "&amp;");
    str.replace("<", "&lt;");
    str.replace(">", "&gt;");
    str.replace("\"", "&quot;");
    str.replace("'", "&#039;");
    return str;
}

// Helper function to populate the station list with defaults
void loadDefaultStations() {
    Serial.println("Loading default station list.");
    arrayURL.clear();      // Clear any existing entries first
    arrayStation.clear();

    // --- Add your desired default stations here ---
    arrayStation.push_back("Virgin Radio IT");  arrayURL.push_back("http://icecast.unitedradio.it/Virgin.mp3");
    arrayStation.push_back("KPop Radio CO");    arrayURL.push_back("http://streamer.radio.co/s06b196587/listen");
    arrayStation.push_back("Classic FM UK");    arrayURL.push_back("http://media-ice.musicradio.com:80/ClassicFMMP3");
    arrayStation.push_back("MAXXED Out");       arrayURL.push_back("http://149.56.195.94:8015/steam"); // Check reliability?
    arrayStation.push_back("SomaFM GrooveSalad"); arrayURL.push_back("http://ice1.somafm.com/groovesalad-128-mp3");
    arrayStation.push_back("KissFM NL");        arrayURL.push_back("http://streamic.kiss-fm.nl/KissFM");
    // Add or remove stations as needed
    // --------------------------------------------

    sflag = 0; // Reset station index to the first default station
    Serial.printf("Loaded %d default stations.\n", arrayURL.size());
}

// Loads WiFi credentials from NVS and station list from SPIFFS (or defaults)
bool loadConfiguration() {
    Serial.println("Loading configuration...");
    // Check NVS for WiFi creds (don't need to read them here, connectToWiFi does)
    if (!preferences.begin("wifi-creds", true)) { Serial.println("WARNING: NVS Namespace 'wifi-creds' not found or failed to open (read-only)."); }
    preferences.end();

    Serial.println("Attempting to load stations from " + String(STATIONS_PATH));
    bool loaded_from_file = false;
    if (SPIFFS.exists(STATIONS_PATH)) {
        File file = SPIFFS.open(STATIONS_PATH, "r");
        if (!file) { Serial.println("ERROR: Failed to open stations file for reading, though it exists!"); }
        else {
            DynamicJsonDocument doc(3072); // Adjust size if needed for many stations
            DeserializationError error = deserializeJson(doc, file);
            file.close();
            if (error) { Serial.print("ERROR: deserializeJson() failed: "); Serial.println(error.c_str()); Serial.println("Stations file might be corrupted."); }
            else {
                arrayURL.clear(); arrayStation.clear();
                JsonArray stationsArray = doc.as<JsonArray>();
                int valid_stations_in_file = 0;
                for (JsonObject stationObj : stationsArray) {
                    if (stationObj.containsKey("name") && stationObj.containsKey("url")) {
                        String name = stationObj["name"].as<String>();
                        String url = stationObj["url"].as<String>();
                        if (name.length() > 0 && url.length() > 0 && (url.startsWith("http://") /*|| url.startsWith("https://")*/)) {
                            arrayStation.push_back(name); arrayURL.push_back(url); valid_stations_in_file++;
                        } else { Serial.println("Skipping invalid station entry found in JSON file (name/URL invalid)."); }
                    } else { Serial.println("Skipping invalid station entry found in JSON file (missing name or url key)."); }
                }
                if (valid_stations_in_file > 0) { Serial.printf("Loaded %d stations successfully from SPIFFS.\n", valid_stations_in_file); loaded_from_file = true; }
                else { Serial.println("Stations file was valid JSON but contained no valid stations."); }
            }
        }
    } else { Serial.println("Stations file not found."); }

    if (!loaded_from_file) {
        loadDefaultStations();
    }

    sflag = 0; // Start with the first station
    if (arrayStation.empty()) {
        Serial.println("WARNING: Station list is empty after load/default attempt!");
        currentStationName = "No Stations";
    } else {
        if (sflag >= arrayStation.size()) sflag = 0; // Bounds check just in case
        currentStationName = arrayStation[sflag];
    }

    Serial.println("Configuration loading finished.");
    return true; // Indicate success/completion
}

// Saves the current station list (in global vectors) to SPIFFS
bool saveStationsToSPIFFS() {
    Serial.println("Saving stations to " + String(STATIONS_PATH));
    DynamicJsonDocument doc(3072); // Should match loading size
    JsonArray stationsArray = doc.to<JsonArray>();

    if (arrayURL.size() != arrayStation.size()) { Serial.println("FATAL ERROR: URL and Station Name lists have different sizes! Cannot save."); return false; }

    for (size_t i = 0; i < arrayURL.size(); ++i) {
        JsonObject stationObj = stationsArray.createNestedObject();
        stationObj["name"] = arrayStation[i];
        stationObj["url"] = arrayURL[i];
    }

    File file = SPIFFS.open(STATIONS_PATH, "w");
    if (!file) { Serial.println("ERROR: Failed to open stations file for writing"); return false; }

    size_t bytesWritten = serializeJson(doc, file);
    file.close();

    if (bytesWritten == 0 && arrayURL.size() > 0) { // Check if write seemed to fail
        Serial.println("ERROR: Failed to write to stations file (serializeJson returned 0).");
        return false;
    }
    Serial.printf("Stations saved successfully to SPIFFS (%d bytes written).\n", bytesWritten);
    return true;
}


// =========================================================================
// CONFIGURATION PORTAL IMPLEMENTATION (AP Mode)
// =========================================================================

// Starts the WiFi Access Point and Web Server for configuration
void startConfigurationPortal() {
    Serial.println("Setting up Access Point...");
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    delay(100);
    IPAddress apIP = WiFi.softAPIP();
    Serial.print("AP IP address: "); Serial.println(apIP);

    server.on("/", HTTP_GET, handleRoot); // handleRoot performs scan inside
    server.on("/savewifi", HTTP_POST, handleSaveWifi);
    server.on("/savestations", HTTP_POST, handleSaveStations);
    server.on("/reboot", HTTP_GET, handleReboot);
    server.onNotFound(handleNotFound);

    server.begin();
    Serial.println("HTTP server started. Connect to WiFi '" + String(AP_SSID) + "' and browse to http://192.168.4.1");
    Serial.println("NOTE: Web page load includes WiFi scan and may take a few seconds."); // Serial warning

    while (true) {
        server.handleClient(); // Process incoming web requests (like '/')
        delay(2);
    }
}

// Serves the main HTML configuration page (performs scan inside)
void handleRoot() {
    Serial.println("Serving root page '/' - Starting WiFi Scan..."); // Log scan start

    String html = "<!DOCTYPE html><html><head><title>Radio Config</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    // Basic CSS Styling
    html += "<style>";
    html += "body { font-family: sans-serif; margin: 15px; background-color: #f4f4f4; color: #333;}";
    html += "h1, h2, h3 { color: #0056b3; }";
    html += "label { display: block; margin-top: 12px; font-weight: bold; }";
    html += "input[type=text], input[type=password], input[type=url], select, button, textarea { margin-top: 6px; margin-bottom: 10px; padding: 10px; border: 1px solid #ccc; border-radius: 4px; width: 95%; max-width: 400px; box-sizing: border-box; }";
    html += "button { background-color: #007bff; color: white; cursor: pointer; border: none; padding: 10px 15px; border-radius: 4px; }";
    html += "button:hover { background-color: #0056b3; }";
    html += "button.delete { background-color: #dc3545; }";
    html += "button.delete:hover { background-color: #c82333; }";
    html += "ul#station-list { list-style: none; padding: 0; max-width: 600px; }";
    html += "ul#station-list li { background-color: #fff; border: 1px solid #ddd; margin-bottom: 8px; padding: 10px 15px; border-radius: 4px; position: relative; word-wrap: break-word; display: flex; justify-content: space-between; align-items: center; }";
    html += "ul#station-list li .info { flex-grow: 1; margin-right: 10px; }";
    html += "ul#station-list li .info b { color: #555; }";
    html += ".del-btn { color: #dc3545; background: none; border: none; font-size: 1.5em; font-weight: bold; cursor: pointer; padding: 0 5px; line-height: 1; }";
    html += ".del-btn:hover { color: #a71d2a; }";
    html += "hr { border: 0; border-top: 1px solid #eee; margin: 25px 0; }";
    html += ".form-section { background-color: #fff; padding: 20px; border-radius: 5px; margin-bottom: 20px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }";
    html += ".note { font-size: 0.9em; color: #666; margin-top: 5px; margin-bottom: 15px;}";
    html += "</style>";
    html += "</head><body>";

    html += "<h1>ESP32 Radio Configuration</h1>";

    // --- WiFi Credentials Form ---
    html += "<div class='form-section'>";
    preferences.begin("wifi-creds", true);
    String current_ssid = preferences.getString("ssid", "");
    preferences.end();
    html += "<h2>WiFi Credentials</h2>";
    html += "<form action='/savewifi' method='POST'>";
    html += "<label for='ssid-select'>Available Networks (Scan Results):</label>";
    html += "<select id='ssid-select' name='ssid-select' onchange='document.getElementById(\"ssid\").value=this.value'>";
    html += "<option value=''>-- Select Network --</option>";

    // --- Perform WiFi Scan HERE ---
    int n = WiFi.scanNetworks();
    Serial.printf("Scan finished inside handleRoot, %d networks found.\n", n);
    // --- Scan Finished ---

    if (n > 0) {
        for (int i = 0; i < n; ++i) {
            String scanned_ssid = WiFi.SSID(i);
            String selected_attr = (scanned_ssid == current_ssid) ? " selected" : "";
            String security_type;
            switch(WiFi.encryptionType(i)) {
                case WIFI_AUTH_OPEN:            security_type = "Open"; break;
                case WIFI_AUTH_WEP:             security_type = "WEP"; break;
                case WIFI_AUTH_WPA_PSK:         security_type = "WPA-PSK"; break;
                case WIFI_AUTH_WPA2_PSK:        security_type = "WPA2-PSK"; break;
                case WIFI_AUTH_WPA_WPA2_PSK:    security_type = "WPA/WPA2-PSK"; break;
                case WIFI_AUTH_WPA2_ENTERPRISE: security_type = "WPA2-Ent"; break;
                case WIFI_AUTH_WPA3_PSK:        security_type = "WPA3-PSK"; break;
                case WIFI_AUTH_WPA2_WPA3_PSK:   security_type = "WPA2/WPA3-PSK"; break;
                default:                        security_type = "Unknown"; break;
            }
            html += "<option value='" + html_escape(scanned_ssid) + "'" + selected_attr + ">";
            html += html_escape(scanned_ssid) + " (" + WiFi.RSSI(i) + " dBm, " + security_type + ")";
            html += "</option>";
        }
    } else {
        html += "<option value=''>No networks found</option>";
    }
    WiFi.scanDelete();

    html += "</select><br>";
    html += "<label for='ssid'>Network Name (SSID):</label>";
    html += "<input type='text' id='ssid' name='ssid' value='" + html_escape(current_ssid) + "' placeholder='Select from list or type SSID' required><br>";
    html += "<label for='password'>Password:</label>";
    html += "<input type='password' id='password' name='password' placeholder='Leave blank for Open network'><br>";
    html += "<div class='note'>Note: Only WPA/WPA2/WPA3-PSK (Password) networks are supported via this form. Leave password blank for Open networks.<br>Enterprise (Username/Password) networks require code changes using ESP-IDF functions.</div>";
    html += "<button type='submit'>Save WiFi & Reboot</button>";
    html += "</form>";
    html += "</div>";

    // --- Radio Stations Form ---
    html += "<div class='form-section'>";
    html += "<h2>Radio Stations</h2>";
    html += "<form id='stations-form' action='/savestations' method='POST'>";
    html += "<div>Current Stations: (<span id='station-count'>" + String(arrayStation.size()) + "</span>)</div>";
    html += "<ul id='station-list'>";
    for (size_t i = 0; i < arrayStation.size(); ++i) {
        html += "<li id='station-item-" + String(i) + "'>";
        html += "<div class='info'>";
        html += "<b>Name:</b> " + html_escape(arrayStation[i]) + "<br>";
        html += "<b>URL:</b> " + html_escape(arrayURL[i]);
        html += "</div>";
        html += "<button type='button' class='del-btn' onclick='deleteStation(this)'>&times;</button>";
        html += "<input type='hidden' name='name" + String(i) + "' value='" + html_escape(arrayStation[i]) + "'>";
        html += "<input type='hidden' name='url" + String(i) + "' value='" + html_escape(arrayURL[i]) + "'>";
        html += "</li>";
    }
    html += "</ul>";
    html += "<h3>Add New Station</h3>";
    html += "<label for='newName'>Name:</label>";
    html += "<input type='text' id='newName' placeholder='Station Name'><br>";
    html += "<label for='newURL'>URL (must start with http://):</label>";
    html += "<input type='url' id='newURL' placeholder='http://...' pattern='http://.*'>";
    html += "<button type='button' onclick='addStationToList()'>Add to List Below</button>";
    html += "<hr>";
    html += "<button type='submit'>Save Station List & Reboot</button>";
    html += "</form>";
    html += "</div>";

    // --- JavaScript for managing the station list dynamically ---
    html += "<script>";
    html += "let newStationCounter = " + String(arrayStation.size()) + ";";
    html += "function escapeHTML(str) { let temp = document.createElement('div'); temp.textContent = str; return temp.innerHTML; }";
    html += "function addStationToList() {";
    html += " var nameInput = document.getElementById('newName');";
    html += " var urlInput = document.getElementById('newURL');";
    html += " var name = nameInput.value.trim();";
    html += " var url = urlInput.value.trim();";
    html += " if (!name || !url || !url.startsWith('http://')) { alert('Please enter a valid Name and a URL starting with http://'); return; }";
    html += " var list = document.getElementById('station-list');";
    html += " var newItem = document.createElement('li');";
    html += " var currentItemIndex = newStationCounter++;";
    html += " newItem.id = 'station-item-' + currentItemIndex;";
    html += " newItem.innerHTML = \"<div class='info'><b>Name:</b> \" + escapeHTML(name) + \"<br><b>URL:</b> \" + escapeHTML(url) + \"</div>\" + ";
    html += "                   \"<button type='button' class='del-btn' onclick='deleteStation(this)'>&times;</button>\" +";
    html += "                   \"<input type='hidden' name='name\" + currentItemIndex + \"' value='\" + escapeHTML(name) + \"'>\" +";
    html += "                   \"<input type='hidden' name='url\" + currentItemIndex + \"' value='\" + escapeHTML(url) + \"'>\";";
    html += " list.appendChild(newItem);";
    html += " nameInput.value = ''; urlInput.value = '';";
    html += " document.getElementById('station-count').innerText = list.children.length;";
    html += "}";
    html += "function deleteStation(btn) {";
    html += " var listItem = btn.parentNode;";
    html += " listItem.parentNode.removeChild(listItem);";
    html += " document.getElementById('station-count').innerText = document.getElementById('station-list').children.length;";
    html += "}";
    html += "</script>";

    html += "</body></html>";
    server.send(200, "text/html", html); // Send the complete HTML page
    Serial.println("Root page '/' served."); // Log completion
}

// Handles saving WiFi credentials
void handleSaveWifi() {
    Serial.println("Processing POST /savewifi");
    if (server.hasArg("ssid") && server.hasArg("password")) {
        String ssid = server.arg("ssid");
        String password = server.arg("password");
        Serial.println("Received SSID: " + ssid);

        preferences.begin("wifi-creds", false);
        preferences.putString("ssid", ssid);
        preferences.putString("password", password);
        preferences.end();

        Serial.println("WiFi credentials saved.");
        String html = "<!DOCTYPE html><html><head><title>WiFi Saved</title></head><body>";
        html += "<h2>WiFi Settings Saved!</h2>";
        html += "<p>The radio will now reboot and attempt to connect.</p>";
        html += "<p>Rebooting in <span id='countdown'>3</span> seconds...</p>";
        html += "<button onclick=\"window.location.href='/reboot'\">Reboot Now</button>";
        html += "<script>var count = 3; function countdown(){ count--; if(count>=0) document.getElementById('countdown').innerText=count; else window.location.href='/reboot'; } setInterval(countdown, 1000);</script>";
        html += "</body></html>";
        server.send(200, "text/html", html);
    } else {
        Serial.println("ERROR: Missing ssid or password in /savewifi request.");
        server.send(400, "text/plain", "Bad Request: Missing SSID or Password parameter.");
    }
}

// Handles saving the station list
void handleSaveStations() {
    Serial.println("Processing POST /savestations");
    arrayURL.clear(); arrayStation.clear();
    for (int i = 0; i < server.args(); i++) {
        String argName = server.argName(i);
        if (argName.startsWith("name")) {
            String name = server.arg(i);
            String urlArgName = "url" + argName.substring(4);
            if (server.hasArg(urlArgName)) {
                String url = server.arg(urlArgName);
                if (name.length() > 0 && url.length() > 0 && url.startsWith("http://")) {
                     arrayStation.push_back(name); arrayURL.push_back(url);
                     Serial.println("  Adding station to save: \"" + name + "\" -> " + url);
                } else { Serial.println("  Skipping invalid station entry from form submission: Name='" + name + "', URL='" + url + "'"); }
            }
        }
    }
    Serial.printf("Received %d valid stations from form to save.\n", arrayStation.size());
    if (saveStationsToSPIFFS()) {
        Serial.println("Stations saved successfully to SPIFFS.");
        String html = "<!DOCTYPE html><html><head><title>Stations Saved</title></head><body>";
        html += "<h2>Station List Saved!</h2>";
        html += "<p>The radio will now reboot and use the updated station list.</p>";
        html += "<p>Rebooting in <span id='countdown'>3</span> seconds...</p>";
        html += "<button onclick=\"window.location.href='/reboot'\">Reboot Now</button>";
        html += "<script>var count = 3; function countdown(){ count--; if(count>=0) document.getElementById('countdown').innerText=count; else window.location.href='/reboot'; } setInterval(countdown, 1000);</script>";
        html += "</body></html>";
        server.send(200, "text/html", html);
    } else {
        Serial.println("ERROR: Failed to save stations to SPIFFS!");
        server.send(500, "text/plain", "Error: Could not save station list.");
    }
}

// Handles the reboot request from the web page
void handleReboot() {
    Serial.println("Received /reboot request. Rebooting...");
    String html = "<!DOCTYPE html><html><head><title>Rebooting</title></head><body><h2>Rebooting...</h2>Please wait and reconnect to your normal WiFi network.</body></html>";
    server.send(200, "text/html", html);
    delay(500);
    ESP.restart();
}

// Handles requests for non-existent pages
void handleNotFound() {
    Serial.println("Serving 404 Not Found");
    server.send(404, "text/plain", "404: Not Found");
}


// =========================================================================
// WIFI CONNECTION (Uses NVS credentials, handles Open Networks, includes Failsafe)
// =========================================================================
void connectToWiFi() {
    preferences.begin("wifi-creds", true); // Open NVS read-only
    String sta_ssid = preferences.getString("ssid", "");
    String sta_password = preferences.getString("password", ""); // Get saved pwd (might be empty)
    preferences.end(); // Close NVS

    if (sta_ssid.length() == 0) {
        Serial.println("WiFi credentials not set. Cannot connect.");
        Serial.println("Use External Button C (GPIO 12) long press OR fix WiFi to enter Config Mode.");
        currentStatus = "No WiFi Cfg"; statusColor = TFT_RED;
        updateDisplay(); // Update screen
        return; // Exit connection attempt
    }

    int retryCount = 0;
    const int maxRetries = 3; // Try up to 3 times before triggering config mode

    while (retryCount < maxRetries) {
        Serial.printf("Connecting to WiFi: %s (Attempt %d/%d)", sta_ssid.c_str(), retryCount + 1, maxRetries);
        currentStatus = "WiFi Try " + String(retryCount + 1); statusColor = TFT_ORANGE;
        updateDisplay(); // Update display

        WiFi.mode(WIFI_STA); // Set Station mode

        // Check if password is empty for Open Network
        if (sta_password.length() == 0) {
            Serial.println(" (Open Network - No Password)");
            WiFi.begin(sta_ssid.c_str()); // Call begin with only SSID
        } else {
            Serial.println(" (Password Protected)");
            WiFi.begin(sta_ssid.c_str(), sta_password.c_str()); // Call begin with SSID and Password
        }

        unsigned long startAttemptTime = millis();
        // Wait for connection or timeout (e.g., 15 seconds per attempt)
        while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 15000) {
            Serial.print(".");
            // Allow button presses to be detected even while waiting for WiFi
            buttonBacklight.loop(); buttonInvert.loop(); buttonPlayStopCfg.loop(); buttonNext.loop();
            delay(500);
        }

        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("\nWiFi connected successfully!");
            Serial.print("IP Address: "); Serial.println(WiFi.localIP());
            currentStatus = "Connected"; statusColor = TFT_GREEN; // Update display status
            updateDisplay(); // Update screen with IP etc.
            return; // <<< SUCCESS! Exit the function normally.
        } else {
            Serial.println("\nWiFi Connection Attempt failed.");
            WiFi.disconnect(true); // Ensure disconnected before retry
            delay(1000); // Wait a second before retrying
            retryCount++;
            currentStatus = "WiFi Fail " + String(retryCount); statusColor = TFT_RED;
            updateDisplay(); // Show failure on display
        }
    } // End while retries

    // --- If we exit the loop, all retries have failed ---
    Serial.println("WiFi connection failed after multiple retries.");
    Serial.println(">>> Entering Configuration Mode on next boot via Failsafe <<<");
    currentStatus = "WiFi Failed!"; statusColor = TFT_RED;
    currentStreamTitle = "Enter Cfg Mode on Reboot"; // Inform user on screen
    updateDisplay();

    // Set the NVS flag to force config mode on next boot
    preferences.begin("config", false); // Open NVS read-write
    preferences.putBool("enterConfig", true); // Set the flag
    preferences.end(); // Close NVS

    delay(4000); // Display message for a few seconds before rebooting
    ESP.restart(); // Reboot into Config Mode
}


// =========================================================================
// AUDIO LIBRARY CALLBACK FUNCTIONS (esp32-audioI2S style)
// =========================================================================

void audio_info(const char *info){
    Serial.print("Info:       "); Serial.println(info);
    String infoStr = String(info); infoStr.toLowerCase(); // Easier check
    if (infoStr.indexOf("error") != -1) {
        if(currentStatus != "Connect FAIL") {
             currentStatus = "Stream Err"; statusColor = TFT_RED; updateDisplay();
        }
    } else if (infoStr.indexOf("stream ready") != -1) {
        currentStatus = "Playing"; statusColor = TFT_GREEN; updateDisplay();
    } else if (infoStr.indexOf("buffer filled") != -1){
        if(playflag && currentStatus != "Playing") {
            currentStatus = "Playing"; statusColor = TFT_GREEN; updateDisplay();
        }
    } else if (infoStr.indexOf("flac sync") != -1) {
        currentStatus = "FLAC..."; statusColor = TFT_CYAN; updateDisplay();
    } else if (infoStr.indexOf("aac sync") != -1) {
        currentStatus = "AAC..."; statusColor = TFT_CYAN; updateDisplay();
    }
}

void audio_id3data(const char *info){
    Serial.print("ID3:        "); Serial.println(info);
    if (currentStreamTitle.length() == 0 && strlen(info) > 5) {
         currentStreamTitle = String(info);
         updateDisplay();
    }
}

void audio_eof_mp3(const char *info){
    Serial.print("EOF MP3:    "); Serial.println(info);
    currentStatus = "File End"; statusColor = TFT_YELLOW;
    playflag = false; // Update flag
    updateDisplay();
}

void audio_showstation(const char *info){
    Serial.print("Station:    "); Serial.println(info);
    if (strlen(info) > 0) {
        currentStationName = String(info);
    } else {
        if (!arrayStation.empty() && sflag < arrayStation.size()) {
            currentStationName = arrayStation[sflag];
        } else {
            currentStationName = "Unknown";
        }
    }
    updateDisplay();
}

void audio_showstreamtitle(const char *info){
    Serial.print("Title:      "); Serial.println(info);
    String newTitle = String(info);
    if (newTitle != currentStreamTitle) { // Update only if changed
        currentStreamTitle = newTitle;
        updateDisplay();
    }
}

void audio_bitrate(const char *info){
    Serial.print("Bitrate:    "); Serial.print(info); Serial.println("kbps");
    currentBitrate = String(info) + "kbps";
    updateDisplay(); // Update the bitrate on screen
}

void audio_commercial(const char *info){
    Serial.print("Commercial: "); Serial.println(info);
    currentStatus = "Ad break"; statusColor = TFT_MAGENTA;
    currentStreamTitle = ""; // Clear title during ad
    updateDisplay();
}

void audio_icyurl(const char *info){
    Serial.print("ICY URL:    "); Serial.println(info);
}

void audio_lasthost(const char *info){
    Serial.print("Last Host:  "); Serial.println(info);
}

void audio_eof_stream(const char *info){
    Serial.print("Stream End: "); Serial.println(info);
    currentStatus = "Stream End"; statusColor = TFT_YELLOW;
    playflag = false; // Update flag
    currentStreamTitle = ""; // Clear title
    currentBitrate = "";
    updateDisplay();
}
