#include <WiFi.h>
#include "Audio.h"       // *** USE ESP32-audioI2S Library (e.g., Tag v2.0.3 for Core v2.0.14) ***
#include <TFT_eSPI.h>    // Graphics and font library
#include <SPI.h>         // Required by TFT_eSPI

// --- Include project-specific header files for graphics ---
// --- Ensure these files are in the same directory as your .ino file ---
#include "frame.h"       // Should define: frame[][], animation_width, animation_height, frames
#include "background.h"  // Should define background image array
#include "Orbitron_Medium_20.h" // Font

// --- TFT Objects and Settings ---
TFT_eSPI tft = TFT_eSPI();  // Invoke library, pins defined in User_Setup.h
#define TFT_GREY 0x5AEB     // New colour

// --- PWM Settings (for TFT Backlight) ---
const int pwmFreq = 5000;
const int pwmResolution = 8;
const int pwmLedChannelTFT = 0;

// --- Network Credentials ---
// *** REPLACE WITH YOUR ACTUAL WIFI CREDENTIALS ***
const char *SSID = "Megamind";      // Your SSID
const char *PASSWORD = ""; // Your Password
// --- End Network Credentials ---

// --- Stream URLs and Station Names (HTTP ONLY!) ---
char * arrayURL[] = {
  "http://icecast.unitedradio.it/Virgin.mp3",       // Index 0
  "http://streamer.radio.co/s06b196587/listen",     // Index 1
  "http://media-ice.musicradio.com:80/ClassicFMMP3",// Index 2
  "http://naxos.cdnstream.com:80/1255_128",         // Index 3
  "http://149.56.195.94:8015/steam",                // Index 4 - Check URL? "steam"?
  "http://ice2.somafm.com/christmas-128-mp3",     // Index 5
  "http://streamic.kiss-fm.nl/KissFM"              // Index 6
  // Add/remove valid HTTP streams as needed
};

String arrayStation[] = { // Ensure this matches the URLs above
  "Virgin Radio IT",    // Index 0
  "KPop Radio",         // Index 1
  "Classic FM",         // Index 2
  "Lite Favorites",     // Index 3
  "MAXXED Out",         // Index 4
  "SomaFM Xmas",        // Index 5
  "KissFM NL",          // Index 6
};

const int numCh = sizeof(arrayURL) / sizeof(char *); // Calculate number of channels
// --- End Stream List ---

// --- Pin Definitions ---
// I2S Pins (Connect to external DAC like UDA1334A)
// !! CRITICAL !! Verify these match YOUR hardware wiring !!
#define I2S_DOUT      25  // Data Out (DIN) - Connect to UDA1334A DIN
#define I2S_BCLK      27  // Bit Clock (BCLK) - Connect to UDA1334A BCLK
#define I2S_LRC       26  // Left/Right Clock (LRC / WS) - Connect to UDA1334A WSEL

// Potentiometer Pin (Volume Control)
// Use an available ADC pin (e.g., GPIO 32 on TTGO T-Display V1.1)
#define POT_PIN       32  // ADC Pin for potentiometer wiper (Check your board pinout!)

// Buttons and LED
const int LED = 13;  // Status LED Pin
const int BTNA = 0;  // GPIO Play/Pause - Strapping Pin Warning!
const int BTNB = 35; // GPIO Next Station (Volume now handled by pot) - Input Only Pin
const int BTNC = 12; // GPIO Backlight
const int BTND = 17; // GPIO Invert Display
// --- End Pin Definitions ---

// --- Audio Object (Using esp32-audioI2S library) ---
Audio audio;
// --- End Audio Objects ---

// --- Global State Variables ---
volatile bool playflag = false; // Playback state flag (volatile for safety)
int ledflag = 0;            // For blinking LED
int currentVolume = 10;     // Current volume (0-21), potentially set by pot at startup
int lastPotVolume = -1;     // Store last volume set by pot to reduce updates
unsigned long lastPotRead = 0; // Timer for reading potentiometer
const int potReadInterval = 100; // Read pot every 100ms

