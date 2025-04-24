// =========================================================================
// Includes
// =========================================================================
#include <WiFi.h>
#include "Audio.h"       // Use compatible version (e.g., 2.0.3 tag for Core 2.0.14)
#include <TFT_eSPI.h>    // Graphics and font library (Requires configuration)
#include <SPI.h>         // Required by TFT_eSPI
#include <WebServer.h>   // Standard ESP32 Web Server (for Config Portal)
#include <FS.h>          // Filesystem base
#include <SPIFFS.h>      // Filesystem implementation
#include <Preferences.h> // For NVS storage (WiFi credentials & Config Flag)
#include <ArduinoJson.h> // For handling station list saving/loading (Install via Library Manager)
#include <Button2.h>     // For button handling (Tap, Long Press) - Install via Library Manager
#include <vector>        // For dynamic station lists

// --- Include project-specific header files for graphics ---
// --- Ensure these files are in the same directory as your .ino file ---
#include "frame.h"       // Defines: frame[][], animation_width, animation_height, frames
#include "background.h"  // Defines background image array
#include "Orbitron_Medium_20.h" // Font data

// =========================================================================
// Definitions and Constants
// =========================================================================

// --- TFT Objects and Settings ---
TFT_eSPI tft = TFT_eSPI();  // Invoke library, pins defined in User_Setup.h
#define TFT_GREY 0x5AEB     // Custom grey color

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

// Buttons and LED
const int LED = 13;  // Status LED Pin (If available/desired on your board)
const int BTNA = 0;  // GPIO Play/Pause / Config Trigger (Long Press) - (TTGO Bottom Button)
const int BTNB = 35; // GPIO Next Station - Input Only Pin (TTGO Top Button)
const int BTNC = 12; // GPIO Backlight Brightness (Requires external button to GND)
const int BTND = 17; // GPIO Invert Display (Requires external button to GND)

// --- Web Configuration Portal ---
// Config Mode is triggered by NVS flag (set via Long Press or WiFi Fail)
const char* AP_SSID = "RadioConfigPortal"; // Name of the WiFi network created by the ESP32
const char* AP_PASSWORD = "password123";   // Password for the configuration network (CHANGE THIS!)
const char* STATIONS_PATH = "/stations.json"; // Filename for station list on SPIFFS

// --- Button Timing ---
#define LONG_PRESS_MS 3000 // 3 seconds for long press trigger

// =========================================================================
// Global Objects
// =========================================================================
Audio audio;             // Instance of the ESP32-audioI2S library
WebServer server(80);      // Web server for configuration
Preferences preferences;   // Object for accessing Non-Volatile Storage
Button2 buttonA;           // Button object for BTNA (GPIO 0)
Button2 buttonB;           // Button object for BTNB (GPIO 35)
Button2 buttonC;           // Button object for BTNC (GPIO 12) - External Button
Button2 buttonD;           // Button object for BTND (GPIO 17) - External Button


// =========================================================================
// Global Variables
// =========================================================================

// --- Dynamic Station Data ---
std::vector<String> arrayURL;     // Vector to hold station URLs
std::vector<String> arrayStation; // Vector to hold station names
String station = "No Stations";   // Holds the name of the currently selected station
int sflag = 0;                    // Current station index in the vectors

// --- Playback & Control State ---
volatile bool playflag = false;   // Playback state flag (true if audio.isRunning())
int currentVolume = 10;           // Current volume (0-21), potentially set by pot at startup
unsigned long lastPotRead = 0;    // Timer for reading potentiometer
const int potReadInterval = 100;  // Read pot every 100ms
int ledflag = 0;                  // For blinking status LED

// --- Display & UI State ---
int backlight[5] = {10, 30, 60, 120, 220}; // PWM duty cycles for backlight
byte b = 2;                       // Index for current backlight level (0-4)
bool inv = 0;                     // Invert display state
float n = 0;                      // Animation frame counter
uint32_t lastAnimUpdate = 0;      // Timer for animation update

// =========================================================================
// Function Prototypes
// =========================================================================

// --- Mode Setup ---
void setupRadioMode();

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
void drawVolume();
void drawStation();
void drawStatus(String status, uint16_t color);
void drawTitle(String title);
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

    Serial.println("\n\n--- Booting ESP32 Radio ---");

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
        // Initialize TFT just for config message
        tft.init();
        tft.setRotation(0); // Match radio mode rotation
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.setCursor(10, 10); tft.setTextSize(2); tft.println("Config Mode");
        tft.setTextSize(1);
        tft.setCursor(10, 40); tft.println("Connect WiFi to:");
        tft.setCursor(10, 55); tft.println(AP_SSID);
        tft.setCursor(10, 70); tft.println("Password:");
        tft.setCursor(10, 85); tft.println(AP_PASSWORD);
        tft.setCursor(10, 105); tft.println("Browser to:");
        tft.setCursor(10, 120); tft.println("192.168.4.1");

        // Initialize Filesystem (needed to load stations for display on web page)
        if (!SPIFFS.begin(true)) { // true = format if mount failed
            Serial.println("CRITICAL: SPIFFS Mount Failed in Config Mode!");
            tft.setTextColor(TFT_RED, TFT_BLACK);
            tft.drawString("FS Error!", 10, 150, 2);
            while (1) delay(1000); // Halt on critical error
        }
        // Load current stations to display them on the web page
        // Ignore return value here, defaults will be loaded if needed by loadConfiguration
        loadConfiguration();

        startConfigurationPortal(); // Enters configuration loop, does not return here

    } else {
        // --- Normal Radio Mode ---
        Serial.println(">>> Starting Radio Mode <<<");

        // Initialize Filesystem & Load Config (WiFi creds & Stations + Defaults if needed)
        if (!SPIFFS.begin(true)) {
            Serial.println("CRITICAL: SPIFFS Mount Failed!");
            tft.init(); tft.fillScreen(TFT_BLACK); tft.setTextColor(TFT_RED);
            tft.drawString("FS Error!", 10, 10, 4); while(1) delay(1000);
        }
        // Load config attempts to load from file OR load defaults if needed
        loadConfiguration(); // Populates vectors and sets initial `station` string

        setupRadioMode(); // Initialize TFT, Buttons, Audio, etc. for Radio operation

        connectToWiFi(); // Tries to connect, updates display, handles WiFi Fail trigger

        Serial.println("--- Setup Complete (Radio Mode) ---");
    }
}

