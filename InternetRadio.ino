// =========================================================================
// Includes
// =========================================================================
#include <WiFi.h>
#include "Audio.h"             // Use compatible version (For WiFi Radio)
#include <TFT_eSPI.h>          // Graphics and font library
#include <SPI.h>               // Required by TFT_eSPI
#include <WebServer.h>         // Standard ESP32 Web Server
#include <FS.h>                // Filesystem base
#include <SPIFFS.h>            // Filesystem implementation
#include <Preferences.h>       // For NVS storage
#include <ArduinoJson.h>       // For station list handling
#include <Button2.h>           // For button handling
#include <vector>              // For dynamic station lists
#include <stdlib.h>            // For random() function
#include <math.h>              // For visualizer calculations if needed
#include "BluetoothA2DPSink.h" // *** For Bluetooth Audio ***
#include <string.h>            // For memcpy if needed
#include <driver/i2s.h>        // *** Potentially needed for i2s_pin_config_t ***

// =========================================================================
// Definitions and Constants
// =========================================================================

// --- TFT Objects and Settings ---
TFT_eSPI tft = TFT_eSPI();
#define TFT_GREY 0x5AEB
#define TFT_ORANGE 0xFDA0
#define TFT_BLUE_DARK 0x001F // Darker blue

// --- Screen Dimensions ---
#define SCREEN_WIDTH tft.width()
#define SCREEN_HEIGHT tft.height()

// --- PWM Settings ---
const int pwmFreq = 5000;
const int pwmResolution = 8;
const int pwmLedChannelTFT = 0;

// --- Pin Definitions ---
#define I2S_DOUT 25
#define I2S_BCLK 27
#define I2S_LRC 26
#define POT_PIN 32
const int LED = 13;
// --- Button Pins ---
const int BTN_BACKLIGHT = 0;  // TTGO Bottom - Backlight Tap / BT Toggle Long Press
const int BTN_CONFIG = 35;    // TTGO Top - Config Mode Long Press / Visualizer Cycle Tap
const int BTN_PLAY_STOP = 12; // External C - Play/Stop Tap (WiFi Radio Only)
const int BTN_NEXT = 17;      // External D - Next Station Tap (WiFi Radio Only)

// --- Web Configuration Portal ---
const char *AP_SSID = "RadioConfigPortal";
const char *AP_PASSWORD = "password123";
const char *STATIONS_PATH = "/stations.json";

// --- Button Timing ---
#define WIFI_CONFIG_LONG_PRESS_MS 3000
#define BT_TOGGLE_LONG_PRESS_MS 1000

// --- Small Animation Indicator ---
const int INDICATOR_X = SCREEN_WIDTH - 10;
const int INDICATOR_Y = SCREEN_HEIGHT - 10;
const int INDICATOR_SIZE = 7;
const unsigned long INDICATOR_PULSE_INTERVAL = 100;
const int MAX_INDICATOR_PULSES = 3;

// --- Visualizer ---
const unsigned long VISUALIZER_UPDATE_INTERVAL = 30;
#define NUM_RETRO_COLORS 8
#define NUM_BARS 16 // Lower this (e.g., 12 or 8) if bar viz still lags
#define MAX_PARTICLES 50

// --- Bluetooth ---
const char *BT_DEVICE_NAME = "JC RADIO"; // Changed Name

// =========================================================================
// Global Objects
// =========================================================================
Audio audio; // For WiFi Radio
WebServer server(80);
Preferences preferences;
Button2 buttonBacklight;
Button2 buttonConfig;
Button2 buttonPlayStop;
Button2 buttonNext;
BluetoothA2DPSink a2dp_sink; // For Bluetooth Audio

// =========================================================================
// Global Variables
// =========================================================================

// --- Audio Mode ---
enum AudioMode
{
    MODE_WIFI_RADIO,
    MODE_BLUETOOTH_SINK
};
AudioMode currentAudioMode = MODE_WIFI_RADIO;
bool isSwitchingMode = false;

// --- Dynamic Station Data (WiFi Radio) ---
std::vector<String> arrayURL;
std::vector<String> arrayStation;
String currentStationName = "No Stations";
String currentStreamTitle = ""; // Also used for BT metadata
String currentStatus = "Booting";
String currentBitrate = "";
int sflag = 0;

// --- Playback & Control State ---
volatile bool playflag = false; // WiFi Radio playback flag
int currentVolume = 10;         // Represents 0-21 for Radio, mapped to 0-127 for BT
unsigned long lastPotRead = 0;
const int potReadInterval = 100;
int lastStablePotADC = -1;           // Store the ADC value that last caused a volume change
const int ADC_CHANGE_THRESHOLD = 40; // Required ADC change to trigger volume update (TUNE THIS VALUE!)
bool ledState = false;

// --- Display & UI State ---
int backlightLevels[5] = {10, 30, 60, 120, 220};
byte currentBacklightIndex = 2;
uint16_t statusColor = TFT_YELLOW;

// --- Small Animation Indicator State (WiFi Radio) ---
unsigned long lastIndicatorUpdateTime = 0;
int indicatorPulseCount = 0;
// *** ADDED STATE_BUFFERING_WARN ***
enum IndicatorState
{
    STATE_STOPPED_OK,
    STATE_PLAYING,
    STATE_WIFI_PROBLEM,
    STATE_BUFFERING_WARN
};
IndicatorState lastIndicatorState = STATE_STOPPED_OK;
bool needsStaticIndicatorDraw = true;
bool isBufferingWarningActive = false; // *** ADDED Global flag for buffering warning ***

// --- Visualizer State ---
bool visualizerModeActive = false;
bool showingAnimation = false;
int currentAnimationIndex = -1;
unsigned long lastVisualizerUpdateTime = 0;
int audioLevel = 0;
uint16_t retroColors[NUM_RETRO_COLORS];
struct Particle
{
    float x, y;
    float vx, vy;
    uint16_t color;
    bool active;
};
Particle particles[MAX_PARTICLES];
int prevBarHeights[NUM_BARS] = {0}; // For optimized bar drawing

// --- Bluetooth State ---
String bt_status = "Off";
String bt_peer_name = "";
volatile int btAudioLevel = 0;

// =========================================================================
// Function Prototypes
// =========================================================================
// Setup
void setupRadioMode();
void drawScreenLayout();
void initializeRetroColors();
// Config Portal
void startConfigurationPortal();
void handleRoot();
void handleSaveWifi();
void handleSaveStations();
void handleReboot();
void handleNotFound();
// Config Storage
bool loadConfiguration();
bool saveStationsToSPIFFS();
void loadDefaultStations();
void connectToWiFi();
// Radio / BT Helpers
String html_escape(String str);
void updateDisplay();
void clearTextArea(int x, int y, int w, int h);
void handlePotentiometer();
void updateSmallIndicator();
// Mode Switching
void startBluetoothMode();
void startWiFiRadioMode();
// Visualizer
void updateAudioLevel();
void runVisualizer();
void drawAnimationBars(int level);
void drawAnimationCircles(int level);
void drawAnimationParticles(int level);
void initializeParticles();
uint16_t getRetroColor(int index);
// Button Handlers
void handlePlayStopTap(Button2 &btn);
void enterConfigModeLongClick(Button2 &btn);
void handleNextStationTap(Button2 &btn);
void handleBacklightTap(Button2 &btn);
void handleVisualizerCycle(Button2 &btn);
void handleToggleAudioMode(Button2 &btn);
// WiFi Radio Audio Callbacks
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
// Bluetooth A2DP Callbacks
void avrc_metadata_callback(uint8_t data_type, const uint8_t *data);
void connection_state_changed(esp_a2d_connection_state_t state, void *ptr);
void audio_state_changed(esp_a2d_audio_state_t state, void *ptr);
void audio_data_callback(const uint8_t *data, uint32_t len);

// =========================================================================
// SETUP FUNCTION
// =========================================================================
void setup()
{
    Serial.begin(115200);
    randomSeed(analogRead(A0));
    delay(200);
    Serial.println("\n\n--- Booting ESP32 Multi-Mode Radio ---");

    preferences.begin("config", false);
    bool forceConfig = preferences.getBool("enterConfig", false);
    if (forceConfig)
    {
        preferences.putBool("enterConfig", false);
        Serial.println(">>> Configuration Flag Found - Entering Config Mode <<<");
    }
    preferences.end();

    bool configMode = forceConfig;

    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    Serial.println("TFT Initialized.");

    if (configMode)
    {
        // --- Configuration Portal Mode ---
        Serial.println(">>> Starting Configuration Portal <<<");
        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.setTextSize(2);
        tft.setCursor(5, 5);
        tft.println("Config Mode");
        tft.setTextSize(1);
        int yPos = 30;
        tft.setCursor(5, yPos);
        tft.print("Connect WiFi to:");
        yPos += 12;
        tft.setCursor(15, yPos);
        tft.print(AP_SSID);
        yPos += 12;
        tft.setCursor(5, yPos);
        tft.print("Password:");
        yPos += 12;
        tft.setCursor(15, yPos);
        tft.print(AP_PASSWORD);
        yPos += 12;
        tft.setCursor(5, yPos);
        tft.print("Browser to:");
        yPos += 12;
        tft.setCursor(15, yPos);
        tft.print("192.168.4.1");
        yPos += 18;
        tft.setTextColor(TFT_ORANGE, TFT_BLACK);
        tft.setCursor(5, yPos);
        tft.print("NOTE: Page load includes");
        yPos += 12;
        tft.setCursor(15, yPos);
        tft.print("WiFi scan, please wait...");
        if (!SPIFFS.begin(true))
        {
            Serial.println("CRITICAL: SPIFFS Mount Failed in Config Mode!");
            tft.setTextColor(TFT_RED, TFT_BLACK);
            tft.drawString("FS Error!", SCREEN_WIDTH - 60, SCREEN_HEIGHT - 15, 2);
            while (1)
                delay(1000);
        }
        loadConfiguration();
        startConfigurationPortal();
    }
    else
    {
        // --- Normal Radio / BT Mode ---
        Serial.println(">>> Starting Normal Mode <<<");
        if (!SPIFFS.begin(true))
        {
            Serial.println("CRITICAL: SPIFFS Mount Failed!");
            tft.setTextColor(TFT_RED);
            tft.drawString("FS Error!", 10, 10, 4);
            while (1)
                delay(1000);
        }
        initializeRetroColors();
        initializeParticles();
        loadConfiguration();
        setupRadioMode(); // Includes initial potentiometer read
        connectToWiFi();
        Serial.println("--- Setup Complete (Normal Mode) ---");
    }
}