int sflag = 0;              // Current station index
char *URL = arrayURL[sflag]; // Pointer to current URL
String station = arrayStation[sflag]; // Current Station Name String

// Backlight control
int backlight[5] = {10, 30, 60, 120, 220}; // PWM duty cycles
byte b = 2; // Index for current backlight level (0-4)

// Button state flags (for simple edge detection)
int press1 = 0; // BTND state
int press2 = 0; // BTNC state
bool inv = 0; // Invert display state

// Animation
float n = 0; // Animation frame counter
uint32_t lastAnimUpdate = 0; // Timer for animation update
// --- End Global State Variables ---

// --- Forward Declarations ---
void initwifi();
// float volume_to_gain(int volume); // Not needed if displaying 0-21 directly
void drawVolume(); // Helper to draw volume/gain
void drawStation(); // Helper to draw station
void drawStatus(String status, uint16_t color); // Helper for status line
void drawTitle(String title); // Helper to draw title (with word wrap)
void handlePotentiometer(); // Function to read and process pot value
// --- End Forward Declarations ---

// =========================================================================
// SETUP FUNCTION
// =========================================================================
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n\n--- Booting ESP32-AudioI2S Radio ---");
  Serial.printf("Number of Stations: %d\n", numCh);
  Serial.flush();

  // --- GPIO Pin Setup ---
  Serial.println("setup: Configuring GPIO pins..."); Serial.flush();
  pinMode(LED, OUTPUT); digitalWrite(LED, HIGH); // LED Off
  pinMode(BTNA, INPUT); // Built-in pull-up often present on GPIO0
  pinMode(BTNB, INPUT); // GPIO35 is input only
  pinMode(BTNC, INPUT_PULLUP);
  pinMode(BTND, INPUT_PULLUP);
  // POT_PIN (e.g., 32) does not require pinMode for analogRead
  Serial.println("setup: GPIO pins configured."); Serial.flush();

  // --- TFT Display Initialization ---
  Serial.println("setup: Initializing TFT display..."); Serial.flush();
  tft.init();
  tft.setRotation(0); // Keep original rotation
  tft.setSwapBytes(true); // Keep original byte swap
  tft.setFreeFont(&Orbitron_Medium_20);
  tft.fillScreen(TFT_BLACK);
  tft.pushImage(0, 0, 135, 240, background); // Use background image
  Serial.println("setup: TFT Initialized."); Serial.flush();

  // --- TFT Backlight PWM Setup ---
  Serial.println("setup: Setting up TFT backlight PWM..."); Serial.flush();
  #ifdef TFT_BL // TFT_BL is usually defined in TFT_eSPI User_Setup
    Serial.printf("setup: TFT_BL defined as GPIO %d\n", TFT_BL);
    ledcSetup(pwmLedChannelTFT, pwmFreq, pwmResolution);
    ledcAttachPin(TFT_BL, pwmLedChannelTFT);
    ledcWrite(pwmLedChannelTFT, backlight[b]);
    Serial.println("setup: TFT backlight PWM configured.");
  #else
    Serial.println("setup: WARNING - TFT_BL pin not defined. Backlight control disabled.");
  #endif
  Serial.flush();

  // --- Initial Screen Drawing ---
  Serial.println("setup: Drawing initial screen elements..."); Serial.flush();
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(14, 20); tft.println("Radio");
  tft.drawLine(0, 28, 135, 28, TFT_GREY);
  for (int i = 0; i < b + 1; i++) { tft.fillRect(108 + (i * 4), 18, 2, 6, TFT_GREEN); } // Draw initial brightness indicator
  Serial.println("setup: Initial screen elements drawn."); Serial.flush();

  // --- WiFi Initialization ---
  Serial.println("setup: Initializing WiFi..."); Serial.flush();
  initwifi(); // Connect to WiFi
  Serial.println("setup: WiFi initialization routine finished."); Serial.flush();

  // --- Post-WiFi Screen Updates ---
  Serial.println("setup: Updating screen post-WiFi..."); Serial.flush();
  handlePotentiometer(); // Read initial pot position BEFORE setting audio volume/drawing display
  drawStatus("Ready", TFT_YELLOW);
  drawVolume(); // Draw volume based on initial pot reading
  drawStation();
  tft.setTextFont(1); // Switch to default font for IP
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.fillRect(8, 211, 120, 10, TFT_BLACK); // Clear area
  tft.setCursor(8, 211, 1); tft.println(WiFi.localIP());
  Serial.println("setup: Screen updated post-WiFi."); Serial.flush();

  // --- Audio Output Initialization (using esp32-audioI2S) ---
  Serial.println("setup: Initializing Audio Output (esp32-audioI2S)..."); Serial.flush();
  // ** VERIFY I2S PINS ARE CORRECT FOR YOUR EXTERNAL DAC **
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT); // Pins defined above
  // Set initial volume based on pot reading done by handlePotentiometer() above
  audio.setVolume(currentVolume);
  Serial.printf("setup: Audio Output Initialized. Pins BCLK=%d, LRC=%d, DOUT=%d\n", I2S_BCLK, I2S_LRC, I2S_DOUT);
  Serial.printf("setup: Initial Volume: %d\n", currentVolume);
  Serial.flush();

  Serial.println("--- Setup Complete ---"); Serial.flush();
}

