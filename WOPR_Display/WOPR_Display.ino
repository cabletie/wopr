/*************************************************** 
  War Games - W.O.P.R. Missile Codes
  2020 Unexpected Maker
  Licensed under MIT Open Source

  This code is designed specifically to run on an ESP32. It uses features only
  available on the ESP32 like RMT and ledcSetup.

  W.O.P.R is available on tindie
  https://www.tindie.com/products/seonr/wopr-missile-launch-code-display-kit/
  
  Wired up for use with the TinyPICO ESP32 Development Board
  https://www.tinypico.com/shop/tinypico

  And the TinyPICO Audio Shield
  https://www.tinypico.com/shop/tinypico-mzffe-zatnr
 ****************************************************
 As modified by @CableTie
  - Added Setup menu to set:
    o DST,
    o TimeZone,
    o LED Brightness,
    o Default menu,
    o Delay before default menu selection is automatically entered
    o Clock digit separator
  - Added savings settings to flash using prefs from EEPROM Module
  - Message when connecting to WiFi
 ****************************************************/
// #define ARDUINO 100
// #include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>         // From Library Manager
#include <Adafruit_LEDBackpack.h> // From Library Manager
#include "OneButton.h"            // From Library Manager
#include <WiFi.h>
#include <time.h>
#include "secret.h"
#include "rmt.h"
#include "ESP32TimerInterrupt.h"
#include <Preferences.h>
#include <PubSubClient.h>

// Defines
#ifndef _BV
#define _BV(bit) (1 << (bit))
#endif

// Debug
#define DEBUG

#define BUT1 14   // front left button
#define BUT2 15   // front right button
#define RGBLED 27 // RGB LED Strip
#define DAC 25    // RGB LED Strip
// Seconds to milliseconds
#define S2MS(s) (1000 * s)

// NTP Wifi Time
#define TZ_DEFAULT 10
#define DST_DEFAULT 0
const char *ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3600 * TZ_DEFAULT;
const int daylightOffset_sec = 3600 * DST_DEFAULT;

// Program & Menu state
uint8_t currentState = 0; // 0 - menu, 1 - running, 2 - Setup
char const *const stateNames[] = {"MENU", "RUN", "SET"};
#define MAXSTATE 2
#define STATEMENU 0
#define STATERUN 1
#define STATESET 2

uint8_t currentMode = 0;    // 0 - movie simulation, 1 - random sequence, 2 - message, 3 - clock, 4 - Ticker, 5 - Setup
long menuTimeoutMillis = 0; // Tracks when the menu mode should be auto changed to clock mode
char const *const modeNames[] = {"MOVIE", "RANDOM", "MESSAGE", "CLOCK", "TICKER", "SETUP"};
#define MAXMODE 5
#define MODEMOVIE 0
#define MODERANDOM 1
#define MODEMESSAGE 2
#define MODECLOCK 3
#define MODETICKER 4
#define MODESETUP 5

/******************************************************
Settings
Setting types
Integer (min, max) - e.g. menuTimeout, defaultMode
Boolean - e.g. DST
Character (min, max) - e.g. Time Separator
*******************************************************/

// Timer to flash setting value part of display on LEDs
ESP32Timer ledFlashTimer(0);
bool ledFlash = true;

#define MENU_TIMEOUT_DEFAULT 10
#define TIMESEPARATOR '\x17'
#define MAXBRIGHTNESS 15

// EEPROM Module
Preferences prefs;
// How long before menu is exited to go to default mode
uint8_t menuTimeout; // Seconds
// Whether daylight savings is on or off
uint8_t daylightSavings;
// Timezone offset in integer hours
int8_t timeZone;
// Character to use between time digits
char timeSeparator; // Currently requires mods to the Adafruit font library
// Mode to exit to menu to
uint8_t defaultMode;
// Brightness 0-15
uint8_t ledBrightness; // LED brightness setting 0-15