// =========================================================================
// SETUP FUNCTION FOR NORMAL RADIO/BT MODE
// =========================================================================
void setupRadioMode()
{
    currentAudioMode = MODE_WIFI_RADIO;
    isSwitchingMode = false;
    bt_status = "Off";

#ifdef TFT_BL
    Serial.printf("setupRadioMode: TFT_BL defined as GPIO %d\n", TFT_BL);
    ledcSetup(pwmLedChannelTFT, pwmFreq, pwmResolution);
    ledcAttachPin(TFT_BL, pwmLedChannelTFT);
    ledcWrite(pwmLedChannelTFT, backlightLevels[currentBacklightIndex]);
    Serial.println("setupRadioMode: TFT backlight PWM configured.");
#else
    Serial.println("setupRadioMode: WARNING - TFT_BL pin not defined. Backlight control disabled.");
#endif

    drawScreenLayout();

    pinMode(LED, OUTPUT);
    digitalWrite(LED, HIGH);

    Serial.println("setupRadioMode: Initializing buttons...");

    // TTGO Bottom Button (GPIO 0) -> Backlight Tap / Mode Toggle Long Press
    buttonBacklight.begin(BTN_BACKLIGHT, INPUT_PULLUP);
    buttonBacklight.setTapHandler(handleBacklightTap);
    buttonBacklight.setLongClickHandler(handleToggleAudioMode);
    buttonBacklight.setLongClickTime(BT_TOGGLE_LONG_PRESS_MS);
    Serial.println(" - Backlight/Mode Button (TTGO Btn A / GPIO 0) Initialized [Tap/Long Press]");

    // TTGO Top Button (GPIO 35) -> Visualizer Cycle Tap / Enter Config Mode Long Press
    buttonConfig.begin(BTN_CONFIG, INPUT);
    buttonConfig.setTapHandler(handleVisualizerCycle);
    buttonConfig.setLongClickHandler(enterConfigModeLongClick);
    buttonConfig.setLongClickTime(WIFI_CONFIG_LONG_PRESS_MS);
    Serial.println(" - Viz Cycle/Config Button (TTGO Btn B / GPIO 35) Initialized [Tap/Long Press]");

    // External Button C (GPIO 12) -> Play/Stop Tap (Only for WiFi Radio)
    buttonPlayStop.begin(BTN_PLAY_STOP, INPUT_PULLUP);
    buttonPlayStop.setTapHandler(handlePlayStopTap);
    Serial.println(" - Play/Stop Button (External / GPIO 12) Initialized [Tap]");

    // External Button D (GPIO 17) -> Next Station Tap (Only for WiFi Radio)
    buttonNext.begin(BTN_NEXT, INPUT_PULLUP);
    buttonNext.setTapHandler(handleNextStationTap);
    Serial.println(" - Next Station Button (External / GPIO 17) Initialized [Tap]");

    Serial.println("setupRadioMode: Buttons initialized.");

    // *** Initial Potentiometer Read & Volume Set ***
    // Read the pot once to initialize lastStablePotADC and set initial volume
    lastStablePotADC = analogRead(POT_PIN); // Initialize with current reading
    handlePotentiometer();                  // Call once to set initial volume based on pot
    Serial.printf("setupRadioMode: Initial Pot Read ADC: %d, Initial Volume: %d\n", lastStablePotADC, currentVolume);

    currentStatus = "Radio Ready";
    statusColor = TFT_YELLOW;
    lastIndicatorState = (WiFi.status() == WL_CONNECTED) ? STATE_STOPPED_OK : STATE_WIFI_PROBLEM;
    needsStaticIndicatorDraw = true;
    isBufferingWarningActive = false; // Ensure warning is off at setup

    Serial.println("setupRadioMode: Initializing I2S for WiFi Radio Mode...");
    Serial.flush();
    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    // audio.setVolume is now handled by handlePotentiometer() called above

    // *** Configure I2S pins for A2DP Sink ***
    Serial.println("Setting A2DP Sink pin config...");
    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCLK,          // Pin 27
        .ws_io_num = I2S_LRC,            // Pin 26
        .data_out_num = I2S_DOUT,        // Pin 25
        .data_in_num = I2S_PIN_NO_CHANGE // Not used (-1)
    };
    a2dp_sink.set_pin_config(pin_config);
    Serial.println("A2DP Sink pin config set.");

    Serial.println("setupRadioMode: Registering Bluetooth callbacks...");
    a2dp_sink.set_avrc_metadata_callback(avrc_metadata_callback);
    a2dp_sink.set_on_connection_state_changed(connection_state_changed);
    a2dp_sink.set_on_audio_state_changed(audio_state_changed);
    a2dp_sink.set_stream_reader(audio_data_callback, true);
    Serial.println("setupRadioMode: Bluetooth callbacks registered.");

    updateDisplay(); // Display initial state
}

// =========================================================================
// INITIALIZE RETRO COLOR PALETTE
// =========================================================================
void initializeRetroColors()
{
    retroColors[0] = tft.color565(255, 0, 127); // Bright Pink
    retroColors[1] = tft.color565(255, 0, 0);   // Red
    retroColors[2] = tft.color565(255, 128, 0); // Orange
    retroColors[3] = tft.color565(255, 255, 0); // Yellow
    retroColors[4] = tft.color565(0, 255, 0);   // Lime Green
    retroColors[5] = tft.color565(0, 255, 255); // Cyan
    retroColors[6] = tft.color565(0, 127, 255); // Bright Blue
    retroColors[7] = tft.color565(127, 0, 255); // Purple/Violet
}

// =========================================================================
// GET A COLOR FROM THE PALETTE
// =========================================================================
uint16_t getRetroColor(int index)
{
    return retroColors[index % NUM_RETRO_COLORS];
}

// =========================================================================
// DRAW INITIAL SCREEN LAYOUT (Static Text) - Now includes Mode
// =========================================================================
void drawScreenLayout()
{
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(5, 5);
    tft.print("Mode:");
    tft.setCursor(150, 5);
    tft.print("Vol:");
    tft.setCursor(5, 25);
    tft.print("Station:"); // Label changes dynamically
    tft.setCursor(5, 45);
    tft.print("Title:"); // Label changes dynamically
    tft.setCursor(5, SCREEN_HEIGHT - 25);
    tft.print("Info:");
    tft.setCursor(5, SCREEN_HEIGHT - 12);
    tft.print("IP:"); // Label changes dynamically
    needsStaticIndicatorDraw = true;
}

// =========================================================================
// CLEAR TEXT AREA HELPER
// =========================================================================
void clearTextArea(int x, int y, int w, int h)
{
    tft.fillRect(x, y, w, h, TFT_BLACK);
}