// =========================================================================
// WIFI INITIALIZATION FUNCTION
// =========================================================================
void initwifi() {
  Serial.println("initwifi: Disconnecting existing WiFi..."); Serial.flush();
  WiFi.disconnect(); WiFi.softAPdisconnect(true); WiFi.mode(WIFI_STA);
  Serial.printf("initwifi: Attempting to connect to SSID: %s\n", SSID); Serial.flush();

  // Basic check if SSID looks like default placeholder
  if (strcmp(SSID, "YOUR_WIFI_SSID") == 0 || strlen(SSID) == 0 || strcmp(SSID, "xxxxx") == 0) {
     Serial.println("initwifi: FATAL ERROR - WiFi SSID not configured in code!");
     tft.setFreeFont(&Orbitron_Medium_20);
     tft.setTextColor(TFT_RED, TFT_BLACK);
     tft.drawString("WiFi SSID?", 10, 60, 2);
     tft.drawString("Set in code!", 10, 80, 2);
     while(1); // Halt
  }

  WiFi.begin(SSID, PASSWORD);
  int i = 0; Serial.print("initwifi: Connecting "); Serial.flush();
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print("."); Serial.flush(); delay(500); i++;
    if (i > 20) { // Timeout after ~10 seconds
       Serial.println("\ninitwifi: Connection Failed! Restarting..."); Serial.flush();
       tft.setFreeFont(&Orbitron_Medium_20);
       tft.setTextColor(TFT_RED, TFT_BLACK); tft.drawString("WiFi FAILED", 10, 60, 2);
       delay(2000); ESP.restart();
     }
  }
  Serial.println("\ninitwifi: WiFi Connected!");
  Serial.printf("initwifi: IP Address: %s\n", WiFi.localIP().toString().c_str()); Serial.flush();
}


// =========================================================================
// MAIN LOOP (Adapted for Potentiometer)
// =========================================================================
uint32_t lastStatusUpdate = 0; // Timer for LED blink/status
uint32_t lastRssiPrint = 0; // Timer for RSSI print