// =========================================================================
// SETUP FUNCTION FOR NORMAL RADIO MODE
// =========================================================================
void setupRadioMode() {
     // Initialize TFT Display
    tft.init();
    tft.setRotation(0);
    tft.setSwapBytes(true);
    tft.setFreeFont(&Orbitron_Medium_20); // Set custom font
    tft.fillScreen(TFT_BLACK);
    tft.pushImage(0, 0, 135, 240, background); // Draw background image

    // Initialize TFT Backlight PWM
    #ifdef TFT_BL // TFT_BL is usually defined in TFT_eSPI User_Setup.h
        Serial.printf("setupRadioMode: TFT_BL defined as GPIO %d\n", TFT_BL);
        ledcSetup(pwmLedChannelTFT, pwmFreq, pwmResolution);
        ledcAttachPin(TFT_BL, pwmLedChannelTFT);
        ledcWrite(pwmLedChannelTFT, backlight[b]); // Set initial brightness
        Serial.println("setupRadioMode: TFT backlight PWM configured.");
    #else
        Serial.println("setupRadioMode: WARNING - TFT_BL pin not defined. Backlight control disabled.");
    #endif

    // Draw Initial Static Screen Elements
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(14, 20); tft.println("Radio"); // Title
    tft.drawLine(0, 28, 135, 28, TFT_GREY);     // Separator line
    for (int i = 0; i < b + 1; i++) { tft.fillRect(108 + (i * 4), 18, 2, 6, TFT_GREEN); } // Draw initial brightness indicator

    // Initialize GPIOs (Input only needed if Button2 doesn't set it)
    pinMode(LED, OUTPUT); digitalWrite(LED, HIGH); // LED Off initially
    // Button2 library handles pin mode internally (INPUT_PULLUP usually)

    // Initialize Button2 objects and handlers
    Serial.println("setupRadioMode: Initializing buttons...");
    buttonA.begin(BTNA, INPUT_PULLUP); // GPIO 0 - Specify PULLUP
    buttonA.setTapHandler(handlePlayStopTap);
    buttonA.setLongClickHandler(enterConfigModeLongClick);
    buttonA.setLongClickTime(LONG_PRESS_MS); // Set long press duration

    buttonB.begin(BTNB, INPUT); // GPIO 35 - Input only, no internal pullup/down
    buttonB.setTapHandler(handleNextStationTap);

    // Initialize external buttons IF their pins are defined (e.g. const int BTNC = 12;)
    // Assumes external buttons are wired between the GPIO pin and GND
    buttonC.begin(BTNC, INPUT_PULLUP); // GPIO 12 - Enable internal pullup
    buttonC.setTapHandler(handleBacklightTap);
    Serial.println(" - BTNC (Backlight) Initialized on GPIO " + String(BTNC));

    buttonD.begin(BTND, INPUT_PULLUP); // GPIO 17 - Enable internal pullup
    buttonD.setTapHandler(handleInvertTap);
    Serial.println(" - BTND (Invert) Initialized on GPIO " + String(BTND));

    Serial.println("setupRadioMode: Buttons initialized.");

    // Initial screen state based on loaded config
    handlePotentiometer(); // Read initial pot position
    drawStatus("Ready", TFT_YELLOW); // Initial status
    drawVolume(); // Draw volume based on initial pot reading
    drawStation(); // Display the initial station name

    // IP Address will be drawn after WiFi connection attempt in setup()

    // Initialize Audio System
    Serial.println("setupRadioMode: Initializing Audio Output..."); Serial.flush();
    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT); // Set I2S pins
    audio.setVolume(currentVolume); // Set initial volume based on pot reading
    Serial.printf("setupRadioMode: Initial Volume: %d\n", currentVolume);
}