// =========================================================================
// UPDATE DISPLAY (Draws Dynamic Text Information based on Mode)
// =========================================================================
void updateDisplay()
{
    if (showingAnimation || isSwitchingMode)
        return;

    clearTextArea(5 + 40, 5, 90, 10);                       // Mode value area
    clearTextArea(150 + 30, 5, 30, 10);                     // Volume value area
    clearTextArea(0, 25, SCREEN_WIDTH, 50);                 // Clear middle area (Station/Title/Device/Track)
    clearTextArea(0, SCREEN_HEIGHT - 25, SCREEN_WIDTH, 25); // Clear bottom area (Info/IP)

    tft.setTextColor(TFT_MAGENTA, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(5 + 40, 5);
    tft.print(currentAudioMode == MODE_WIFI_RADIO ? "WiFi Radio" : "Bluetooth");

    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(150 + 30, 5);
    // Display Volume as percentage 0-100 for consistency
    int displayVolumePercent;
    if (currentAudioMode == MODE_WIFI_RADIO)
    {
        displayVolumePercent = map(currentVolume, 0, 21, 0, 100);
    }
    else
    { // MODE_BLUETOOTH_SINK
        displayVolumePercent = map(currentVolume, 0, 127, 0, 100);
    }
    tft.print(displayVolumePercent); // Display as percentage

    tft.setTextSize(1);
    if (currentAudioMode == MODE_WIFI_RADIO)
    {
        // --- WiFi Radio Display ---
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setCursor(5, 25);
        tft.print("Station:");
        tft.setCursor(5, 45);
        tft.print("Title:");
        // Use "Status" label for the bottom-left text line
        tft.setCursor(5, SCREEN_HEIGHT - 25);
        tft.print("Status:");
        tft.setCursor(5, SCREEN_HEIGHT - 12);
        tft.print("IP:");

        // Display Status text on the "Status" line
        int statusX = 5 + 55;
        int statusY = SCREEN_HEIGHT - 25;
        clearTextArea(statusX, statusY, SCREEN_WIDTH - statusX - 5, 10);
        tft.setTextColor(statusColor, TFT_BLACK);
        tft.setCursor(statusX, statusY);
        tft.print(currentStatus.substring(0, 25));

        // Display Station Name
        int stationX = 5 + 55;
        int stationY = 25;
        clearTextArea(stationX, stationY, SCREEN_WIDTH - stationX - 5, 10);
        tft.setTextColor(TFT_CYAN, TFT_BLACK);
        tft.setCursor(stationX, stationY);
        tft.print(currentStationName.substring(0, 28));

        // Display Title (potentially multi-line)
        int titleX = 5 + 55;
        int titleY = 45;
        int titleWidth = SCREEN_WIDTH - titleX - 5;
        int titleHeight = 35;
        int charWidth = 6;
        int charsPerLine = titleWidth / charWidth;
        clearTextArea(titleX, titleY, titleWidth, titleHeight);
        tft.setTextColor(TFT_ORANGE, TFT_BLACK);
        tft.setCursor(titleX, titleY);
        String titleToDisplay = currentStreamTitle;
        titleToDisplay.trim(); // Trim initial title once
        if (titleToDisplay.length() > 0)
        {
            int currentLine = 0;
            int maxLines = 2;
            while (titleToDisplay.length() > 0 && currentLine < maxLines)
            {
                int breakPoint = -1;
                String line;
                if (titleToDisplay.length() <= charsPerLine)
                {
                    line = titleToDisplay;
                    titleToDisplay = ""; // No more title left
                }
                else
                {
                    for (int i = charsPerLine - 1; i >= 0; i--)
                    { // Find last space within limit
                        if (titleToDisplay.charAt(i) == ' ')
                        {
                            breakPoint = i;
                            break;
                        }
                    }
                    if (breakPoint != -1)
                    { // Found a space to break at
                        line = titleToDisplay.substring(0, breakPoint);
                        // *** CORRECTED LINES ***
                        titleToDisplay = titleToDisplay.substring(breakPoint + 1); // Get the rest
                        titleToDisplay.trim();                                     // Trim the remaining part in-place
                        // *** END CORRECTION ***
                    }
                    else
                    { // No space found, just break at char limit
                        line = titleToDisplay.substring(0, charsPerLine);
                        // *** CORRECTED LINES ***
                        titleToDisplay = titleToDisplay.substring(charsPerLine); // Get the rest
                        titleToDisplay.trim();                                   // Trim the remaining part in-place
                        // *** END CORRECTION ***
                    }
                }
                tft.print(line); // Print the current line
                currentLine++;
                if (titleToDisplay.length() > 0 && currentLine < maxLines)
                {                                                       // If more text and lines available
                    tft.setCursor(titleX, titleY + (currentLine * 12)); // Move cursor for next line
                }
            }
        }
        else
        {
            tft.print("---");
        } // Show placeholder if no title

        // Display IP Address
        int ipX = 5 + 25;
        int ipY = SCREEN_HEIGHT - 12;
        clearTextArea(ipX, ipY, 150, 10);
        tft.setTextColor(TFT_GREEN, TFT_BLACK);
        tft.setCursor(ipX, ipY);
        if (WiFi.status() == WL_CONNECTED)
        {
            tft.print(WiFi.localIP().toString());
        }
        else
        {
            tft.print("---.---.---.---");
        }

        needsStaticIndicatorDraw = true;
    }
    else
    {
        // --- Bluetooth Sink Display ---
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setCursor(5, 25);
        tft.print("Status:");
        tft.setCursor(5, 45);
        tft.print("Device:");
        tft.setCursor(5, 65);
        tft.print("Track:");
        clearTextArea(5, SCREEN_HEIGHT - 25, SCREEN_WIDTH - 10, 10);
        clearTextArea(5, SCREEN_HEIGHT - 12, SCREEN_WIDTH - 10, 10);

        int statusX = 5 + 55;
        int statusY = 25;
        clearTextArea(statusX, statusY, SCREEN_WIDTH - statusX - 5, 10);
        tft.setTextColor(TFT_CYAN, TFT_BLACK);
        tft.setCursor(statusX, statusY);
        tft.print(bt_status);

        int deviceX = 5 + 55;
        int deviceY = 45;
        clearTextArea(deviceX, deviceY, SCREEN_WIDTH - deviceX - 5, 10);
        tft.setTextColor(TFT_GREEN, TFT_BLACK);
        tft.setCursor(deviceX, deviceY);
        tft.print(bt_peer_name.isEmpty() ? "---" : bt_peer_name.substring(0, 28));

        int trackX = 5 + 55;
        int trackY = 65;
        clearTextArea(trackX, trackY, SCREEN_WIDTH - trackX - 5, 10);
        tft.setTextColor(TFT_ORANGE, TFT_BLACK);
        tft.setCursor(trackX, trackY);
        tft.print(currentStreamTitle.isEmpty() ? "---" : currentStreamTitle.substring(0, 28));

        needsStaticIndicatorDraw = false;
        tft.fillRect(INDICATOR_X, INDICATOR_Y, INDICATOR_SIZE, INDICATOR_SIZE, TFT_BLACK);
    }
}

// =========================================================================
// UPDATE SMALL INDICATOR ANIMATION (Bottom Right) - Only in WiFi Radio mode
// =========================================================================
void updateSmallIndicator()
{
    if (currentAudioMode != MODE_WIFI_RADIO || showingAnimation || isSwitchingMode)
    {
        if (!needsStaticIndicatorDraw)
        {
            tft.fillRect(INDICATOR_X, INDICATOR_Y, INDICATOR_SIZE, INDICATOR_SIZE, TFT_BLACK);
        }                                // Clear if it was drawn before
        needsStaticIndicatorDraw = true; // Mark that it might need redraw if we switch back
        return;
    }

    uint32_t currentTime = millis();
    IndicatorState currentState;

    // Determine current state (order matters: Problems > Warnings > Playing > Stopped)
    bool isWiFiProblem = (WiFi.status() != WL_CONNECTED || currentStatus == "Connecting" || currentStatus.startsWith("WiFi Try"));

    if (isWiFiProblem)
    {
        currentState = STATE_WIFI_PROBLEM;
    }
    else if (isBufferingWarningActive)
    { // Check buffering flag
        currentState = STATE_BUFFERING_WARN;
    }
    else if (playflag)
    {
        currentState = STATE_PLAYING;
    }
    else
    {
        currentState = STATE_STOPPED_OK;
    }

    // Reset pulse count if state changes
    if (currentState != lastIndicatorState)
    {
        indicatorPulseCount = 0;
        lastIndicatorState = currentState;
        needsStaticIndicatorDraw = true; // Force redraw on state change
    }

    // Decide if indicator should pulse (Only Green briefly, Blue continuously)
    // Yellow and Red will be solid.
    bool shouldPulse = (currentState == STATE_PLAYING && indicatorPulseCount < MAX_INDICATOR_PULSES) || (currentState == STATE_WIFI_PROBLEM);

    // Update TFT if static draw needed or if it's time for a pulse
    if (needsStaticIndicatorDraw || (shouldPulse && (currentTime - lastIndicatorUpdateTime > INDICATOR_PULSE_INTERVAL)))
    {
        uint16_t indicatorColor;
        switch (currentState)
        {
        case STATE_PLAYING:
            indicatorColor = TFT_GREEN;
            break;
        case STATE_WIFI_PROBLEM:
            indicatorColor = TFT_BLUE_DARK;
            break;
        case STATE_BUFFERING_WARN:
            indicatorColor = TFT_YELLOW;
            break; // Yellow for warning
        case STATE_STOPPED_OK:
        default:
            indicatorColor = TFT_RED;
            break;
        }

        tft.fillRect(INDICATOR_X, INDICATOR_Y, INDICATOR_SIZE, INDICATOR_SIZE, indicatorColor);

        // Only increment pulse count for the brief green pulses
        if (currentState == STATE_PLAYING && indicatorPulseCount < MAX_INDICATOR_PULSES)
        {
            indicatorPulseCount++;
        }

        lastIndicatorUpdateTime = currentTime;
        needsStaticIndicatorDraw = false; // Mark as drawn
    }
}

// =========================================================================
// VISUALIZER MODE FUNCTIONS
// =========================================================================
void updateAudioLevel()
{
    if (currentAudioMode == MODE_WIFI_RADIO)
    {
        // --- WiFi Radio Mode ---
        // Placeholder - Replace with actual analysis if possible
        if (playflag && audio.isRunning())
        {
            int baseLevel = 30;
            int peakAmount = 70;
            audioLevel = baseLevel + random(0, peakAmount + 1);
            if (random(100) < 5)
            {
                audioLevel = constrain(audioLevel + random(20, 40), 0, 100);
            }
        }
        else
        {
            audioLevel = random(0, 5);
        }
        audioLevel = constrain(audioLevel, 0, 100);
    }
    else if (currentAudioMode == MODE_BLUETOOTH_SINK)
    {
        // --- Bluetooth Sink Mode ---
        audioLevel = btAudioLevel; // Use level from BT callback
    }
    else
    {
        audioLevel = 0;
    }
}

void runVisualizer()
{
    uint32_t currentTime = millis();
    if (currentTime - lastVisualizerUpdateTime > VISUALIZER_UPDATE_INTERVAL)
    {
        lastVisualizerUpdateTime = currentTime;
        updateAudioLevel();

        switch (currentAnimationIndex)
        {
        case 0:
            drawAnimationBars(audioLevel);
            break;
        case 1:
            drawAnimationCircles(audioLevel);
            break;
        case 2:
            drawAnimationParticles(audioLevel);
            break;
        default:
            tft.fillScreen(TFT_BLACK);
            tft.setTextColor(TFT_RED);
            tft.drawString("Error: No Viz", 20, SCREEN_HEIGHT / 2 - 10);
            break;
        }
    }
}
// --- Animation 1: Vertical Bars (Optimized) ---
void drawAnimationBars(int level)
{
    int barWidth = SCREEN_WIDTH / NUM_BARS;
    int maxBarHeight = SCREEN_HEIGHT;

    for (int i = 0; i < NUM_BARS; i++)
    {
        int barLevel = constrain(level + random(-10, 11), 0, 100);
        int newHeight = map(barLevel, 0, 100, 0, maxBarHeight);
        newHeight = constrain(newHeight, 0, maxBarHeight);

        int x = i * barWidth;
        int currentPrevHeight = prevBarHeights[i];

        if (newHeight < currentPrevHeight)
        {
            int clearY = SCREEN_HEIGHT - currentPrevHeight;
            int clearHeight = currentPrevHeight - newHeight;
            tft.fillRect(x, clearY, barWidth - 1, clearHeight, TFT_BLACK);
        }

        // Draw the bar - simplified draw logic (redraw whole bar if height > 0)
        if (newHeight > 0)
        {
            int newY = SCREEN_HEIGHT - newHeight;
            uint16_t color = getRetroColor(i);
            tft.fillRect(x, newY, barWidth - 1, newHeight, color);
        }
        // Ensure area above bar is cleared if height reduced to 0 (covered by initial clear logic)

        prevBarHeights[i] = newHeight;
    }
}
// --- Animation 2: Expanding Circles ---
void drawAnimationCircles(int level)
{
    tft.fillScreen(TFT_BLACK);
    int centerX = SCREEN_WIDTH / 2;
    int centerY = SCREEN_HEIGHT / 2;
    int numCircles = map(level, 0, 100, 1, 8);
    int maxRadius = map(level, 0, 100, 5, min(centerX, centerY) - 5);

    for (int i = 0; i < numCircles; i++)
    {
        int radius = maxRadius * (float)(i + 1) / numCircles;
        int rOffset = map(level, 50, 100, 0, 5);
        radius += random(-rOffset, rOffset + 1);
        radius = max(1, radius);
        uint16_t color = getRetroColor(i + (millis() / 500));
        if (radius > 0)
        {
            tft.drawCircle(centerX, centerY, radius, color);
        }
    }
    int centerDotRadius = map(level, 0, 100, 1, 5);
    tft.fillCircle(centerX, centerY, centerDotRadius, getRetroColor(numCircles + (millis() / 300)));
}
// --- Animation 3: Particle Explosion ---
void initializeParticles()
{
    for (int i = 0; i < MAX_PARTICLES; i++)
    {
        particles[i].active = false;
    }
}
void drawAnimationParticles(int level)
{
    tft.fillScreen(TFT_BLACK);
    int centerX = SCREEN_WIDTH / 2;
    int centerY = SCREEN_HEIGHT / 2;

    if (level > 75 && random(100) < 40)
    {
        int numToEmit = map(level, 75, 100, 1, 5);
        for (int n = 0; n < numToEmit; n++)
        {
            for (int i = 0; i < MAX_PARTICLES; i++)
            {
                if (!particles[i].active)
                {
                    particles[i].active = true;
                    particles[i].x = centerX;
                    particles[i].y = centerY;
                    float angle = random(0, 360) * DEG_TO_RAD;
                    float speed = random(10, 30 + level / 2) / 10.0;
                    particles[i].vx = cos(angle) * speed;
                    particles[i].vy = sin(angle) * speed;
                    particles[i].color = getRetroColor(random(0, NUM_RETRO_COLORS));
                    break;
                }
            }
        }
    }

    for (int i = 0; i < MAX_PARTICLES; i++)
    {
        if (particles[i].active)
        {
            particles[i].x += particles[i].vx;
            particles[i].y += particles[i].vy;
            if (particles[i].x < 0 || particles[i].x >= SCREEN_WIDTH || particles[i].y < 0 || particles[i].y >= SCREEN_HEIGHT)
            {
                particles[i].active = false;
            }
            else
            {
                tft.fillRect((int)particles[i].x, (int)particles[i].y, 2, 2, particles[i].color);
            }
        }
    }
}

// =========================================================================
// MAIN LOOP - Handles Mode Switching
// =========================================================================
void loop()
{
    uint32_t currentTime = millis();
    static uint32_t lastHeapPrint = 0; // Timer for heap print

    // Debug: Print Free Heap periodically
    if (currentTime - lastHeapPrint > 5000)
    {
        Serial.printf("Free Heap: %u bytes | Mode: %s\n", ESP.getFreeHeap(), (currentAudioMode == MODE_WIFI_RADIO ? "WiFi" : "BT"));
        lastHeapPrint = currentTime;
    }

    // Handle buttons unless switching modes
    if (!isSwitchingMode)
    {
        buttonBacklight.loop();
        buttonConfig.loop();
        if (currentAudioMode == MODE_WIFI_RADIO)
        {
            buttonPlayStop.loop();
            buttonNext.loop();
        }
    }

    // Read potentiometer periodically (if not switching modes)
    if (!isSwitchingMode && (currentTime - lastPotRead > potReadInterval))
    {
        handlePotentiometer();
        lastPotRead = currentTime;
    }

    // --- Mode Specific Logic ---
    if (currentAudioMode == MODE_WIFI_RADIO)
    {
        // --- WiFi Radio Mode Loop ---
        if (WiFi.status() == WL_CONNECTED)
        {
            audio.loop();
        } // Only loop audio if connected

        if (visualizerModeActive && showingAnimation)
        {
            runVisualizer(); // Run visualizer if active
        }
        else
        {
            // --- Normal WiFi Radio Display Logic ---
            visualizerModeActive = false;
            showingAnimation = false; // Ensure flags are off

            // Check WiFi status if not already handled by connectToWiFi
            if (WiFi.status() != WL_CONNECTED)
            {
                if (currentStatus != "WiFi Lost" && currentStatus != "No WiFi Cfg" && !currentStatus.startsWith("WiFi Fail") && !currentStatus.startsWith("WiFi Try"))
                {
                    currentStatus = "WiFi Lost";
                    statusColor = TFT_RED;
                    updateDisplay();
                    Serial.println("Loop: WiFi connection lost.");
                }
            }

            // Update play status based on audio library state
            bool currentPlayState = audio.isRunning();
            if (currentPlayState != playflag)
            {
                playflag = currentPlayState;
                Serial.printf("Loop: Radio Play state changed. playflag = %s\n", playflag ? "true" : "false");
                if (!playflag)
                {
                    // If stopped, update status unless it's already an error/end state
                    if (currentStatus != "Stopped" && currentStatus != "File End" && currentStatus != "Stream End" && !currentStatus.startsWith("ERR") && !currentStatus.startsWith("WiFi") && currentStatus != "Connecting")
                    {
                        currentStatus = "Stopped";
                        statusColor = TFT_RED;
                    }
                    currentBitrate = "";
                    currentStreamTitle = "";          // Clear dynamic info
                    isBufferingWarningActive = false; // *** Clear warning when stopped ***
                }
                else
                {
                    // If started playing, update status if it was stopped/ready
                    if (currentStatus == "Stopped" || currentStatus == "Radio Ready" || currentStatus == "File End" || currentStatus == "Stream End")
                    {
                        currentStatus = "Connecting";
                        statusColor = TFT_ORANGE;
                    }
                    // Buffering flag is cleared by "stream ready" in audio_info
                }
                updateDisplay(); // Update display on state change
            }
            updateSmallIndicator(); // Update the small status indicator
        }

        // Blink LED while playing WiFi Radio
        static uint32_t lastLedToggle = 0;
        if (playflag)
        {
            if (currentTime - lastLedToggle > 500)
            { // Blink interval
                lastLedToggle = currentTime;
                ledState = !ledState;
                digitalWrite(LED, ledState ? LOW : HIGH); // LED ON when LOW
            }
        }
        else
        {
            if (ledState)
            { // Ensure LED is OFF if stopped
                digitalWrite(LED, HIGH);
                ledState = false;
            }
        }
    }
    else if (currentAudioMode == MODE_BLUETOOTH_SINK)
    {
        // --- Bluetooth Sink Mode Loop ---
        if (visualizerModeActive && showingAnimation)
        {
            runVisualizer(); // Run visualizer if active
        }
        else
        {
            visualizerModeActive = false;
            showingAnimation = false; // Ensure flags are off
        }

        // Keep LED solid ON when Bluetooth is connected
        if (bt_status == "Connected")
        {
            if (!ledState)
            {
                digitalWrite(LED, LOW);
                ledState = true;
            }
        }
        else
        {
            if (ledState)
            {
                digitalWrite(LED, HIGH);
                ledState = false;
            }
        }
    }

    yield(); // Allow background tasks
}

// =========================================================================
// Button2 Handler Functions
// =========================================================================
void handlePlayStopTap(Button2 &btn)
{
    if (currentAudioMode != MODE_WIFI_RADIO || isSwitchingMode)
        return;
    Serial.println("Tap: Play/Stop Button (Radio)");
    if (!playflag)
    { // If currently stopped
        if (WiFi.status() == WL_CONNECTED && !arrayURL.empty())
        {
            Serial.println(" -> Start Playing Radio");
            currentStatus = "Connecting";
            statusColor = TFT_ORANGE;
            isBufferingWarningActive = false; // *** Clear any old warning before connecting ***
            if (!showingAnimation)
                updateDisplay();
            if (sflag >= arrayURL.size())
                sflag = 0;
            char *currentURL = (char *)arrayURL[sflag].c_str();
            currentStationName = arrayStation[sflag];
            currentStreamTitle = "";
            currentBitrate = "";
            if (!audio.connecttohost(currentURL))
            {
                currentStatus = "Connect FAIL";
                statusColor = TFT_RED;
                if (!showingAnimation)
                    updateDisplay();
            }
            else
            {
                playflag = true;
            }
        }
        else if (arrayURL.empty())
        {
            currentStatus = "No Stations";
            statusColor = TFT_RED;
            if (!showingAnimation)
                updateDisplay();
        }
        else
        { // No WiFi
            currentStatus = "No WiFi";
            statusColor = TFT_RED;
            if (!showingAnimation)
                updateDisplay();
        }
    }
    else
    { // If currently playing
        Serial.println(" -> Stop Playing Radio");
        currentStatus = "Stopped";
        statusColor = TFT_RED;
        currentStreamTitle = "";
        currentBitrate = "";
        audio.stopSong();
        playflag = false;
        // Buffering flag cleared in loop when playflag becomes false
        if (!showingAnimation)
            updateDisplay();
    }
}

void enterConfigModeLongClick(Button2 &btn)
{
    if (isSwitchingMode)
        return;
    Serial.println("Long Click: Enter Config Mode Button (TTGO Btn B / GPIO 35)");
    if (currentAudioMode == MODE_WIFI_RADIO)
    {
        if (playflag)
            audio.stopSong();
        playflag = false;
        isBufferingWarningActive = false; // Clear flag
    }
    else
    {
        a2dp_sink.end();
        bt_status = "Off";
    }
    visualizerModeActive = false;
    showingAnimation = false;

    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_MAGENTA, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(10, 50);
    tft.print("Rebooting to Config Mode...");

    preferences.begin("config", false);
    preferences.putBool("enterConfig", true);
    preferences.end();

    Serial.println("Config flag set in NVS. Rebooting...");
    delay(2000);
    ESP.restart();
}

void handleNextStationTap(Button2 &btn)
{
    if (currentAudioMode != MODE_WIFI_RADIO || isSwitchingMode)
        return;
    Serial.println("Tap: Next Station Button (Radio)");
    if (!playflag && !arrayStation.empty())
    {
        sflag = (sflag + 1) % arrayStation.size();
        currentStationName = arrayStation[sflag];
        Serial.printf(" -> Next Station: %d - %s\n", sflag, currentStationName.c_str());
        currentStatus = "Radio Ready";
        statusColor = TFT_YELLOW;
        currentStreamTitle = "";
        currentBitrate = "";
        isBufferingWarningActive = false; // Clear warning when changing station (while stopped)
        if (!showingAnimation)
            updateDisplay();
    }
    else if (arrayStation.empty())
    {
        currentStatus = "No Stations";
        statusColor = TFT_RED;
        if (!showingAnimation)
            updateDisplay();
    }
    else if (playflag)
    {
        Serial.println(" -> No action (Stop playback first)");
    }
}

void handleBacklightTap(Button2 &btn)
{
    if (isSwitchingMode)
        return;
    Serial.println("Tap: Backlight Button (TTGO Btn A)");
    currentBacklightIndex = (currentBacklightIndex + 1) % 5;
#ifdef TFT_BL
    ledcWrite(pwmLedChannelTFT, backlightLevels[currentBacklightIndex]);
    Serial.printf(" -> Backlight level %d, PWM %d\n", currentBacklightIndex, backlightLevels[currentBacklightIndex]);
#else
    Serial.println(" -> Backlight control disabled (TFT_BL not defined).");
#endif
}

void handleVisualizerCycle(Button2 &btn)
{
    if (isSwitchingMode)
        return;
    Serial.println("Tap: Visualizer Cycle Button (TTGO Btn B)");
    if (!showingAnimation)
    {
        currentAnimationIndex = (currentAnimationIndex + 1) % 3;
        visualizerModeActive = true;
        showingAnimation = true;
        Serial.printf(" -> Activating Visualizer Animation %d\n", currentAnimationIndex);
        tft.fillScreen(TFT_BLACK);

        if (currentAnimationIndex == 0)
        {
            memset(prevBarHeights, 0, sizeof(prevBarHeights));
        }
        else if (currentAnimationIndex == 2)
        {
            initializeParticles();
        }
    }
    else
    {
        visualizerModeActive = false;
        showingAnimation = false;
        Serial.println(" -> Deactivating Visualizer, Returning to Menu");
        drawScreenLayout();
        updateDisplay(); // Redraw standard UI
    }
}

void handleToggleAudioMode(Button2 &btn)
{
    if (isSwitchingMode)
        return;
    isSwitchingMode = true;
    Serial.println("Long Click: Toggle Audio Mode Button (TTGO Btn A)");
    visualizerModeActive = false;
    showingAnimation = false;

    if (currentAudioMode == MODE_WIFI_RADIO)
    {
        Serial.println("Switching from WiFi Radio -> Bluetooth Sink");
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_CYAN);
        tft.drawString("Switching to BT...", 20, SCREEN_HEIGHT / 2 - 10);
        startBluetoothMode(); // Will also clear buffering flag
    }
    else
    {
        Serial.println("Switching from Bluetooth Sink -> WiFi Radio");
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_CYAN);
        tft.drawString("Switching to WiFi...", 20, SCREEN_HEIGHT / 2 - 10);
        startWiFiRadioMode(); // Will also clear buffering flag
    }

    delay(500);
    isSwitchingMode = false;
    drawScreenLayout();
    updateDisplay();
}