void loop() {
  uint32_t currentTime = millis(); // Get current time once per loop

  audio.loop(); // *** MUST BE CALLED REGULARLY for esp32-audioI2S ***

  //Serial.print("Raw ADC (GPIO 32): ");
  //Serial.println(analogRead(POT_PIN));
  // Read and handle potentiometer periodically
  if (currentTime - lastPotRead > potReadInterval) {
      handlePotentiometer();
      lastPotRead = currentTime;
  }

  playflag = audio.isRunning(); // Update playflag based on audio library state

  // --- Handle Animation (Reduced Frequency) ---
  if (playflag) {
    // Update animation roughly 15 times/sec (every 66ms)
    if (currentTime - lastAnimUpdate > 66) {
      lastAnimUpdate = currentTime;
      int frameIndex = int(n);
      if (frameIndex >= 0 && frameIndex < frames) {
         tft.pushImage(50, 126, animation_width, animation_height, frame[frameIndex]);
      } else { n = 0; } // Reset if index is bad
      n = n + 0.3; // Adjust step for visual speed
      if (int(n) >= frames) n = 0; // Loop animation
    }
  } else {
    // If stopped, show static frame (draw only if not already showing last frame)
    int lastFrameIndex = frames > 0 ? frames - 1 : 0;
     if (abs(n - lastFrameIndex) > 0.1) { // Check if not already on last frame
         if(lastFrameIndex >= 0 && lastFrameIndex < frames) { // Bounds check
            tft.pushImage(50, 126, animation_width, animation_height, frame[lastFrameIndex]);
         }
         n = lastFrameIndex; // Set counter to last frame
     }
  }

  // --- Status LED ---
  if (playflag) {
    if (currentTime - lastStatusUpdate > 1000) { // Update status LED every second
        lastStatusUpdate = currentTime;
        ledflag = !ledflag; digitalWrite(LED, ledflag ? LOW : HIGH);
    }
  } else {
     if (digitalRead(LED) == LOW) { digitalWrite(LED, HIGH); ledflag = 0; } // Ensure LED is off
  }


  // --- Handle Button Presses (with Debounce) ---
  static uint32_t lastButtonATime = 0;
  static uint32_t lastButtonBTime = 0;
  const uint32_t debounceDelay = 250; // ms debounce time

  // BTNA: Play / Stop Toggle
  if (digitalRead(BTNA) == LOW && currentTime - lastButtonATime > debounceDelay) {
    lastButtonATime = currentTime;
    Serial.println("loop: BTNA pressed."); Serial.flush();
    if (!playflag) {
      Serial.println("loop: BTNA -> Start Playing"); Serial.flush();
      drawStatus("Connecting", TFT_ORANGE);
      bool connected = audio.connecttohost(URL); // Start stream
      if (!connected) {
          drawStatus("Connect FAIL", TFT_RED);
      }
      // Status will be updated by callbacks on success or error
    } else {
      Serial.println("loop: BTNA -> Stop Playing"); Serial.flush();
      drawStatus("Stopped", TFT_RED);
      audio.stopSong(); // Stop stream
      drawTitle(""); // Clear title when stopping
    }
    // playflag = audio.isRunning(); // Update flag immediately after action - Already done at top of loop
  }

  // BTNB: Change Station ONLY (Volume is handled by potentiometer)
  if (digitalRead(BTNB) == LOW && currentTime - lastButtonBTime > debounceDelay) {
     lastButtonBTime = currentTime;
     Serial.println("loop: BTNB pressed."); Serial.flush();
     if (!playflag) { // Change station ONLY if stopped
        sflag = (sflag + 1) % numCh; // Cycle through available channels
        URL = arrayURL[sflag]; station = arrayStation[sflag];
        Serial.printf("loop: BTNB -> Next Station: %d - %s\n", sflag, station.c_str()); Serial.flush();
        drawStation(); // Update TFT
        drawStatus("Ready", TFT_YELLOW); // Reset status line
        drawTitle(""); // Clear title area
     } else {
         // If playing, BTNB currently does nothing for volume
         Serial.println("loop: BTNB -> No action (Volume controlled by pot)"); Serial.flush();
     }
  }

  // BTNC: Backlight Brightness Cycle (Momentary Press - using edge detection)
  if (digitalRead(BTNC) == LOW) {
    if (press2 == 0) { press2 = 1;
      Serial.println("loop: BTNC pressed -> Changing Backlight"); Serial.flush();
      tft.fillRect(108, 18, 25, 6, TFT_BLACK); b = (b + 1) % 5;
      for (int i = 0; i < b + 1; i++) { tft.fillRect(108 + (i * 4), 18, 2, 6, TFT_GREEN); }
      #ifdef TFT_BL
        ledcWrite(pwmLedChannelTFT, backlight[b]); Serial.printf("loop: Backlight level %d, PWM %d\n", b, backlight[b]); Serial.flush();
      #endif
    }
  } else { press2 = 0; } // Reset edge trigger

  // BTND: Invert Display Toggle (Momentary Press - using edge detection)
  if (digitalRead(BTND) == LOW) {
    if (press1 == 0) { press1 = 1;
      inv = !inv; Serial.printf("loop: BTND pressed -> Invert Display: %s\n", inv ? "ON" : "OFF"); Serial.flush();
      tft.invertDisplay(inv);
    }
  } else { press1 = 0; } // Reset edge trigger

  // --- Optional: Print RSSI periodically ---
  if (currentTime - lastRssiPrint > 5000) { // Print every 5 seconds
      lastRssiPrint = currentTime;
      if (WiFi.status() == WL_CONNECTED) {
         long rssi = WiFi.RSSI(); Serial.printf("Loop: Current WiFi RSSI: %ld dBm\n", rssi); Serial.flush();
      } else { Serial.println("Loop: WiFi not connected."); Serial.flush(); }
  }

  // --- Yield CPU ---
   yield(); // Allow other tasks to run, especially WiFi and background tasks

} // End of loop()