// Settings names
char const *const settingNames[] = {"TIMEOUT", "DST", "DELIM", "MODE", "BRIGHT", "TZONE"};

// Setting enum thingy
#define MAXSETTING 5
#define SETTIMEOUT 0
#define SETDST 1
#define SETDELIM 2
#define SETDEFAULT 3
#define SETBRIGHT 4
#define SETTZONE 5

// Which setting we are setting
uint8_t currentSetting = 0;

/* Code cracking stuff
 * Though this works really well, there are probably much nicer and cleaner 
 * ways of doing this, so feel free to improve it and make a pull request!
 */
uint8_t counter = 0;
unsigned long nextTick = 0;
unsigned long nextSolve = 0;
uint16_t tickStep = 100;
uint16_t solveStep = 1000;
uint16_t solveStepMin = 4000;
uint16_t solveStepMax = 8000;
float solveStepMulti = 1;
uint8_t solveCount = 0;
uint8_t solveCountFinished = 10;
byte lastDefconLevel = 0;

// Audio stuff
bool beeping = false;
unsigned long nextBeep = 0;
uint8_t beepCount = 3;
int freq = 2000;
int channel = 0;
int resolution = 8;

// RGB stuff
unsigned long nextRGB = 0;
long nextPixelHue = 0;
uint32_t defcon_colors[] = {
    Color(255, 255, 255),
    Color(255, 0, 0),
    Color(255, 255, 0),
    Color(0, 255, 0),
    Color(0, 0, 255),
};

// Setup 3 AlphaNumeric displays (4 digits per display)
Adafruit_AlphaNum4 matrix[3] = {Adafruit_AlphaNum4(), Adafruit_AlphaNum4(), Adafruit_AlphaNum4()};

char displaybuffer[12] = {'-', '-', '-', ' ', '-', '-', '-', '-', ' ', '-', '-', '-'};

char missile_code[12] = {'A', 'B', 'C', 'D', 'E', 'F', '0', '1', '2', '3', '4', '5'};

char missile_code_movie[12] = {'C', 'P', 'E', ' ', '1', '7', '0', '4', ' ', 'T', 'K', 'S'};

char missile_code_message[12] = {'L', 'O', 'L', 'Z', ' ', 'F', 'O', 'R', ' ', 'Y', 'O', 'U'};

uint8_t code_solve_order_movie[10] = {7, 1, 4, 6, 11, 2, 5, 0, 10, 9}; // 4 P 1 0 S E 7 C K T