// =========================================================================
// MODE SWITCHING FUNCTIONS
// =========================================================================
void startBluetoothMode()
{
    if (playflag)
    {
        audio.stopSong();
        playflag = false;
    }
    isBufferingWarningActive = false; // *** Clear warning when switching away ***
    Serial.println("WiFi Radio stopped.");

    Serial.println("Disconnecting and turning off WiFi...");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("WiFi disconnected and turned off.");
    delay(100);

    Serial.println("Attempting to start Bluetooth...");
    Serial.printf("Attempting a2dp_sink.start(\"%s\")...\n", BT_DEVICE_NAME);
    a2dp_sink.set_auto_reconnect(true);

    a2dp_sink.start((char *)BT_DEVICE_NAME);

    currentAudioMode = MODE_BLUETOOTH_SINK;
    bt_status = "Initializing";
    bt_peer_name = "";
    currentStreamTitle = "";
    btAudioLevel = 0;
    a2dp_sink.set_volume(currentVolume); // Ensure BT volume matches current level

    Serial.println("Bluetooth Sink start() called. Mode set to BT.");
}

void startWiFiRadioMode()
{
    Serial.println("Stopping Bluetooth Sink...");
    a2dp_sink.end();
    bt_status = "Off";
    bt_peer_name = "";
    currentStreamTitle = "";
    btAudioLevel = 0;
    isBufferingWarningActive = false; // *** Clear warning when switching away ***
    Serial.println("Bluetooth Sink stopped.");
    delay(100);

    Serial.println("Setting WiFi mode to Station...");
    WiFi.mode(WIFI_STA);
    delay(100);

    Serial.println("Re-initializing I2S for WiFi Radio...");
    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);

    currentAudioMode = MODE_WIFI_RADIO;
    currentStatus = "Radio Ready";
    // Map currentVolume (0-127) back to Radio scale (0-21)
    int radioVol = map(currentVolume, 0, 127, 0, 21);
    currentVolume = constrain(radioVol, 0, 21);
    audio.setVolume(currentVolume);
    Serial.printf("WiFi Radio mode. Volume mapped back to: %d\n", currentVolume);
    Serial.println("I2S re-initialized for WiFi Radio.");

    connectToWiFi();
}