// =========================================================================
// HELPER FUNCTIONS
// =========================================================================

// Function to read potentiometer and update volume
void handlePotentiometer() {
    int potValue = analogRead(POT_PIN); // Reads 0-4095 correctly

    int newVolume;
    if (potValue < 50) { // Deadzone check
        newVolume = 0;
    } else {
        // Map the range 50-4095 to 0-21
        newVolume = map(potValue, 50, 4095, 0, 21);
    }
    newVolume = constrain(newVolume, 0, 21); // Ensures 0-21 range

    // Check if the volume actually changed
    if (newVolume != currentVolume) {
        currentVolume = newVolume;       // Update global state
        audio.setVolume(currentVolume);  // *** CALL TO LIBRARY ***
        drawVolume();                    // Update the display (user can't see well)
        // *** ADD THIS PRINTF BACK INSIDE THE IF ***
        // Serial.printf(">>> Volume SET command called with value: %d (ADC: %d)\n", currentVolume, potValue);
    }
}

// Draw Volume indicator on TFT (Shows 0-21)
void drawVolume() {
    tft.setFreeFont(&Orbitron_Medium_20); // Ensure correct font
    tft.setTextSize(1);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.fillRect(78, 66, 50, 16, TFT_BLACK); // Clear area (adjust width if needed for '21')
    tft.drawString(String(currentVolume), 78, 66, 2); // Draw volume 0-21, Font size 2
}

// Draw Station Name on TFT
void drawStation() {
    tft.setFreeFont(&Orbitron_Medium_20); // Ensure correct font
    tft.setTextSize(1);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    String displayStation = String(station); // Use current global station name
    if (displayStation.length() > 15) displayStation = displayStation.substring(0, 15); // Limit length
    tft.fillRect(10, 108, 120, 16, TFT_BLACK); // Clear area
    tft.drawString(displayStation, 12, 108, 2); // Font size 2
}

// Draw Status Message on TFT (Top Right)
void drawStatus(String status, uint16_t color) {
    tft.setFreeFont(&Orbitron_Medium_20); // Ensure correct font
    tft.setTextSize(1);
    tft.setTextColor(color, TFT_BLACK);
    tft.fillRect(78, 44, 55, 16, TFT_BLACK); // Clear status area
    if (status.length() > 10) status = status.substring(0, 10); // Limit length
    tft.drawString(status, 78, 44, 2); // Font size 2
}