uint8_t code_solve_order_random[12] = {99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

// Storage and default for the current ticker message
char tickerMessage[256] = "WOPR by UnexpectedMaker";

// Initialise the buttons using OneButton library
OneButton Button1(BUT1, false);
OneButton Button2(BUT2, false);

// MQTT
// Add your MQTT Broker IP address, example:
//const char* mqtt_server = "192.168.1.144";
const char* mqtt_server = "192.168.86.87";

PubSubClient client(WiFiClass);
long lastMsg = 0;
char msg[256];
int value = 0;

// MQTT callback function
void mqttCallback(char* topic, byte* message, unsigned int length) {
  Serial.print("Message arrived on topic: ");
  Serial.print(topic);
  Serial.print(". Message: ");
  String messageTemp;
  
  for (int i = 0; i < length; i++) {
    Serial.print((char)message[i]);
    messageTemp += (char)message[i];
  }
  Serial.println();

  // Feel free to add more if statements to control more GPIOs with MQTT

  // If a message is received on the topic esp32/output, you check if the message is either "on" or "off". 
  // Changes the output state according to the message
  if (String(topic) == "wopr/ticker") {
    Serial.print("Changing ticker string to ");
    Serial.println(messageTemp);
    strncpy(tickerMessage,(char *)message,length);
  }
}

void mqttReconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if (client.connect("WOPRClient")) {
      Serial.println("connected");
      // Subscribe
      client.subscribe("wopr/ticker");
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

void setup()
{
  Serial.begin(115200);
  Serial.println("Wargames Missile Codes");

  Serial.print("Arduino v");
  Serial.println(ARDUINO);

  // Start EEPROM and load parameters
  prefs.begin("settings"); // use "settings" namespace

  // How long before menu is exited to go to default mode
  menuTimeout = prefs.getUChar("Timeout", MENU_TIMEOUT_DEFAULT); // Seconds
  Serial.printf("Loaded Timout: %d\n", menuTimeout);
  // Whether daylight savings is on or off
  daylightSavings = prefs.getUChar("DST", DST_DEFAULT);
  Serial.printf("Loaded DST: %s\n", daylightSavings ? "On" : "Off");
  // Timezone
  timeZone = prefs.getChar("TZone", TZ_DEFAULT);
  Serial.printf("Loaded TZone: %d\n", timeZone);
  // Character to use between time digits
  // Currently requires mods to the Adafruit font library
  timeSeparator = prefs.getUChar("Delim", TIMESEPARATOR);
  Serial.printf("Loaded Delimiter: 0x%02X\n", timeSeparator);
  // Mode to exit to menu to
  defaultMode = prefs.getUChar("Dflt", MODECLOCK);
  Serial.printf("Loaded Default Mode: %s\n", modeNames[defaultMode]);
  // LED brightness setting 0-15
  ledBrightness = prefs.getUChar("Bright", MAXBRIGHTNESS);
  Serial.printf("Loaded LED Brightness: %d\n", ledBrightness);
  setLedBrightness(ledBrightness);

  // Start LED flash timer
  // Not sure if it's OK to leave this running ...
  // Interval in microsecs
  if (ledFlashTimer.attachInterruptInterval(500 * 1000, toggleLedFlash))
    Serial.println("Starting ledFlashTimer OK, millis() = " + String(millis()));
  else
    Serial.println("Can't set ledFlashTimer. Select another freq. or timer");

  // Setup RMT RGB strip
  while (!RGB_Setup(RGBLED, 50))
  {
    // This is not good...
    delay(1000);
  }

  // Attatch button IO for OneButton
  Button1.attachClick(BUT1Press);
  Button2.attachLongPressStart(BUT2LongPress);
  Button2.attachClick(BUT2Press);

  // Initialise each of the HT16K33 LED Drivers
  matrix[0].begin(0x70); // pass in the address  0x70 == 1, 0x72 == 2, 0x74 == 3
  matrix[1].begin(0x72); // pass in the address  0x70 == 1, 0x72 == 2, 0x74 == 3
  matrix[2].begin(0x74); // pass in the address  0x70 == 1, 0x72 == 2, 0x74 == 3

  // Reset the code variables
  ResetCode();

  // Clear the display & RGB strip
  Clear();
  RGB_Clear();

  // Setup the Audio channel
  ledcSetup(channel, freq, resolution);
  ledcAttachPin(DAC, channel);

  /* Initialise WiFi to get the current time.
   * Once the time is obtained, WiFi is disconnected and the internal 
   * ESP32 RTC is used to keep the time
   * Make sure you have set your SSID and Password in secret.h
   */
  DisplayText("Trying WiFi");
  StartWifi();

  // Display MENU and Initialise timeout
  DisplayText("MENU");
  menuTimeoutMillis = millis() + S2MS(menuTimeout);

  // Configure MQTT
  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);

}

// Timer callback to flash settings number
void toggleLedFlash()
{
  ledFlash = !ledFlash;
}

String GetCurrentSettingValueString()
{
  switch (currentSetting)
  {
  case SETBRIGHT:
    return String(ledBrightness);
    break;
  case SETDEFAULT:
    return modeNames[defaultMode];
    break;
  case SETDELIM:
    return String(timeSeparator);
    break;
  case SETDST:
    return String(daylightSavings);
    break;
  case SETTIMEOUT:
    return String(menuTimeout);
    break;
  case SETTZONE:
    return String(timeZone);
    break;

  default:
    return "Error";
    break;
  }
}

void setLedBrightness(uint8_t b)
{
  for (int x = 0; x < 3; x++)
    matrix[x].setBrightness(ledBrightness);
}

void incrementSetting()
{
  switch (currentSetting)
  {
  case SETBRIGHT:
    ledBrightness++;
    if (ledBrightness > MAXBRIGHTNESS)
      ledBrightness = MAXBRIGHTNESS;
    setLedBrightness(ledBrightness);
    break;
  case SETDEFAULT:
    defaultMode++;
    if (defaultMode > MAXMODE)
      defaultMode = 0;
    break;
  case SETDELIM:
    timeSeparator++;
    if (timeSeparator > 127)
      timeSeparator = 0;
    break;
  case SETDST:
    daylightSavings = daylightSavings==1?0:1;
    break;
  case SETTIMEOUT:
    menuTimeout++;
    if (menuTimeout > 240)
      menuTimeout = 3;
    break;
  case SETTZONE:
    timeZone++;
    if (timeZone > 12)
      timeZone = -12;
    break;

  default:
    break;
  }
}

void decrementSetting()
{
  switch (currentSetting)
  {
  case SETBRIGHT:
    if (ledBrightness > 0)
      ledBrightness--;
    setLedBrightness(ledBrightness);
    break;
  case SETDEFAULT:
    if (defaultMode == 0)
      defaultMode = MAXMODE;
    else
      defaultMode--;
    break;
  case SETDELIM:
    if (timeSeparator == 0)
      timeSeparator = 127;
    else
      timeSeparator--;
    break;
  case SETDST:
    daylightSavings = daylightSavings==1?0:1;
    break;
  case SETTIMEOUT:
    if (menuTimeout == 3)
      menuTimeout = 240;
    else
      menuTimeout--;
    break;
  case SETTZONE:
    timeZone--;
    if (timeZone < (-12))
      timeZone = 12;
    break;

  default:
    break;
  }
}

void StartWifi()
{
  //connect to WiFi
  Serial.printf("Connecting to %s ", ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" CONNECTED");

  //init and get the time
  Serial.printf("calling configTime(%d,%d,%s)",timeZone * 3600 + daylightSavings * 3600, 0, ntpServer);
  configTime(timeZone * 3600 + daylightSavings * 3600, 0, ntpServer);

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
  {
    Serial.println("Failed to obtain time");
    return;
  }
  Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");

  //disconnect WiFi as it's no longer needed
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// Button press code her
long nextButtonPress = 0;

void BUT1Press()
{
  // String for sending to LED Display
  char ledString[13];

  // Only allow a button press every 10ms
  if (nextButtonPress < millis())
  {
    nextButtonPress = millis() + 10;

    // If we are not in the menu, cancel the current state and show the menu
    if (currentState == STATERUN)
    {
      if (currentMode == MODESETUP)
      {
        // Increment the setting value
        incrementSetting();
      }
      else
      {
        currentState = STATEMENU; // Jump out to menu mode
        currentMode = MODEMOVIE;  // Always start from the first mode item in the list

        DisplayText("MENU");
        menuTimeoutMillis = millis() + S2MS(menuTimeout);

        //Shutdown the audio if it's beeping
        ledcWriteTone(channel, 0);
        beeping = false;
      }
    }
    else if (currentState == STATESET)
    {
      currentSetting++;
      if (currentSetting > MAXSETTING)
        currentSetting = 0;
      Serial.printf("SET %s (%s)\n", String(settingNames[currentSetting]), GetCurrentSettingValueString());
      sprintf(ledString, "%s %s", settingNames[currentSetting], GetCurrentSettingValueString());
      Serial.print("LED String: ");
      Serial.println(ledString);
      DisplayText(ledString);
    }
    else
    {
      // Top level menu
      // Reset menuTimeout watchdog
      menuTimeoutMillis = millis() + S2MS(menuTimeout);

      // Update the current program state and display it on the menu
      if (currentMode == MAXMODE)
        currentMode = 0;
      else
        currentMode++;

      sprintf(ledString, "MODE %s", modeNames[currentMode]);
      DisplayText(ledString);
    }

    Serial.printf("Current State: %s (%d)\n", stateNames[currentState], currentState);

    Serial.print("  Current Mode: ");
    Serial.print(modeNames[currentMode]);
    Serial.printf("(%d)\n", currentMode);
    if (currentMode == MODESETUP)
    {
      Serial.printf(" Current Setting: %s (%s)", settingNames[currentSetting], GetCurrentSettingValueString());
    }
    Serial.println();
  }
}

void setupCurrentMode()
{
  char settingString[12];

  // Set the defcon state if we are not the clock or setting up, otherwise clear the RGB
  switch (currentMode)
  {
  case MODECLOCK:
    RGB_Clear(true);
    currentState = STATERUN;
    break;
  case MODESETUP:
    RGB_Clear(true);
    currentState = STATESET;
    sprintf(settingString, "%s %s", settingNames[currentSetting], GetCurrentSettingValueString());
    DisplayText(settingString);
    break;
  case MODEMOVIE:
  case MODERANDOM:
  case MODEMESSAGE:
  case MODETICKER:
    RGB_SetDefcon(5, true);
    ResetCode();
    Clear();
    currentState = STATERUN;
    break;
  default:
    currentState = STATEMENU;
    break;
  }
}

void BUT2Press()
{
  // Only allow a button press every 10ms
  if (nextButtonPress < millis())
  {
    nextButtonPress = millis() + 10;

    if (currentState == STATESET)
    {
      currentState = STATERUN;
    }
    else if (currentState == STATERUN)
    {
      if (currentMode == MODESETUP)
        decrementSetting();
    }
    // If in the menu, start whatever menu option we are in
    else if (currentState == STATEMENU)
    {
      setupCurrentMode();
    }
  }

  Serial.printf("Current State: %s (%d)\n", stateNames[currentState], currentState);

  Serial.print("  Current Mode: ");
  Serial.print(modeNames[currentMode]);
  if (currentMode == MODESETUP)
  {
    Serial.printf(" Current Setting: %s (%s)", settingNames[currentSetting], GetCurrentSettingValueString());
  }
  Serial.println();
}

void BUT2LongPress()
{
  // Finish setting state
  if ((currentState == STATERUN) & (currentMode == MODESETUP))
    currentState = STATEMENU;
  Serial.println("Long Press Button 2 - exit to Menu");

  // Apply settings
  Serial.printf("calling configTime(%d,%d,%s)",timeZone * 3600 + daylightSavings * 3600, 0, ntpServer);
  configTime(timeZone * 3600 + daylightSavings * 3600, 0, ntpServer);

  // Save settings to NVRAM if they have changed
  // Temp vars to compare saved with current
  uint8_t _menuTimeout;
  boolean _daylightSavings;
  int8_t _timeZone;
  char _timeSeparator;
  uint8_t _defaultMode;
  uint8_t _ledBrightness;

  if (prefs.getUChar("Timeout", MENU_TIMEOUT_DEFAULT) != menuTimeout)
    prefs.putUChar("Timeout", menuTimeout);
  if (prefs.getBool("DST", DST_DEFAULT) != daylightSavings)
    prefs.putUChar("DST", daylightSavings);
  if (prefs.getChar("TZone", TZ_DEFAULT) != timeZone)
    prefs.putUChar("TZone", timeZone);
  if (prefs.getUChar("Delim", TIMESEPARATOR) != timeSeparator)
    prefs.putUChar("Delim", timeSeparator);
  if (prefs.getUChar("Dflt", MODECLOCK) != defaultMode)
    prefs.putUChar("Dflt", defaultMode);
  if (prefs.getUChar("Bright", MAXBRIGHTNESS) != ledBrightness)
    prefs.putUChar("Bright", ledBrightness);
}

// Take the time data from the RTC and format it into a string we can display
void DisplayTime()
{
  // Store the current time into a struct
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
  {
    Serial.println("Failed to obtain time");
    return;
  }
  // Format the contents of the time struct into a string for display
  char DateAndTimeString[12];
  // if ( timeinfo.tm_hour < 10 )
  //   sprintf(DateAndTimeString, "   %d %02d %02d", timeinfo.tm_hour,timeinfo.tm_min,timeinfo.tm_sec);
  // else

  // CableTie 2020-07-25
  // - \0x17 is a modified character in the Adafruit font for a ^ using the bottom two segments (L and N)
  // - Gonna revisit this - might end up leaving it as a space character
  sprintf(DateAndTimeString, "  %02d%c%02d%c%02d", timeinfo.tm_hour, timeSeparator, timeinfo.tm_min, timeSeparator, timeinfo.tm_sec);

  // This maybe should be replaced with a call to DisplayText()
  // - need to make DateAndTimeString a String instead of a char* */

  // Iterate through each digit on the display and populate the time, or clear the digit
  uint8_t curDisplay = 0;
  uint8_t curDigit = 0;

  for (uint8_t i = 0; i < 10; i++)
  {
    matrix[curDisplay].writeDigitAscii(curDigit, DateAndTimeString[i]);
    curDigit++;
    if (curDigit == 4)
    {
      curDigit = 0;
      curDisplay++;
    }
  }

  // Show whatever is in the display buffer on the display
  Display();
}

// Display whatever is in txt on the display
void DisplayText(String txt)
{
  uint8_t curDisplay = 0;
  uint8_t curDigit = 0;

  Clear();

  // Iterate through each digit and push the character from the txt string into that position
  for (uint8_t i = 0; i < txt.length(); i++)
  {
    matrix[curDisplay].writeDigitAscii(curDigit, txt.charAt(i));
    curDigit++;
    if (curDigit == 4)
    {
      curDigit = 0;
      curDisplay++;
    }
  }

  // Show whatever is in the display buffer on the display
  Display();
}

// Return a random time step for the next solving solution
uint16_t GetNextSolveStep()
{
  return random(solveStepMin, solveStepMax) * solveStepMulti;
}

// Fill whatever is in the code buffer into the display buffer
void FillCodes()
{
  int matrix_index = 0;
  int character_index = 0;
  char c = 0;
  char c_code = 0;

  for (int i = 0; i < 12; i++)
  {
    c = displaybuffer[i];
    c_code = missile_code[i];
    if (c == '-')
    {
      // c is a character we need to randomise
      c = random(48, 91);
      while ((c > 57 && c < 65) || c == c_code)
        c = random(48, 91);
    }
    matrix[matrix_index].writeDigitAscii(character_index, c);
    character_index++;
    if (character_index == 4)
    {
      character_index = 0;
      matrix_index++;
    }
  }

  // Show whatever is in the display buffer on the display
  Display();
}

// Randomise the order of the code being solved
void RandomiseSolveOrder()
{
  for (uint8_t i = 0; i < 12; i++)
  {
    uint8_t ind = random(0, 12);
    while (code_solve_order_random[ind] < 99)
      ind = random(0, 12);

    code_solve_order_random[ind] = i;
  }
}

// Reset the code being solved back to it's starting state
void ResetCode()
{
  if (currentMode == MODEMOVIE)
  {
    solveStepMulti = 1;
    solveCountFinished = 10;
    for (uint8_t i = 0; i < 12; i++)
      missile_code[i] = missile_code_movie[i];
  }
  else if (currentMode == MODERANDOM)
  {
    solveStepMulti = 0.5;

    // Randomise the order in which we solve this code
    RandomiseSolveOrder();

    // Set the code length and populate the code with random chars
    solveCountFinished = 12;

    for (uint8_t i = 0; i < 12; i++)
    {
      Serial.print("Setting code index ");
      Serial.print(i);

      // c is a character we need to randomise
      char c = random(48, 91);
      while (c > 57 && c < 65)
        c = random(48, 91);

      Serial.print(" to char ");
      Serial.println(c);

      missile_code[i] = c;
    }
  }
  else if (currentMode == MODEMESSAGE)
  {
    solveStepMulti = 0.5;

    // Randomise the order in which we solve this code
    RandomiseSolveOrder();

    // Set the code length and populate the code with the stored message
    solveCountFinished = 12;
    for (uint8_t i = 0; i < 12; i++)
      missile_code[i] = missile_code_message[i];
  }

  // Set the first solve time step for the first digit lock

  solveStep = GetNextSolveStep();
  nextSolve = millis() + solveStep;
  solveCount = 0;
  lastDefconLevel = 0;

  // Clear code display buffer
  for (uint8_t i = 0; i < 12; i++)
  {
    if (currentMode == MODEMOVIE && (i == 3 || i == 8))
      displaybuffer[i] = ' ';
    else
      displaybuffer[i] = '-';
  }
}

/*  Solve the code based on the order of the solver for the current mode
 *  This is fake of course, but so was the film!
 *  The reason we solve based on a solver order, is so we can solve the code 
 *  in the order it was solved in the movie.
 */

void SolveCode()
{
  // If the number of digits solved is less than the number to be solved
  if (solveCount < solveCountFinished)
  {
    // Grab the next digit from the code based on the mode
    uint8_t index = 0;

    if (currentMode == MODEMOVIE)
    {
      index = code_solve_order_movie[solveCount];
      displaybuffer[index] = missile_code[index];
    }
    else
    {
      index = code_solve_order_random[solveCount];
      displaybuffer[index] = missile_code[index];
    }

    Serial.println("Found " + String(displaybuffer[index]) + " @ index: " + String(solveCount));

    // move tghe solver to the next digit of the code
    solveCount++;

    // Get current percentage of code solved so we can set the defcon display
    float solved = 1 - ((float)solveCount / (float)solveCountFinished);

    Serial.println("Solved " + String(solved));

    byte defconValue = int(solved * 5 + 1);
    RGB_SetDefcon(defconValue, false);

    Serial.println("Defcon " + String(defconValue));

    Serial.println("Next solve index: " + String(solveCount));

    FillCodes();

    // Long beep to indicate a digit in he code has been solved!
    ledcWriteTone(channel, 1500);
    beeping = true;
    beepCount = 3;
    nextBeep = millis() + 500;
  }
}

// Clear the contents of the 16 segment LED display buffers and update the display
void Clear()
{
  // There are 3 LED drivers
  for (int i = 0; i < 3; i++)
  {
    // There are 4 digits per LED driver
    for (int d = 0; d < 4; d++)
      matrix[i].writeDigitAscii(d, ' ');

    matrix[i].writeDisplay();
  }
}

// Show the contents of the display buffer on the displays
void Display()
{
  for (int i = 0; i < 3; i++)
    matrix[i].writeDisplay();
}

void RGB_SetDefcon(byte level, bool force)
{
  // Only update the defcon display if the value has changed
  // to prevent flickering
  if (lastDefconLevel != level || force)
  {
    lastDefconLevel = level;

    // Clear the RGB LEDs
    RGB_Clear();

    // Level needs to be clamped to between 0 and 4
    byte newLevel = constrain(level - 1, 0, 4);
    leds[newLevel] = defcon_colors[newLevel];

    RGB_FillBuffer();
  }
}

void RGB_Rainbow(int wait)
{
  if (nextRGB < millis())
  {
    nextRGB = millis() + wait;
    nextPixelHue += 256;

    if (nextPixelHue > 65536)
      nextPixelHue = 0;

    // For each RGB LED
    for (int i = 0; i < 5; i++)
    {
      int pixelHue = nextPixelHue + (i * 65536L / 5);
      leds[i] = gamma32(ColorHSV(pixelHue));
    }
    // Update RGB LEDs
    RGB_FillBuffer();
  }
  mqttReconnect();
}

void loop()
{
  // Used by OneButton to poll for button inputs
  Button1.tick();
  Button2.tick();

  // Check for MQTT
  if (!client.connected()) {
    mqttReconnect();
  }
  client.loop();

  // We are in the menu
  if (currentState == STATEMENU)
  {
    // We dont need to do anything here, but lets show some fancy RGB!
    RGB_Rainbow(10);
    // Determine timout to go to clock if in menu mode for too long
    if (menuTimeoutMillis < millis())
    {
      // Menu timout, go to default mode
      currentState = STATERUN;   // Running
      currentMode = defaultMode; // whatever is set as default mode, usually clock
      setupCurrentMode();

      // And report it
      Serial.print("Menu timout --> ");
      Serial.print(modeNames[currentMode]);
      Serial.println(" mode");
    }
  }
  else if (currentState == STATESET)
  {
  }
  // We are in run mode
  else
  {
    if (currentMode == MODECLOCK) // Clock
    {
      if (nextBeep < millis())
      {
        DisplayTime();
        nextBeep = millis() + 1000;
      }
      RGB_Rainbow(10);
    }
    // Setup mode where b1 and b2 increment and decrement the value
    else if (currentMode == MODESETUP) // Setup
    {
      if (nextTick < millis())
      {
        nextTick = millis() + tickStep;
        if (ledFlash)
          sprintf(displaybuffer, "%s %s", settingNames[currentSetting], GetCurrentSettingValueString());
        else
          sprintf(displaybuffer, "%s", settingNames[currentSetting], GetCurrentSettingValueString());
        DisplayText(displaybuffer);
        RGB_SetDefcon(1, false);
      }
    }
    else if (currentMode == MODETICKER)
    {
      DisplayText(tickerMessage)
      // ToDo Scroll Ticker message across the LED display
    }
    else
    {
      // If we have solved the code
      if (solveCount == solveCountFinished)
      {
        if (nextBeep < millis())
        {
          beeping = !beeping;
          nextBeep = millis() + 500;

          if (beeping)
          {
            if (beepCount > 0)
            {
              RGB_SetDefcon(1, true);
              FillCodes();
              beepCount--;
              ledcWriteTone(channel, 1500);
            }
            else
            {
              RGB_SetDefcon(1, true);
              DisplayText("LAUNCHING...");
              // Test return to clock timeout
              if (menuTimeoutMillis < millis())
              {
                currentMode = defaultMode;
                setupCurrentMode();
                // And report it
                Serial.print("Menu timout --> ");
                Serial.print(modeNames[currentMode]);
                Serial.println(" mode");
              }
            }
          }
          else
          {
            Clear();
            RGB_Clear(true);
            ledcWriteTone(channel, 0);
          }
        }

        // We are solved, so no point running any of the code below!
        return;
      }

      // Pat the menu timeout watchdog timer
      menuTimeoutMillis = millis() + S2MS(menuTimeout);

      // Only update the displays every "tickStep"
      if (nextTick < millis())
      {
        nextTick = millis() + tickStep;

        // This displays whatever teh current state of the display is
        FillCodes();

        // If we are not currently beeping, play some random beep/bop computer-y sounds
        if (!beeping)
          ledcWriteTone(channel, random(90, 250));
      }

      // This is where we solve each code digit
      // The next solve step is a random length to make it take a different time every run
      if (nextSolve < millis())
      {
        nextSolve = millis() + solveStep;
        // Set the solve time step to a random length
        solveStep = GetNextSolveStep();
        //
        SolveCode();
      }

      // Zturn off any beeping if it's trying to beep
      if (beeping)
      {
        if (nextBeep < millis())
        {
          ledcWriteTone(channel, 0);
          beeping = false;
        }
      }
    }
  }
}