// =========================================================================
// NORMAL RADIO LOOP - Runs continuously in Radio Mode
// =========================================================================
void loop() {
    uint32_t currentTime = millis(); // Get current time once per loop

    // --- Handle Button Events ---
    buttonA.loop();
    buttonB.loop();
    buttonC.loop(); // Call loop even if external button not connected (harmless)
    buttonD.loop(); // Call loop even if external button not connected (harmless)


    // --- Handle WiFi Connection Status ---
    if (WiFi.status() != WL_CONNECTED) {
        if (playflag) { // Only stop if it was playing
             audio.stopSong();
             playflag = false; // Manually update flag as audio.loop() might not run
             drawStatus("WiFi Lost", TFT_RED);
             drawTitle(""); // Clear title
             Serial.println("WiFi connection lost. Playback stopped.");
        }
        // No audio processing if not connected
    } else {
        // --- Handle Audio Streaming (Only if WiFi is connected) ---
        audio.loop(); // Process audio buffers, callbacks, etc.
    }

    // Update playflag based on library state AFTER calling audio.loop()
    playflag = audio.isRunning();

    // --- Handle Potentiometer ---
    // Read periodically to avoid blocking loop too much
    if (currentTime - lastPotRead > potReadInterval) {
        handlePotentiometer();
        lastPotRead = currentTime;
    }

    // --- Handle Animation ---
    if (playflag) {
        // Update animation roughly 15 times/sec (every 66ms)
        if (currentTime - lastAnimUpdate > 66) {
            lastAnimUpdate = currentTime;
            int frameIndex = int(n);
            // Check bounds using 'frames' variable from frame.h
            if (frames > 0 && frameIndex >= 0 && frameIndex < frames) {
                // Check animation dimensions from frame.h
                tft.pushImage(50, 126, animation_width, animation_height, frame[frameIndex]);
            } else {
                n = 0; // Reset if index is bad or frames is 0
            }
            n = n + 0.3; // Adjust step for visual speed
            if (frames > 0 && int(n) >= frames) n = 0; // Loop animation
        }
    } else {
        // If stopped, show static frame (draw only if not already showing last frame)
        if (frames > 0) { // Only draw if frames exist
             int lastFrameIndex = frames - 1;
             if (abs(n - lastFrameIndex) > 0.1) { // Check if not already on last frame
                 // Ensure animation dimensions are valid before drawing
                 if (animation_width > 0 && animation_height > 0) {
                    tft.pushImage(50, 126, animation_width, animation_height, frame[lastFrameIndex]);
                 }
                 n = lastFrameIndex; // Set counter to last frame
             }
        }
    }

    // --- Status LED ---
    static uint32_t lastStatusUpdate = 0; // Timer for LED blink
    if (playflag) {
        if (currentTime - lastStatusUpdate > 500) { // Blink faster when playing
            lastStatusUpdate = currentTime;
            ledflag = !ledflag; digitalWrite(LED, ledflag ? LOW : HIGH); // Toggle LED
        }
    } else {
        // Ensure LED is off when stopped
        if (digitalRead(LED) == LOW) { digitalWrite(LED, HIGH); ledflag = 0; }
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
// Button2 Handler Functions
// =========================================================================

// Called on short tap of BTNA (GPIO 0)
void handlePlayStopTap(Button2& btn) {
    Serial.println("Tap: BTNA (Play/Stop)");
     if (!playflag) { // If stopped, try to play
         if (WiFi.status() == WL_CONNECTED && !arrayURL.empty()) {
             Serial.println(" -> Start Playing");
             drawStatus("Connecting", TFT_ORANGE);
             if (sflag >= arrayURL.size()) sflag = 0; // Bounds check
             char* currentURL = (char*)arrayURL[sflag].c_str(); // Get C-string from String vector
             bool connected = audio.connecttohost(currentURL);
             if (!connected) drawStatus("Connect FAIL", TFT_RED);
             // Status updated by callbacks
         } else if (arrayURL.empty()) {
             Serial.println(" -> No stations to play.");
             drawStatus("No Stations", TFT_RED);
         } else {
             Serial.println(" -> WiFi not connected.");
             drawStatus("No WiFi", TFT_RED);
         }
    } else { // If playing, stop
         Serial.println(" -> Stop Playing");
         drawStatus("Stopped", TFT_RED);
         audio.stopSong();
         drawTitle(""); // Clear title
    }
}

// Called on long press of BTNA (GPIO 0)
void enterConfigModeLongClick(Button2& btn) {
    Serial.println("Long Click: BTNA (Enter Config Mode on Reboot)");
    audio.stopSong(); // Stop audio if playing
    drawStatus("Reboot Cfg", TFT_MAGENTA);
    tft.fillRect(10, 140, 115, 36, TFT_BLACK); // Clear title area
    tft.setFreeFont(&Orbitron_Medium_20); tft.setTextSize(1); tft.setTextColor(TFT_YELLOW);
    tft.drawString("Rebooting to", 10, 140, 2);
    tft.drawString("Config Mode...", 10, 158, 2);

    preferences.begin("config", false); // Open NVS read-write
    preferences.putBool("enterConfig", true); // Set the flag
    preferences.end(); // Close NVS
    Serial.println("Config flag set in NVS. Rebooting...");
    delay(2000); // Show message on screen briefly
    ESP.restart(); // Reboot the ESP32
}

// Called on short tap of BTNB (GPIO 35)
void handleNextStationTap(Button2& btn) {
    Serial.println("Tap: BTNB (Next Station)");
    if (!playflag && !arrayStation.empty()) { // Change station ONLY if stopped AND list not empty
        sflag = (sflag + 1) % arrayStation.size(); // Cycle index using vector size
        station = arrayStation[sflag]; // Update global station name String
        Serial.printf(" -> Next Station: %d - %s\n", sflag, station.c_str());
        drawStation(); // Update TFT
        drawStatus("Ready", TFT_YELLOW); // Reset status line
        drawTitle(""); // Clear title area
    } else if (arrayStation.empty()) {
         Serial.println(" -> Station list empty.");
         drawStatus("No Stations", TFT_RED);
    } else { // If playing, button does nothing
         Serial.println(" -> No action (player running)");
    }
}

// Called on short tap of BTNC (GPIO 12) - Requires external button
void handleBacklightTap(Button2& btn) {
    Serial.println("Tap: BTNC (Backlight)");
    tft.fillRect(108, 18, 25, 6, TFT_BLACK); // Clear previous brightness indicators
    b = (b + 1) % 5; // Cycle brightness index 0-4
    for (int i = 0; i < b + 1; i++) { tft.fillRect(108 + (i * 4), 18, 2, 6, TFT_GREEN); } // Draw new indicators
    #ifdef TFT_BL
        ledcWrite(pwmLedChannelTFT, backlight[b]); // Set new PWM duty cycle
        Serial.printf(" -> Backlight level %d, PWM %d\n", b, backlight[b]);
    #endif
}

// Called on short tap of BTND (GPIO 17) - Requires external button
void handleInvertTap(Button2& btn) {
    Serial.println("Tap: BTND (Invert Display)");
    inv = !inv; // Toggle invert state
    Serial.printf(" -> Invert Display: %s\n", inv ? "ON" : "OFF");
    tft.invertDisplay(inv); // Apply inversion to TFT
}


// =========================================================================
// HELPER FUNCTIONS (Continued)
// =========================================================================

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
    arrayURL.clear();       // Clear any existing entries first
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
    // This acts as simple filtering/debouncing for the pot
    if (newVolume != currentVolume) {
        currentVolume = newVolume;       // Update global volume variable
        audio.setVolume(currentVolume);  // Set volume in the audio library
        drawVolume();                    // Update the display
        Serial.printf("Volume set by pot: %d (ADC: %d)\n", currentVolume, potValue);
    }
}

// Draws the current volume level (0-21) on the TFT
void drawVolume() {
    tft.setFreeFont(&Orbitron_Medium_20); // Ensure correct font is active
    tft.setTextSize(1);                   // Set text size for volume display
    tft.setTextColor(TFT_YELLOW, TFT_BLACK); // Set text color
    tft.fillRect(78, 66, 50, 16, TFT_BLACK); // Clear the background area first (adjust width if needed for '21')
    tft.drawString(String(currentVolume), 78, 66, 2); // Draw volume (0-21), Font size 2
}

// Draws the current station name on the TFT
void drawStation() {
    tft.setFreeFont(&Orbitron_Medium_20); // Ensure correct font
    tft.setTextSize(1);                   // Set text size
    tft.setTextColor(TFT_CYAN, TFT_BLACK); // Set text color
    String displayStation = station; // Use the global station string
    if (displayStation.length() > 15) displayStation = displayStation.substring(0, 15); // Truncate if too long
    tft.fillRect(10, 108, 120, 16, TFT_BLACK); // Clear background area
    tft.drawString(displayStation, 12, 108, 2); // Draw station name, Font size 2
}

// Draws a status message (e.g., "Playing", "Stopped") on the TFT
void drawStatus(String status, uint16_t color) {
    tft.setFreeFont(&Orbitron_Medium_20); // Ensure correct font
    tft.setTextSize(1);                   // Set text size
    tft.setTextColor(color, TFT_BLACK);   // Set text color
    tft.fillRect(78, 44, 55, 16, TFT_BLACK); // Clear background area (adjust width for longest status)
    if (status.length() > 10) status = status.substring(0, 10); // Truncate if too long
    tft.drawString(status, 78, 44, 2); // Draw status message, Font size 2
}

// Draws the stream title (metadata) on the TFT, with basic word wrap
void drawTitle(String title) {
    tft.setFreeFont(&Orbitron_Medium_20); // Ensure correct font
    tft.setTextSize(1);                   // Set text size
    tft.setTextColor(TFT_ORANGE, TFT_BLACK); // Set text color for title
    tft.fillRect(10, 140, 115, 36, TFT_BLACK); // Clear background area (allows for two lines)

    title.trim(); // Remove leading/trailing whitespace
    if (title.length() == 0) return; // Don't draw if title is empty

    // Simple word wrap (approximate character limit per line)
    int maxLen = 18; // Adjust based on font width and desired look
    if (title.length() <= maxLen) {
         // Title fits on one line
         tft.drawString(title, 12, 140, 2); // Font size 2
    } else {
        // Title needs wrapping
        int splitPoint = -1;
        // Find the last space character at or before the max length
        for (int i = maxLen; i > 0; i--) {
            if (title.charAt(i) == ' ') {
                splitPoint = i;
                break;
            }
        }

        String line1;
        String line2;

        if (splitPoint != -1) { // Found a space to break at
             line1 = title.substring(0, splitPoint);
             line2 = title.substring(splitPoint + 1); // Start line 2 after the space
        } else { // No space found, just force break at maxLen
             line1 = title.substring(0, maxLen);
             line2 = title.substring(maxLen);
        }

        // Truncate second line if it's still too long
        if (line2.length() > maxLen) line2 = line2.substring(0, maxLen - 3) + "...";

        tft.drawString(line1, 12, 140, 2);      // Draw first line
        tft.drawString(line2, 12, 140 + 18, 2); // Draw second line below (adjust Y offset if needed)
    }
}

// =========================================================================
// CONFIGURATION PORTAL IMPLEMENTATION (AP Mode)
// =========================================================================

// Starts the WiFi Access Point and Web Server for configuration
void startConfigurationPortal() {
    Serial.println("Setting up Access Point...");
    // Start WiFi AP with SSID and Password
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    delay(100); // Short delay for AP to start
    IPAddress apIP = WiFi.softAPIP();
    Serial.print("AP IP address: ");
    Serial.println(apIP); // Should be 192.168.4.1

    // --- Define Web Server Handlers (Routes) ---
    server.on("/", HTTP_GET, handleRoot);             // Serve the main config page
    server.on("/savewifi", HTTP_POST, handleSaveWifi); // Handle saving WiFi credentials
    server.on("/savestations", HTTP_POST, handleSaveStations); // Handle saving station list
    server.on("/reboot", HTTP_GET, handleReboot);       // Handle reboot request
    server.onNotFound(handleNotFound);                // Handle requests for unknown pages

    server.begin(); // Start the web server
    Serial.println("HTTP server started. Connect to WiFi '" + String(AP_SSID) + "' and browse to http://192.168.4.1");

    // Loop indefinitely to handle web server client requests
    // The ESP32 will only exit this mode by rebooting (triggered via web page)
    while (true) {
        server.handleClient(); // Process incoming client connections
        delay(2); // Small delay to prevent tight loop hogging CPU and allow events
    }
}

// --- Web Server Handler Functions ---

// Serves the main HTML configuration page (WITH WIFI SCAN DROPDOWN)
void handleRoot() {
    Serial.println("Serving root page '/'");
    String html = "<!DOCTYPE html><html><head><title>Radio Config</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    // Basic CSS Styling
    html += "<style>";
    html += "body { font-family: sans-serif; margin: 15px; background-color: #f4f4f4; color: #333;}";
    html += "h1, h2, h3 { color: #0056b3; }"; // Adjusted heading color
    html += "label { display: block; margin-top: 12px; font-weight: bold; }";
    html += "input[type=text], input[type=password], input[type=url], select, button, textarea { margin-top: 6px; margin-bottom: 10px; padding: 10px; border: 1px solid #ccc; border-radius: 4px; width: 95%; max-width: 400px; box-sizing: border-box; }"; // Added select
    html += "button { background-color: #007bff; color: white; cursor: pointer; border: none; padding: 10px 15px; border-radius: 4px; }";
    html += "button:hover { background-color: #0056b3; }";
    html += "button.delete { background-color: #dc3545; }"; // Specific delete button style
    html += "button.delete:hover { background-color: #c82333; }";
    html += "ul#station-list { list-style: none; padding: 0; max-width: 600px; }"; // Wider list
    html += "ul#station-list li { background-color: #fff; border: 1px solid #ddd; margin-bottom: 8px; padding: 10px 15px; border-radius: 4px; position: relative; word-wrap: break-word; display: flex; justify-content: space-between; align-items: center; }";
    html += "ul#station-list li .info { flex-grow: 1; margin-right: 10px; }"; // Container for name/url
    html += "ul#station-list li .info b { color: #555; }";
    html += ".del-btn { color: #dc3545; background: none; border: none; font-size: 1.5em; font-weight: bold; cursor: pointer; padding: 0 5px; line-height: 1; }"; // Adjusted delete style
    html += ".del-btn:hover { color: #a71d2a; }";
    html += "hr { border: 0; border-top: 1px solid #eee; margin: 25px 0; }";
    html += ".form-section { background-color: #fff; padding: 20px; border-radius: 5px; margin-bottom: 20px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }";
    html += ".note { font-size: 0.9em; color: #666; margin-top: 5px; margin-bottom: 15px;}"; // Adjusted margin
    html += "</style>";
    html += "</head><body>";
    html += "<h1>ESP32 Radio Configuration</h1>";

    // --- WiFi Credentials Form ---
    html += "<div class='form-section'>";
    preferences.begin("wifi-creds", true); // Open NVS read-only
    String current_ssid = preferences.getString("ssid", "");
    preferences.end();
    html += "<h2>WiFi Credentials</h2>";
    html += "<form action='/savewifi' method='POST'>";
    html += "<label for='ssid-select'>Available Networks (Scan Results):</label>";
    html += "<select id='ssid-select' name='ssid-select' onchange='document.getElementById(\"ssid\").value=this.value'>"; // Add JS to copy selection to input field
    html += "<option value=''>-- Scan & Select Network --</option>";

    // --- Perform WiFi Scan ---
    Serial.println("Scanning WiFi networks...");
    // WiFi.scanNetworks will return the number of networks found.
    // It can take a few seconds. Ensure WiFi radio is enabled (implicitly done by softAP).
    int n = WiFi.scanNetworks();
    Serial.printf("Scan finished, %d networks found.\n", n);

    if (n > 0) {
        for (int i = 0; i < n; ++i) {
            String scanned_ssid = WiFi.SSID(i);
            String selected_attr = (scanned_ssid == current_ssid) ? " selected" : ""; // Pre-select if matches saved SSID
            // Display SSID, Signal Strength (RSSI), and Security Type
            String security_type;
            switch(WiFi.encryptionType(i)) {
                case WIFI_AUTH_OPEN:            security_type = "Open"; break;
                case WIFI_AUTH_WEP:             security_type = "WEP"; break; // Note: WEP is insecure
                case WIFI_AUTH_WPA_PSK:         security_type = "WPA-PSK"; break;
                case WIFI_AUTH_WPA2_PSK:        security_type = "WPA2-PSK"; break;
                case WIFI_AUTH_WPA_WPA2_PSK:    security_type = "WPA/WPA2-PSK"; break;
                case WIFI_AUTH_WPA2_ENTERPRISE: security_type = "WPA2-Ent"; break; // Identify Enterprise
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
    // Clean up scan results
    WiFi.scanDelete(); // Free memory used by scan results
    // ------------------------

    html += "</select><br>";

    // SSID Input Field (can be filled by dropdown or typed manually)
    html += "<label for='ssid'>Network Name (SSID):</label>";
    html += "<input type='text' id='ssid' name='ssid' value='" + html_escape(current_ssid) + "' placeholder='Select from list or type SSID' required><br>";

    // Password Input Field (Optional)
    html += "<label for='password'>Password:</label>";
    html += "<input type='password' id='password' name='password' placeholder='Leave blank for Open network'><br>"; // Removed 'required'
    html += "<div class='note'>Note: Only WPA/WPA2/WPA3-PSK (Password) networks are supported via this form. Leave password blank for Open networks.<br>Enterprise (Username/Password) networks require code changes using ESP-IDF functions.</div>";
    html += "<button type='submit'>Save WiFi & Reboot</button>";
    html += "</form>";
    html += "</div>"; // End form-section

    // --- Radio Stations Form ---
    html += "<div class='form-section'>";
    html += "<h2>Radio Stations</h2>";
    // Form submits all current stations (including hidden fields for existing ones)
    html += "<form id='stations-form' action='/savestations' method='POST'>";
    html += "<div>Current Stations: (<span id='station-count'>" + String(arrayStation.size()) + "</span>)</div>";
    html += "<ul id='station-list'>";
    // Load current stations again or use global vector if fresh (already loaded in this func)
    // loadConfiguration(); // Reload the list from SPIFFS - Already done if called by setup()
    for (size_t i = 0; i < arrayStation.size(); ++i) {
        html += "<li id='station-item-" + String(i) + "'>"; // Unique ID for each item
        html += "<div class='info'>"; // Div for text content
        html += "<b>Name:</b> " + html_escape(arrayStation[i]) + "<br>";
        html += "<b>URL:</b> " + html_escape(arrayURL[i]);
        html += "</div>";
        // Delete button calls JS function
        html += "<button type='button' class='del-btn' onclick='deleteStation(this)'>&times;</button>";
        // Hidden fields to include this station when form is submitted
        html += "<input type='hidden' name='name" + String(i) + "' value='" + html_escape(arrayStation[i]) + "'>";
        html += "<input type='hidden' name='url" + String(i) + "' value='" + html_escape(arrayURL[i]) + "'>";
        html += "</li>";
    }
    html += "</ul>";
    html += "<h3>Add New Station</h3>";
    html += "<label for='newName'>Name:</label>";
    html += "<input type='text' id='newName' placeholder='Station Name'><br>";
    html += "<label for='newURL'>URL (must start with http://):</label>";
    html += "<input type='url' id='newURL' placeholder='http://...' pattern='http://.*'>"; // Removed 'required' from URL for flexibility? Keep required.
    // "Add to List" button calls JS function, doesn't submit form
    html += "<button type='button' onclick='addStationToList()'>Add to List Below</button>";
    html += "<hr>";
    // "Save Station List" button submits the entire form
    html += "<button type='submit'>Save Station List & Reboot</button>";
    html += "</form>";
    html += "</div>"; // End form-section

    // --- JavaScript for managing the station list dynamically ---
    html += "<script>";
    // Counter for unique names for newly added stations (ensures submission works if user adds/deletes/adds)
    html += "let newStationCounter = " + String(arrayStation.size()) + ";";
    // Function to escape HTML special characters for safe insertion into values (client-side version)
    html += "function escapeHTML(str) { let temp = document.createElement('div'); temp.textContent = str; return temp.innerHTML; }";
    // Adds a new station visually to the list and includes hidden fields
    html += "function addStationToList() {";
    html += " var nameInput = document.getElementById('newName');";
    html += " var urlInput = document.getElementById('newURL');";
    html += " var name = nameInput.value.trim();";
    html += " var url = urlInput.value.trim();";
    // Basic validation
    html += " if (!name || !url || !url.startsWith('http://')) { alert('Please enter a valid Name and a URL starting with http://'); return; }";
    html += " var list = document.getElementById('station-list');";
    html += " var newItem = document.createElement('li');";
    html += " var currentItemIndex = newStationCounter++;"; // Use counter for unique names
    html += " newItem.id = 'station-item-' + currentItemIndex;"; // Assign unique ID
    // Create content with delete button and hidden fields
    html += " newItem.innerHTML = \"<div class='info'><b>Name:</b> \" + escapeHTML(name) + \"<br><b>URL:</b> \" + escapeHTML(url) + \"</div>\" + ";
    html += "                     \"<button type='button' class='del-btn' onclick='deleteStation(this)'>&times;</button>\" +";
    html += "                     \"<input type='hidden' name='name\" + currentItemIndex + \"' value='\" + escapeHTML(name) + \"'>\" +";
    html += "                     \"<input type='hidden' name='url\" + currentItemIndex + \"' value='\" + escapeHTML(url) + \"'>\";";
    html += " list.appendChild(newItem);"; // Add the new item to the list
    html += " nameInput.value = ''; urlInput.value = '';"; // Clear input fields
    html += " document.getElementById('station-count').innerText = list.children.length;"; // Update count
    html += "}";
    // Removes a station item visually from the list (backend handles removal on save)
    html += "function deleteStation(btn) {";
    html += " var listItem = btn.parentNode;"; // Get the parent <li> element
    html += " listItem.parentNode.removeChild(listItem);"; // Remove it from the <ul>
    html += " document.getElementById('station-count').innerText = document.getElementById('station-list').children.length;"; // Update count
    html += " // When the form is submitted, this item simply won't be included.";
    html += "}";
    html += "</script>";

    html += "</body></html>";
    server.send(200, "text/html", html); // Send the complete HTML page
}

// --- Other Web Server Handlers ---
// (handleSaveWifi, handleSaveStations, handleReboot, handleNotFound
//  remain unchanged from the previous "full code" example)
void handleSaveWifi() {
    Serial.println("Processing POST /savewifi");
    if (server.hasArg("ssid") && server.hasArg("password")) {
        String ssid = server.arg("ssid");
        String password = server.arg("password");
        Serial.println("Received SSID: " + ssid);

        preferences.begin("wifi-creds", false); // Open NVS read-write
        preferences.putString("ssid", ssid);
        preferences.putString("password", password);
        preferences.end(); // Close NVS (assume success in this core version)

        Serial.println("WiFi credentials saved (assuming success).");
        // Send a success page that informs the user and triggers reboot via JS
        String html = "<!DOCTYPE html><html><head><title>WiFi Saved</title></head><body>";
        html += "<h2>WiFi Settings Saved!</h2>";
        html += "<p>The radio will now reboot and attempt to connect using the new credentials.</p>";
        html += "<p>Rebooting in <span id='countdown'>3</span> seconds...</p>";
        html += "<button onclick=\"window.location.href='/reboot'\">Reboot Now</button>"; // Button calls reboot endpoint
        html += "<script>var count = 3; function countdown(){ count--; if(count>=0) document.getElementById('countdown').innerText=count; else window.location.href='/reboot'; } setInterval(countdown, 1000);</script>"; // Countdown triggers reboot
        html += "</body></html>";
        server.send(200, "text/html", html);
    } else {
        Serial.println("ERROR: Missing ssid or password in /savewifi request.");
        server.send(400, "text/plain", "Bad Request: Missing SSID or Password parameter.");
    }
}
void handleSaveStations() {
    Serial.println("Processing POST /savestations");
    arrayURL.clear(); arrayStation.clear(); // Clear vectors before rebuilding
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
void handleReboot() {
    Serial.println("Received /reboot request. Rebooting...");
    String html = "<!DOCTYPE html><html><head><title>Rebooting</title></head><body><h2>Rebooting...</h2>Please wait and reconnect to your normal WiFi network.</body></html>";
    server.send(200, "text/html", html);
    delay(500);
    ESP.restart();
}
void handleNotFound() {
    Serial.println("Serving 404 Not Found");
    server.send(404, "text/plain", "404: Not Found");
}


// =========================================================================
// CONFIGURATION LOADING/SAVING IMPLEMENTATION
// =========================================================================
// (loadConfiguration, saveStationsToSPIFFS, loadDefaultStations
//  remain unchanged from the previous "full code" example)
bool loadConfiguration() {
    Serial.println("Loading configuration...");
    if (!preferences.begin("wifi-creds", true)) { Serial.println("WARNING: NVS Namespace 'wifi-creds' not found or failed to open (read-only)."); }
    preferences.end();
    Serial.println("Attempting to load stations from " + String(STATIONS_PATH));
    bool loaded_from_file = false;
    if (SPIFFS.exists(STATIONS_PATH)) {
        File file = SPIFFS.open(STATIONS_PATH, "r");
        if (!file) { Serial.println("ERROR: Failed to open stations file for reading, though it exists!"); }
        else {
            DynamicJsonDocument doc(3072);
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
                        if (name.length() > 0 && url.length() > 0 && url.startsWith("http://")) {
                            arrayStation.push_back(name); arrayURL.push_back(url); valid_stations_in_file++;
                        } else { Serial.println("Skipping invalid station entry found in JSON file (name/URL invalid)."); }
                    } else { Serial.println("Skipping invalid station entry found in JSON file (missing name or url key)."); }
                }
                if (valid_stations_in_file > 0) { Serial.printf("Loaded %d stations successfully from SPIFFS.\n", valid_stations_in_file); loaded_from_file = true; }
                else { Serial.println("Stations file was valid JSON but contained no valid stations."); }
            }
        }
    } else { Serial.println("Stations file not found."); }
    if (!loaded_from_file) { loadDefaultStations(); }
    sflag = 0;
    if (arrayStation.empty()) { Serial.println("WARNING: Station list is empty after load/default attempt!"); station = "No Stations"; }
    else { if (sflag >= arrayStation.size()) sflag = 0; station = arrayStation[sflag]; }
    Serial.println("Configuration loading finished.");
    return true;
}
bool saveStationsToSPIFFS() {
    Serial.println("Saving stations to " + String(STATIONS_PATH));
    DynamicJsonDocument doc(3072);
    JsonArray stationsArray = doc.to<JsonArray>();
    if (arrayURL.size() != arrayStation.size()) { Serial.println("FATAL ERROR: URL and Station Name lists have different sizes! Cannot save."); return false; }
    for (size_t i = 0; i < arrayURL.size(); ++i) { JsonObject stationObj = stationsArray.createNestedObject(); stationObj["name"] = arrayStation[i]; stationObj["url"] = arrayURL[i]; }
    File file = SPIFFS.open(STATIONS_PATH, "w");
    if (!file) { Serial.println("ERROR: Failed to open stations file for writing"); return false; }
    size_t bytesWritten = serializeJson(doc, file); file.close();
    if (bytesWritten == 0 && arrayURL.size() > 0) { Serial.println("ERROR: Failed to write to stations file (serializeJson returned 0)."); return false; }
    Serial.printf("Stations saved successfully to SPIFFS (%d bytes written).\n", bytesWritten);
    return true;
}


// =========================================================================
// WIFI CONNECTION (Uses credentials stored in NVS, includes Failsafe Trigger)
// =========================================================================
// (Remains unchanged from the previous full code example)
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
        Serial.println("Hold BTNA (GPIO 0) long press OR fix WiFi to enter Config Mode.");
        drawStatus("No WiFi Cfg", TFT_RED);
        return; // Exit connection attempt
    }

    int retryCount = 0;
    const int maxRetries = 3; // Try up to 3 times before triggering config mode

    while (retryCount < maxRetries) {
        Serial.printf("Connecting to WiFi: %s (Attempt %d/%d)", sta_ssid.c_str(), retryCount + 1, maxRetries);
        drawStatus("WiFi Try " + String(retryCount + 1), TFT_ORANGE); // Update display

        WiFi.mode(WIFI_STA); // Set Station mode

        // *** Check if password is empty for Open Network ***
        if (sta_password.length() == 0) {
            Serial.println(" (Open Network - No Password)");
            WiFi.begin(sta_ssid.c_str()); // Call begin with only SSID
        } else {
            Serial.println(" (Password Protected)");
            WiFi.begin(sta_ssid.c_str(), sta_password.c_str()); // Call begin with SSID and Password
        }
        // *** End of modification ***

        unsigned long startAttemptTime = millis();
        // Wait for connection or timeout (e.g., 15 seconds per attempt)
        while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 15000) {
            Serial.print(".");
            // Allow button presses to be detected even while waiting for WiFi
            buttonA.loop(); buttonB.loop(); buttonC.loop(); buttonD.loop();
            delay(500);
        }

        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("\nWiFi connected successfully!");
            Serial.print("IP Address: "); Serial.println(WiFi.localIP());
            drawStatus("Connected", TFT_GREEN); // Update display status
            // Update IP on TFT display
            tft.setTextFont(1); tft.setTextColor(TFT_GREEN, TFT_BLACK);
            tft.fillRect(8, 211, 120, 10, TFT_BLACK); // Clear area
            tft.setCursor(8, 211, 1); tft.println(WiFi.localIP().toString());
            return; // <<< SUCCESS! Exit the function normally.
        } else {
            Serial.println("\nWiFi Connection Attempt failed.");
            WiFi.disconnect(true); // Ensure disconnected before retry
            delay(1000); // Wait a second before retrying
            retryCount++;
            drawStatus("WiFi Fail " + String(retryCount), TFT_RED);
        }
    } // End while retries

    // --- If we exit the loop, all retries have failed ---
    Serial.println("WiFi connection failed after multiple retries.");
    Serial.println(">>> Entering Configuration Mode on next boot via Failsafe <<<");
    drawStatus("WiFi Failed!", TFT_RED);
    tft.setFreeFont(&Orbitron_Medium_20); tft.setTextSize(1);
    tft.fillRect(10, 140, 115, 36, TFT_BLACK); // Clear title area
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString("WiFi Failed!", 10, 140, 2);
    tft.drawString("Enter Cfg Mode", 10, 158, 2); // Inform user on screen

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
// (Remain unchanged from the previous full code example)
void audio_info(const char *info){ Serial.print("Info:        "); Serial.println(info); if (strstr(info, "error") != NULL || strstr(info, "ERROR") != NULL) { /*drawStatus("ERROR", TFT_RED);*/ } else if (strstr(info, "stream ready") != NULL) { drawStatus("Playing", TFT_GREEN); } else if (strstr(info, "buffer filled") != NULL){ if(playflag) drawStatus("Playing", TFT_GREEN); } else if (strstr(info, "FLAC sync") != NULL) { drawStatus("FLAC...", TFT_CYAN); } else if (strstr(info, "AAC sync") != NULL) { drawStatus("AAC...", TFT_CYAN); } }
void audio_id3data(const char *info){ Serial.print("ID3:         "); Serial.println(info); }
void audio_eof_mp3(const char *info){ Serial.print("EOF MP3:     "); Serial.println(info); drawStatus("File End", TFT_YELLOW); }
void audio_showstation(const char *info){ Serial.print("Station:     "); Serial.println(info); if (strlen(info) > 0) { station = String(info); } else { if (!arrayStation.empty() && sflag < arrayStation.size()) { station = arrayStation[sflag]; } else { station = "Unknown"; } } drawStation(); }
void audio_showstreamtitle(const char *info){ Serial.print("Title:       "); Serial.println(info); drawTitle(String(info)); }
void audio_bitrate(const char *info){ Serial.print("Bitrate:     "); Serial.print(info); Serial.println("kbps"); }
void audio_commercial(const char *info){ Serial.print("Commercial:  "); Serial.println(info); drawStatus("Ad break", TFT_MAGENTA); drawTitle(""); }
void audio_icyurl(const char *info){ Serial.print("ICY URL:     "); Serial.println(info); }
void audio_lasthost(const char *info){ Serial.print("Last Host:   "); Serial.println(info); }
void audio_eof_stream(const char *info){ Serial.print("Stream End:  "); Serial.println(info); drawStatus("Stream End", TFT_YELLOW); drawTitle(""); }