// Draw Stream Title/Metadata on TFT (with basic word wrap)
void drawTitle(String title) {
    tft.setFreeFont(&Orbitron_Medium_20); // Ensure correct font
    tft.setTextSize(1);
    tft.setTextColor(TFT_ORANGE, TFT_BLACK); // Use Orange for title
    tft.fillRect(10, 140, 115, 36, TFT_BLACK); // Clear area (allow for two lines)

    title.trim(); // Remove leading/trailing whitespace
    if (title.length() == 0) return; // Don't draw if empty

    // Simple word wrap (approximate)
    int maxLen = 18; // Max characters per line (Adjust based on font/size and testing)
    if (title.length() <= maxLen) {
         tft.drawString(title, 12, 140, 2); // Font size 2
    } else {
        int splitPoint = -1;
        // Find last space before or at maxLen
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
             line2 = title.substring(splitPoint + 1); // Skip the space
        } else { // No space found, just break at maxLen
             line1 = title.substring(0, maxLen);
             line2 = title.substring(maxLen);
        }

        if (line2.length() > maxLen) line2 = line2.substring(0, maxLen-3) + "..."; // Truncate line 2 if still too long

        tft.drawString(line1, 12, 140, 2);      // Font size 2
        tft.drawString(line2, 12, 140 + 18, 2); // Draw second line below (adjust Y offset if needed)
    }
}

// =========================================================================
// AUDIO LIBRARY CALLBACK FUNCTIONS (esp32-audioI2S style)
// =========================================================================
// These are called by audio.loop() when events occur

void audio_info(const char *info){
    Serial.print("Info:        "); Serial.println(info);
    // Optionally display temporary info/errors on TFT status line
    if (strstr(info, "error") != NULL || strstr(info, "ERROR") != NULL) {
        drawStatus("ERROR", TFT_RED);
    } else if (strstr(info, "stream ready") != NULL) {
        drawStatus("Playing", TFT_GREEN);
    }
    // Add more conditions if needed
}

void audio_id3data(const char *info){  //id3 metadata
    Serial.print("ID3:         "); Serial.println(info);
    // You could parse specific ID3 tags here if needed (e.g., Artist)
}

void audio_eof_mp3(const char *info){  //end of file (local file, not stream)
    Serial.print("EOF:         "); Serial.println(info);
    drawStatus("File End", TFT_YELLOW);
    // playflag will update automatically in loop()
}

void audio_showstation(const char *info){
    Serial.print("Station:     "); Serial.println(info);
    if (strlen(info) > 0) {
        station = String(info); // Update global station name if provided by stream
        drawStation(); // Update TFT
    } else {
        // If stream doesn't provide station name, use the one from our array
        station = arrayStation[sflag];
        drawStation();
    }
}

void audio_showstreamtitle(const char *info){
    Serial.print("Title:       "); Serial.println(info);
    drawTitle(String(info)); // Display title on TFT
}

void audio_bitrate(const char *info){
    Serial.print("Bitrate:     "); Serial.println(info);
    // Could display bitrate on TFT if desired (e.g., below title)
}

void audio_commercial(const char *info){  //duration in sec
    Serial.print("Commercial:  "); Serial.println(info);
    drawStatus("Ad break", TFT_MAGENTA); // Example status update
    drawTitle(""); // Clear title during commercial
}

void audio_icyurl(const char *info){  //homepage
    Serial.print("ICY URL:     "); Serial.println(info);
}

void audio_lasthost(const char *info){  //stream URL played
    Serial.print("Last Host:   "); Serial.println(info);
}

void audio_eof_stream(const char *info){ // Stream ended OR connection lost
    Serial.print("Stream End:  "); Serial.println(info);
    drawStatus("Stream End", TFT_YELLOW);
    drawTitle(""); // Clear title
    // playflag will update automatically in loop()
}

// Add other available callbacks from esp32-audioI2S (v2.x compatible ones) if needed
// void audio_codec(const char *info){ ... }
// void audio_samplerate(const char *info){ ... }
// ... etc