// =========================================================================
// HELPER FUNCTIONS (Potentiometer, HTML Escape, Config Loading/Saving)
// =========================================================================
void handlePotentiometer()
{
    int potValue = analogRead(POT_PIN);
    int newVolumeMapped;

    if (lastStablePotADC == -1 || abs(potValue - lastStablePotADC) > ADC_CHANGE_THRESHOLD)
    {
        if (currentAudioMode == MODE_WIFI_RADIO)
        {
            if (potValue < 50)
            {
                newVolumeMapped = 0;
            }
            else
            {
                newVolumeMapped = map(potValue, 50, 4095, 0, 21);
            }
            newVolumeMapped = constrain(newVolumeMapped, 0, 21);

            if (newVolumeMapped != currentVolume)
            {
                currentVolume = newVolumeMapped;
                audio.setVolume(currentVolume);
                lastStablePotADC = potValue;
                if (!showingAnimation)
                    updateDisplay();
                Serial.printf("Radio Volume set by pot: %d (ADC: %d)\n", currentVolume, potValue);
            }
        }
        else
        { // MODE_BLUETOOTH_SINK
            if (potValue < 50)
            {
                newVolumeMapped = 0;
            }
            else
            {
                newVolumeMapped = map(potValue, 50, 4095, 0, 127);
            }
            newVolumeMapped = constrain(newVolumeMapped, 0, 127);

            if (newVolumeMapped != currentVolume)
            {
                currentVolume = newVolumeMapped;
                a2dp_sink.set_volume(currentVolume);
                lastStablePotADC = potValue;
                if (!showingAnimation)
                    updateDisplay();
                Serial.printf("Bluetooth Volume set by pot: %d (ADC: %d)\n", currentVolume, potValue);
            }
        }
    }
    else if (lastStablePotADC == -1)
    {
        lastStablePotADC = potValue;
    }
}

String html_escape(String str)
{
    str.replace("&", "&amp;");
    str.replace("<", "&lt;");
    str.replace(">", "&gt;");
    str.replace("\"", "&quot;");
    str.replace("'", "&#039;");
    return str;
}

// *** loadDefaultStations function ***
void loadDefaultStations()
{
    Serial.println("Loading default station list.");
    arrayURL.clear();
    arrayStation.clear();
    // Add some default stations
    arrayStation.push_back("SomaFM GrooveSalad");
    arrayURL.push_back("http://ice1.somafm.com/groovesalad-128-mp3");
    arrayStation.push_back("SomaFM Def Con");
    arrayURL.push_back("http://ice1.somafm.com/defcon-128-mp3");
    arrayStation.push_back("Classic FM UK");
    arrayURL.push_back("http://media-ice.musicradio.com/ClassicFMMP3");
    arrayStation.push_back("NPO Radio 2 NL");
    arrayURL.push_back("http://icecast.omroep.nl/radio2-bb-mp3");
    arrayStation.push_back("Virgin Radio IT");
    arrayURL.push_back("http://icecast.unitedradio.it/Virgin.mp3"); // ADDED
    arrayStation.push_back("Naxos Classical");
    arrayURL.push_back("http://naxos.cdnstream.com:80/1255_128"); // ADDED
    // arrayStation.push_back("BBC Radio 1 UK"); arrayURL.push_back("http://stream.live.vc.bbcmedia.co.uk/bbc_radio_one"); // REMOVED
    sflag = 0; // Start with the first station
    Serial.printf("Loaded %d default stations.\n", arrayURL.size());
}

bool loadConfiguration()
{
    Serial.println("Loading configuration...");
    preferences.begin("wifi-creds", true);
    String saved_ssid = preferences.getString("ssid", "");
    preferences.end();
    if (saved_ssid.length() == 0)
    {
        Serial.println("WARNING: No WiFi SSID found in NVS.");
    }

    Serial.println("Attempting to load stations from " + String(STATIONS_PATH));
    bool loaded_from_file = false;
    if (SPIFFS.exists(STATIONS_PATH))
    {
        File file = SPIFFS.open(STATIONS_PATH, "r");
        if (!file)
        {
            Serial.println("ERROR: Failed to open stations file for reading!");
        }
        else
        {
            DynamicJsonDocument doc(3072);
            DeserializationError error = deserializeJson(doc, file);
            file.close();

            if (error)
            {
                Serial.print("ERROR: deserializeJson() failed: ");
                Serial.println(error.c_str());
            }
            else
            {
                arrayURL.clear();
                arrayStation.clear();
                JsonArray stationsArray = doc.as<JsonArray>();
                int valid_stations_in_file = 0;
                for (JsonObject stationObj : stationsArray)
                {
                    if (stationObj.containsKey("name") && stationObj.containsKey("url"))
                    {
                        String name = stationObj["name"].as<String>();
                        String url = stationObj["url"].as<String>();
                        if (name.length() > 0 && url.length() > 0 && url.startsWith("http://"))
                        {
                            arrayStation.push_back(name);
                            arrayURL.push_back(url);
                            valid_stations_in_file++;
                        }
                        else
                        {
                            Serial.println("Skipping invalid station entry in JSON.");
                        }
                    }
                    else
                    {
                        Serial.println("Skipping incomplete station entry in JSON.");
                    }
                }
                if (valid_stations_in_file > 0)
                {
                    Serial.printf("Loaded %d valid stations from SPIFFS.\n", valid_stations_in_file);
                    loaded_from_file = true;
                }
                else
                {
                    Serial.println("Stations file was valid JSON but contained no valid stations.");
                }
            }
        }
    }
    else
    {
        Serial.println("Stations file not found (" + String(STATIONS_PATH) + ").");
    }

    if (!loaded_from_file)
    {
        loadDefaultStations(); // *** Calls the updated function ***
    }

    sflag = 0;
    if (arrayStation.empty())
    {
        Serial.println("WARNING: Station list is empty after loading!");
        currentStationName = "No Stations";
    }
    else
    {
        if (sflag >= arrayStation.size())
            sflag = 0;
        currentStationName = arrayStation[sflag];
    }
    Serial.println("Configuration loading finished.");
    return true;
}

bool saveStationsToSPIFFS()
{
    Serial.println("Saving stations to " + String(STATIONS_PATH));
    DynamicJsonDocument doc(3072);
    JsonArray stationsArray = doc.to<JsonArray>();
    if (arrayURL.size() != arrayStation.size())
    {
        Serial.println("FATAL ERROR: Station Name/URL list size mismatch! Cannot save.");
        return false;
    }
    for (size_t i = 0; i < arrayURL.size(); ++i)
    {
        JsonObject stationObj = stationsArray.createNestedObject();
        stationObj["name"] = arrayStation[i];
        stationObj["url"] = arrayURL[i];
    }
    File file = SPIFFS.open(STATIONS_PATH, "w");
    if (!file)
    {
        Serial.println("ERROR: Failed to open stations file for writing");
        return false;
    }
    size_t bytesWritten = serializeJson(doc, file);
    file.close();
    if (bytesWritten == 0 && arrayURL.size() > 0)
    {
        Serial.println("ERROR: Failed to write stations data to file (wrote 0 bytes).");
        return false;
    }
    Serial.printf("Stations saved successfully to SPIFFS (%d bytes written).\n", bytesWritten);
    return true;
}

// =========================================================================
// CONFIGURATION PORTAL IMPLEMENTATION (AP Mode)
// =========================================================================
void startConfigurationPortal()
{
    Serial.println("Setting up Access Point...");
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    delay(100);
    IPAddress apIP = WiFi.softAPIP();
    Serial.print("AP IP address: ");
    Serial.println(apIP);
    server.on("/", HTTP_GET, handleRoot);
    server.on("/savewifi", HTTP_POST, handleSaveWifi);
    server.on("/savestations", HTTP_POST, handleSaveStations);
    server.on("/reboot", HTTP_GET, handleReboot);
    server.onNotFound(handleNotFound);
    server.begin();
    Serial.println("HTTP server started. Connect to WiFi '" + String(AP_SSID) + "' and browse to http://192.168.4.1");
    Serial.println("NOTE: Web page load includes WiFi scan and may take a few seconds.");
    while (true)
    {
        server.handleClient();
        delay(2);
    }
}

void handleRoot()
{
    Serial.println("Serving root page '/' - Starting WiFi Scan...");
    String html = "<!DOCTYPE html><html><head><title>Radio Config</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>body { font-family: sans-serif; margin: 15px; background-color: #f4f4f4; color: #333;} h1, h2, h3 { color: #0056b3; } label { display: block; margin-top: 12px; font-weight: bold; } input[type=text], input[type=password], input[type=url], select, button, textarea { margin-top: 6px; margin-bottom: 10px; padding: 10px; border: 1px solid #ccc; border-radius: 4px; width: 95%; max-width: 400px; box-sizing: border-box; } button { background-color: #007bff; color: white; cursor: pointer; border: none; padding: 10px 15px; border-radius: 4px; } button:hover { background-color: #0056b3; } button.delete { background-color: #dc3545; } button.delete:hover { background-color: #c82333; } ul#station-list { list-style: none; padding: 0; max-width: 600px; } ul#station-list li { background-color: #fff; border: 1px solid #ddd; margin-bottom: 8px; padding: 10px 15px; border-radius: 4px; position: relative; word-wrap: break-word; display: flex; justify-content: space-between; align-items: center; } ul#station-list li .info { flex-grow: 1; margin-right: 10px; } ul#station-list li .info b { color: #555; } .del-btn { color: #dc3545; background: none; border: none; font-size: 1.5em; font-weight: bold; cursor: pointer; padding: 0 5px; line-height: 1; } .del-btn:hover { color: #a71d2a; } hr { border: 0; border-top: 1px solid #eee; margin: 25px 0; } .form-section { background-color: #fff; padding: 20px; border-radius: 5px; margin-bottom: 20px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); } .note { font-size: 0.9em; color: #666; margin-top: 5px; margin-bottom: 15px;}</style>";
    html += "</head><body><h1>ESP32 Radio Configuration</h1>";
    html += "<div class='form-section'>";
    preferences.begin("wifi-creds", true);
    String current_ssid = preferences.getString("ssid", "");
    preferences.end();
    html += "<h2>WiFi Credentials</h2><form action='/savewifi' method='POST'><label for='ssid-select'>Available Networks (Scan Results):</label>";
    html += "<select id='ssid-select' name='ssid-select' onchange='document.getElementById(\"ssid\").value=this.value'><option value=''>-- Select Network --</option>";
    int n = WiFi.scanNetworks();
    Serial.printf("Scan finished inside handleRoot, %d networks found.\n", n);
    if (n > 0)
    {
        for (int i = 0; i < n; ++i)
        {
            String scanned_ssid = WiFi.SSID(i);
            String selected_attr = (scanned_ssid == current_ssid) ? " selected" : "";
            String security_type;
            switch (WiFi.encryptionType(i))
            {
            case WIFI_AUTH_OPEN:
                security_type = "Open";
                break;
            case WIFI_AUTH_WEP:
                security_type = "WEP";
                break;
            case WIFI_AUTH_WPA_PSK:
                security_type = "WPA-PSK";
                break;
            case WIFI_AUTH_WPA2_PSK:
                security_type = "WPA2-PSK";
                break;
            case WIFI_AUTH_WPA_WPA2_PSK:
                security_type = "WPA/WPA2-PSK";
                break;
            case WIFI_AUTH_WPA2_ENTERPRISE:
                security_type = "WPA2-Ent";
                break;
            case WIFI_AUTH_WPA3_PSK:
                security_type = "WPA3-PSK";
                break;
            case WIFI_AUTH_WPA2_WPA3_PSK:
                security_type = "WPA2/WPA3-PSK";
                break;
            default:
                security_type = "Unknown";
                break;
            }
            html += "<option value='" + html_escape(scanned_ssid) + "'" + selected_attr + ">";
            html += html_escape(scanned_ssid) + " (" + WiFi.RSSI(i) + " dBm, " + security_type + ")";
            html += "</option>";
        }
    }
    else
    {
        html += "<option value=''>No networks found</option>";
    }
    WiFi.scanDelete();
    html += "</select><br>";
    html += "<label for='ssid'>Network Name (SSID):</label><input type='text' id='ssid' name='ssid' value='" + html_escape(current_ssid) + "' placeholder='Select from list or type SSID' required><br>";
    html += "<label for='password'>Password:</label><input type='password' id='password' name='password' placeholder='Leave blank for Open network'><br>";
    html += "<div class='note'>Note: Only WPA/WPA2/WPA3-PSK supported. Leave password blank for Open networks. Enterprise networks are not supported.</div>";
    html += "<button type='submit'>Save WiFi & Reboot</button></form></div>";
    html += "<div class='form-section'><h2>Radio Stations</h2>";
    html += "<form id='stations-form' action='/savestations' method='POST'>";
    html += "<div>Current Stations: (<span id='station-count'>" + String(arrayStation.size()) + "</span>)</div>";
    html += "<ul id='station-list'>";
    for (size_t i = 0; i < arrayStation.size(); ++i)
    {
        html += "<li id='station-item-" + String(i) + "'>";
        html += "<div class='info'><b>Name:</b> " + html_escape(arrayStation[i]) + "<br><b>URL:</b> " + html_escape(arrayURL[i]) + "</div>";
        html += "<button type='button' class='del-btn' onclick='deleteStation(this)'>&times;</button>";
        html += "<input type='hidden' name='name" + String(i) + "' value='" + html_escape(arrayStation[i]) + "'>";
        html += "<input type='hidden' name='url" + String(i) + "' value='" + html_escape(arrayURL[i]) + "'>";
        html += "</li>";
    }
    html += "</ul>";
    html += "<h3>Add New Station</h3>";
    html += "<label for='newName'>Name:</label><input type='text' id='newName' placeholder='Station Name'><br>";
    html += "<label for='newURL'>URL (must start with http://):</label><input type='url' id='newURL' placeholder='http://...' pattern='http://.*'><br>";
    html += "<button type='button' onclick='addStationToList()'>Add to List Below</button>";
    html += "<hr>";
    html += "<button type='submit'>Save Station List & Reboot</button></form></div>";
    html += "<script>";
    html += "let newStationCounter = " + String(arrayStation.size()) + "; ";
    html += "function escapeHTML(str) { let temp = document.createElement('div'); temp.textContent = str; return temp.innerHTML; } ";
    html += "function addStationToList() { var nameInput = document.getElementById('newName'); var urlInput = document.getElementById('newURL'); var name = nameInput.value.trim(); var url = urlInput.value.trim(); if (!name || !url || !url.startsWith('http://')) { alert('Please enter a valid Name and a URL starting with http://'); return; } var list = document.getElementById('station-list'); var newItem = document.createElement('li'); var currentItemIndex = newStationCounter++; newItem.id = 'station-item-' + currentItemIndex; newItem.innerHTML = \"<div class='info'><b>Name:</b> \" + escapeHTML(name) + \"<br><b>URL:</b> \" + escapeHTML(url) + \"</div>\" + \"<button type='button' class='del-btn' onclick='deleteStation(this)'>&times;</button>\" + \"<input type='hidden' name='name\" + currentItemIndex + \"' value='\" + escapeHTML(name) + \"'>\" + \"<input type='hidden' name='url\" + currentItemIndex + \"' value='\" + escapeHTML(url) + \"'>\"; list.appendChild(newItem); nameInput.value = ''; urlInput.value = ''; document.getElementById('station-count').innerText = list.children.length; } ";
    html += "function deleteStation(btn) { var listItem = btn.parentNode; listItem.parentNode.removeChild(listItem); document.getElementById('station-count').innerText = document.getElementById('station-list').children.length; }";
    html += "</script>";
    html += "</body></html>";
    server.send(200, "text/html", html);
    Serial.println("Root page '/' served.");
}

void handleSaveWifi()
{
    Serial.println("Processing POST /savewifi");
    if (server.hasArg("ssid") && server.hasArg("password"))
    {
        String ssid = server.arg("ssid");
        String password = server.arg("password");
        if (ssid.length() == 0)
        {
            Serial.println("ERROR: Received empty SSID.");
            server.send(400, "text/plain", "Bad Request: SSID cannot be empty.");
            return;
        }
        Serial.println("Received SSID: " + ssid);
        Serial.println("Received Password: [REDACTED]");
        preferences.begin("wifi-creds", false);
        preferences.putString("ssid", ssid);
        preferences.putString("password", password);
        preferences.end();
        Serial.println("WiFi credentials saved to NVS.");
        String html = "<!DOCTYPE html><html><head><title>WiFi Saved</title><meta http-equiv='refresh' content='4; url=/reboot'></head><body>";
        html += "<h2>WiFi Settings Saved!</h2><p>Device will reboot in <span id='countdown'>3</span> seconds to apply settings.</p>";
        html += "<p><a href='/reboot'>Reboot Now</a></p>";
        html += "<script>var count = 3; function countdown(){ if(count>0) document.getElementById('countdown').innerText=count; count--; } setInterval(countdown, 1000);</script>";
        html += "</body></html>";
        server.send(200, "text/html", html);
    }
    else
    {
        Serial.println("ERROR: Missing ssid or password in POST request.");
        server.send(400, "text/plain", "Bad Request: Missing parameters.");
    }
}

void handleSaveStations()
{
    Serial.println("Processing POST /savestations");
    arrayURL.clear();
    arrayStation.clear();
    for (int i = 0;; i++)
    {
        String nameArgName = "name" + String(i);
        String urlArgName = "url" + String(i);
        if (server.hasArg(nameArgName) && server.hasArg(urlArgName))
        {
            String name = server.arg(nameArgName);
            String url = server.arg(urlArgName);
            if (name.length() > 0 && url.length() > 0 && url.startsWith("http://"))
            {
                arrayStation.push_back(name);
                arrayURL.push_back(url);
                Serial.println("  Adding: \"" + name + "\" -> " + url);
            }
            else
            {
                Serial.println("  Skipping invalid entry #" + String(i) + " (empty or bad URL format).");
            }
        }
        else
        {
            break;
        }
    }
    Serial.printf("Received %d valid stations from form.\n", arrayStation.size());
    if (saveStationsToSPIFFS())
    {
        Serial.println("Stations saved successfully to SPIFFS.");
        String html = "<!DOCTYPE html><html><head><title>Stations Saved</title><meta http-equiv='refresh' content='4; url=/reboot'></head><body>";
        html += "<h2>Station List Saved!</h2><p>Device will reboot in <span id='countdown'>3</span> seconds.</p>";
        html += "<p><a href='/reboot'>Reboot Now</a></p>";
        html += "<script>var count = 3; function countdown(){ if(count>0) document.getElementById('countdown').innerText=count; count--; } setInterval(countdown, 1000);</script>";
        html += "</body></html>";
        server.send(200, "text/html", html);
    }
    else
    {
        Serial.println("ERROR: Failed to save stations to SPIFFS!");
        server.send(500, "text/plain", "Internal Server Error: Could not save stations.");
    }
}

void handleReboot()
{
    Serial.println("Received /reboot request. Rebooting...");
    String html = "<!DOCTYPE html><html><head><title>Rebooting</title></head><body><h2>Rebooting device...</h2></body></html>";
    server.send(200, "text/html", html);
    delay(500);
    ESP.restart();
}

void handleNotFound()
{
    Serial.println("Serving 404 Not Found");
    server.send(404, "text/plain", "404: Not Found");
}

// =========================================================================
// WIFI CONNECTION
// =========================================================================
void connectToWiFi()
{
    preferences.begin("wifi-creds", true);
    String sta_ssid = preferences.getString("ssid", "");
    String sta_password = preferences.getString("password", "");
    preferences.end();
    if (sta_ssid.length() == 0)
    {
        Serial.println("WiFi credentials not set in NVS.");
        Serial.println("Use TTGO Top Button (GPIO 35) long press for Config Mode.");
        currentStatus = "No WiFi Cfg";
        statusColor = TFT_RED;
        if (!showingAnimation)
            updateDisplay();
        return;
    }
    int retryCount = 0;
    const int maxRetries = 3;
    const int connectTimeout_ms = 15000;
    WiFi.mode(WIFI_STA);
    while (retryCount < maxRetries)
    {
        Serial.printf("Connecting to WiFi: %s (Attempt %d/%d)\n", sta_ssid.c_str(), retryCount + 1, maxRetries);
        currentStatus = "WiFi Try " + String(retryCount + 1);
        statusColor = TFT_ORANGE;
        if (!showingAnimation)
            updateDisplay();
        if (sta_password.length() == 0)
        {
            WiFi.begin(sta_ssid.c_str());
        }
        else
        {
            WiFi.begin(sta_ssid.c_str(), sta_password.c_str());
        }
        unsigned long startAttemptTime = millis();
        bool attemptTimeout = false;
        while (WiFi.status() != WL_CONNECTED)
        {
            if (millis() - startAttemptTime > connectTimeout_ms)
            {
                Serial.println("\n -> Connection attempt timed out.");
                attemptTimeout = true;
                break;
            }
            Serial.print(".");
            if (!isSwitchingMode)
            {
                buttonBacklight.loop();
                buttonConfig.loop();
                buttonPlayStop.loop();
                buttonNext.loop();
            }
            if (!showingAnimation && currentAudioMode == MODE_WIFI_RADIO)
                updateSmallIndicator();
            delay(500);
        }
        if (WiFi.status() == WL_CONNECTED)
        {
            Serial.println("\nWiFi connected!");
            Serial.print("IP Address: ");
            Serial.println(WiFi.localIP());
            currentStatus = "Connected";
            statusColor = TFT_GREEN;
            if (!showingAnimation)
                updateDisplay();
            return;
        }
        else
        {
            Serial.println("\n -> WiFi Connection Failed.");
            WiFi.disconnect(true);
            delay(1000);
            retryCount++;
            currentStatus = "WiFi Fail " + String(retryCount);
            statusColor = TFT_RED;
            if (!showingAnimation)
                updateDisplay();
        }
    }
    Serial.println("-----------------------------------------");
    Serial.println("WiFi connection failed after all retries.");
    Serial.println("-----------------------------------------");
    currentStatus = "WiFi Failed!";
    statusColor = TFT_RED;
    if (!showingAnimation)
        updateDisplay();
}

// =========================================================================
// AUDIO LIBRARY CALLBACK FUNCTIONS (WiFi Radio)
// =========================================================================
void audio_info(const char *info)
{
    if (currentAudioMode != MODE_WIFI_RADIO)
        return;
    Serial.print("Info:        ");
    Serial.println(info);
    String infoStr = String(info);
    infoStr.toLowerCase();

    if (infoStr.indexOf("error") != -1 || infoStr.indexOf("failed") != -1)
    {
        if (currentStatus != "Connect FAIL" && currentStatus != "WiFi Failed!")
        {
            currentStatus = "Stream Err";
            statusColor = TFT_RED;
            isBufferingWarningActive = false; // Clear warning on definite error
            if (!showingAnimation)
                updateDisplay();
        }
    }
    else if (infoStr.indexOf("stream ready") != -1 || infoStr.indexOf("buffer filled") != -1)
    {
        if (currentStatus != "Playing")
        {
            currentStatus = "Playing";
            statusColor = TFT_GREEN;
            if (!showingAnimation)
                updateDisplay();
        }
        isBufferingWarningActive = false; // Clear warning when buffer is okay
        playflag = true;
    }
    else if (infoStr.indexOf("sync") != -1)
    {
        if (currentStatus != "Playing")
        {
            currentStatus = "Syncing...";
            statusColor = TFT_CYAN;
            if (!showingAnimation)
                updateDisplay();
        }
    }
    else if (infoStr.indexOf("connect") != -1 && currentStatus != "Playing")
    {
        currentStatus = "Connecting";
        statusColor = TFT_ORANGE;
        isBufferingWarningActive = false; // Clear warning when starting connection
        if (!showingAnimation)
            updateDisplay();
    }
    // Check for slow stream / buffering warnings
    else if (infoStr.indexOf("slow stream") != -1 || infoStr.indexOf("buffer low") != -1 || infoStr.indexOf("buffering") != -1)
    {
        Serial.println(">>> Buffering Warning Detected <<<");
        isBufferingWarningActive = true; // Set the warning flag
    }
}
void audio_id3data(const char *info)
{
    if (currentAudioMode != MODE_WIFI_RADIO)
        return;
    Serial.print("ID3:         ");
    Serial.println(info);
    if (currentStreamTitle.length() == 0 && strlen(info) > 3)
    {
        currentStreamTitle = String(info);
        if (!showingAnimation)
            updateDisplay();
    }
}
void audio_eof_mp3(const char *info)
{
    if (currentAudioMode != MODE_WIFI_RADIO)
        return;
    Serial.print("EOF MP3:     ");
    Serial.println(info);
    currentStatus = "File End";
    statusColor = TFT_YELLOW;
    playflag = false;
    currentStreamTitle = "";
    currentBitrate = "";
    isBufferingWarningActive = false; // Clear warning on end of file
    if (!showingAnimation)
        updateDisplay();
}
void audio_showstation(const char *info)
{
    if (currentAudioMode != MODE_WIFI_RADIO)
        return;
    Serial.print("Station:     ");
    Serial.println(info);
    if (strlen(info) > 0)
    {
        String icyName = String(info);
        if (icyName != currentStationName)
        {
            Serial.printf(" (ICY Name differs from list: %s)\n", currentStationName.c_str());
        }
    }
    else
    {
        if (!arrayStation.empty() && sflag < arrayStation.size())
        {
            currentStationName = arrayStation[sflag];
        }
        else
        {
            currentStationName = "Unknown";
        }
    }
    if (!showingAnimation)
        updateDisplay();
}
void audio_showstreamtitle(const char *info)
{
    if (currentAudioMode != MODE_WIFI_RADIO)
        return;
    Serial.print("Title:       ");
    Serial.println(info);
    String newTitle = String(info);
    newTitle.trim();
    if (newTitle != currentStreamTitle && newTitle.length() > 0)
    {
        currentStreamTitle = newTitle;
        if (!showingAnimation)
            updateDisplay();
    }
    else if (newTitle.length() == 0 && currentStreamTitle.length() > 0)
    {
        currentStreamTitle = "";
        if (!showingAnimation)
            updateDisplay();
    }
}
void audio_bitrate(const char *info)
{
    if (currentAudioMode != MODE_WIFI_RADIO)
        return;
    Serial.print("Bitrate:     ");
    Serial.print(info);
    Serial.println("kbps");
    String newBitrate = String(info) + "kbps";
    if (newBitrate != currentBitrate)
    {
        currentBitrate = newBitrate;
        // No longer displayed, so no need to update TFT
        // if (!showingAnimation) updateDisplay();
    }
}
void audio_commercial(const char *info)
{
    if (currentAudioMode != MODE_WIFI_RADIO)
        return;
    Serial.print("Commercial:  ");
    Serial.println(info);
}
void audio_icyurl(const char *info)
{
    if (currentAudioMode != MODE_WIFI_RADIO)
        return;
    Serial.print("ICY URL:     ");
    Serial.println(info);
}
void audio_lasthost(const char *info)
{
    if (currentAudioMode != MODE_WIFI_RADIO)
        return;
    Serial.print("Last Host:   ");
    Serial.println(info);
}
void audio_eof_stream(const char *info)
{
    if (currentAudioMode != MODE_WIFI_RADIO)
        return;
    Serial.print("Stream End:  ");
    Serial.println(info);
    currentStatus = "Stream End";
    statusColor = TFT_YELLOW;
    playflag = false;
    currentStreamTitle = "";
    currentBitrate = "";
    isBufferingWarningActive = false; // Clear warning on end of stream
    if (!showingAnimation)
        updateDisplay();
}

// =========================================================================
// BLUETOOTH A2DP CALLBACK FUNCTIONS
// =========================================================================
void connection_state_changed(esp_a2d_connection_state_t state, void *ptr)
{
    Serial.println("==> Entered connection_state_changed callback");
    Serial.printf("BT Connection State: %d\n", state);
    bool updateNeeded = false;
    switch (state)
    {
    case ESP_A2D_CONNECTION_STATE_DISCONNECTED:
        bt_status = "Disconnected";
        bt_peer_name = "";
        currentStreamTitle = "";
        btAudioLevel = 0;
        updateNeeded = true;
        Serial.println("A2DP Disconnected");
        break;
    case ESP_A2D_CONNECTION_STATE_CONNECTING:
        bt_status = "Connecting...";
        updateNeeded = true;
        Serial.println("A2DP Connecting...");
        break;
    case ESP_A2D_CONNECTION_STATE_CONNECTED:
        bt_status = "Connected";
        bt_peer_name = a2dp_sink.get_peer_name();
        if (bt_peer_name.isEmpty())
        {
            esp_bd_addr_t *p_last_addr = a2dp_sink.get_last_peer_address();
            if (p_last_addr != nullptr)
            {
                char addr_str[18];
                snprintf(addr_str, sizeof(addr_str), "%02X:%02X:%02X:%02X:%02X:%02X", (*p_last_addr)[0], (*p_last_addr)[1], (*p_last_addr)[2], (*p_last_addr)[3], (*p_last_addr)[4], (*p_last_addr)[5]);
                bt_peer_name = String(addr_str);
            }
            else
            {
                bt_peer_name = "Unknown Device";
                Serial.println("Could not retrieve peer address.");
            }
        }
        updateNeeded = true;
        Serial.printf("A2DP Connected to: %s\n", bt_peer_name.c_str());
        a2dp_sink.set_volume(currentVolume);
        Serial.printf(" -> Set BT volume to %d on connect\n", currentVolume);
        break;
    case ESP_A2D_CONNECTION_STATE_DISCONNECTING:
        bt_status = "Disconnecting";
        btAudioLevel = 0;
        updateNeeded = true;
        Serial.println("A2DP Disconnecting");
        break;
    default:
        bt_status = "Unknown State";
        updateNeeded = true;
        Serial.printf("A2DP Unknown Connection State: %d\n", state);
        break;
    }
    if (updateNeeded && !showingAnimation && !isSwitchingMode)
    {
        updateDisplay();
    }
}

void audio_state_changed(esp_a2d_audio_state_t state, void *ptr)
{
    Serial.println("==> Entered audio_state_changed callback");
    Serial.printf("BT Audio State: %d\n", state);
    bool updateNeeded = false;
    switch (state)
    {
    case ESP_A2D_AUDIO_STATE_STARTED:
        Serial.println("A2DP Audio Started");
        break;
    case ESP_A2D_AUDIO_STATE_STOPPED:
        Serial.println("A2DP Audio Stopped");
        btAudioLevel = 0;
        break; // Reset level viz
    case ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND:
        Serial.println("A2DP Audio Suspended");
        bt_status = "Suspended";
        btAudioLevel = 0;
        updateNeeded = true;
        break;
    default:
        Serial.printf("A2DP Unknown Audio State: %d\n", state);
        break;
    }
    if (updateNeeded && !showingAnimation && !isSwitchingMode)
    {
        updateDisplay();
    }
}

void avrc_metadata_callback(uint8_t data_type, const uint8_t *data)
{
    Serial.println("==> Entered avrc_metadata_callback callback");
    Serial.printf("AVRC Metadata: type %d, data: %s\n", data_type, (char *)data);
    if (data_type == ESP_AVRC_MD_ATTR_TITLE)
    {
        String newTitle = String((char *)data);
        newTitle.trim();
        if (newTitle != currentStreamTitle && newTitle.length() > 0)
        {
            currentStreamTitle = newTitle;
            Serial.printf(" -> Track Title Updated: %s\n", currentStreamTitle.c_str());
            if (!showingAnimation && !isSwitchingMode)
            {
                updateDisplay();
            }
        }
    }
}

#define RMS_WINDOW_SIZE 512
int16_t sample_buffer[RMS_WINDOW_SIZE];
uint32_t sample_count = 0;
void audio_data_callback(const uint8_t *data, uint32_t len)
{
    int samples_received = len / 4;
    for (int i = 0; i < samples_received; i++)
    {
        int16_t left_sample = (int16_t)((data[i * 4 + 1] << 8) | data[i * 4 + 0]);
        sample_buffer[sample_count % RMS_WINDOW_SIZE] = left_sample;
        sample_count++;
        if (sample_count % RMS_WINDOW_SIZE == 0)
        {
            double sum_sq = 0;
            for (int j = 0; j < RMS_WINDOW_SIZE; j++)
            {
                long long sample_val = (long long)sample_buffer[j];
                sum_sq += (double)(sample_val * sample_val);
            }
            double mean_sq = sum_sq / RMS_WINDOW_SIZE;
            double rms = sqrt(mean_sq);
            const double MAX_EXPECTED_RMS = 20000.0; // Tune this value
            int level = map(constrain(rms, 0.0, MAX_EXPECTED_RMS), 0.0, MAX_EXPECTED_RMS, 0, 100);
            btAudioLevel = level;
        }
    }
}
