#include "AiEsp32RotaryEncoder.h"
#include "Arduino.h"
#include "si5351.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <TinyGPSPlus.h>
#include <SoftwareSerial.h>
#include <SPI.h>
#include <RTClib.h>

Si5351 si5351;
// calibration factor for si5351 based on the calibration tool
int32_t cal_factor = 148000;

// global definitions
#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 64 // OLED display height, in pixels

// Defines for Frequency Display
#define FREQUENCY_DIVISOR_HZ 1
#define FREQUENCY_DIVISOR_KHZ 10
#define FREQUENCY_DIVISOR_MHZ 10000

#define PIN_SP 14
#define PIN_TX 12
uint16_t duration = 85; // Morse code typing speed in milliseconds - higher number means slower typing
uint16_t hz = 750;      // Volume up if a high ohm speaker connected to circuit of the morse generator

String cw_message = "VVV de K5VZ  LOCATOR IS EM13lb  PWR IS 10mW  ANT IS 6BVT VERTICAL"; // Your message

// global variables
// Declaration for an SSD1306 display connected to I2C (SDA, SCL pins)
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

/*
connecting Rotary encoder

Rotary encoder side    MICROCONTROLLER side
-------------------    ---------------------------------------------------------------------
CLK (A pin)            any microcontroler intput pin with interrupt -> in this example pin 32
DT (B pin)             any microcontroler intput pin with interrupt -> in this example pin 21
SW (button pin)        any microcontroler intput pin with interrupt -> in this example pin 25
GND - to microcontroler GND
VCC                    microcontroler VCC (then set ROTARY_ENCODER_VCC_PIN -1)

***OR in case VCC pin is not free you can cheat and connect:***
VCC                    any microcontroler output pin - but set also ROTARY_ENCODER_VCC_PIN 25
                        in this example pin 25

*/
#if defined(ESP8266)
#define ROTARY_ENCODER_A_PIN D6
#define ROTARY_ENCODER_B_PIN D5
#define ROTARY_ENCODER_BUTTON_PIN D7
#else
#define ROTARY_ENCODER_A_PIN 18
#define ROTARY_ENCODER_B_PIN 5
#define ROTARY_ENCODER_BUTTON_PIN 19
#endif
#define ROTARY_ENCODER_VCC_PIN -1 /* 27 put -1 of Rotary encoder Vcc is connected directly to 3,3V; else you can use declared output pin for powering rotary encoder */

// depending on your encoder - try 1,2 or 4 to get expected behaviour
#define ROTARY_ENCODER_STEPS 4
unsigned long shortPressAfterMiliseconds = 50;  // how long a short press shoud be. Do not set too low to avoid bouncing (false press events).
unsigned long longPressAfterMiliseconds = 1000; // how long a long press shoud be.

// instead of changing here, rather change numbers above
AiEsp32RotaryEncoder rotaryEncoder = AiEsp32RotaryEncoder(ROTARY_ENCODER_A_PIN, ROTARY_ENCODER_B_PIN, ROTARY_ENCODER_BUTTON_PIN, ROTARY_ENCODER_VCC_PIN, ROTARY_ENCODER_STEPS);

const int bandswitch[] = {160, 80, 40, 20, 15, 10, 6};
const int bandswitch_max = 6;
const long freqswitch_low[] = {18000, 35000, 70000, 140000, 210000, 280000, 500000};
const long freqswitch_high[] = {20000, 40000, 73000, 143500, 214500, 297000, 540000};

const String modes[] = {"USB", "LSB", "CW", "WSPR", "FT8", "FT4"};
int current_mode = 2;

int current_band = 4;
long current_freq[] = {18010, 35010, 70010, 140010, 210010, 280010, 500010};
long target_freq;

/*
   This sample sketch demonstrates the normal use of a TinyGPSPlus (TinyGPSPlus) object.
   It requires the use of SoftwareSerial, and assumes that you have a
   9600-baud serial GPS device hooked up on pins 17(rx) and 16(tx).
*/
static const int RXPin = 17, TXPin = 16;
static const uint32_t GPSBaud = 9600;

bool gpsLocationValid = false;
bool gpsDateValid = false;
bool gpsTimeValid = false;
String gridSquare = "";

// The TinyGPSPlus object
TinyGPSPlus gps;

// The serial connection to the GPS device
SoftwareSerial ss(RXPin, TXPin);

// DS1307 RTC
RTC_DS1307 rtc;

// predefined function prototypes
// rotary encoder functions
void init_rotary_encoder();
void on_button_short_click();
void on_button_long_click();
void handle_band_change();
void rotary_loop();
void IRAM_ATTR readEncoderISR();
// si5351 functions
void init_si5351();
void set_si5351_freuency(long target_freq);
// oled display functions
void clearDisplay();
void showDisplay();
void displayFrequency(uint64_t frequency);
void displayBand();
void displayMode();
void displayTime();
void displayGridSquare(String gridSquare);
void display_loop();
// cw beacon functions
void send_cw_message();
void cw_string_proc(String str);
void cw_char_proc(char m);
void di();
void dah();
void char_space();
void word_space();
void cw(bool state);
// GPS routines
void initGPSSync();
void displayGPSInfo();
static char *get_mh(double lat, double lon, int size);

void setup()
{
  Serial.begin(115200);
  // connect to GPS
  ss.begin(GPSBaud);
  // set tx led pin mode
  pinMode(PIN_TX, OUTPUT);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
  { // Address 0x3D for 128x64
    Serial.println(F("SSD1306 allocation failed"));
    for (;;)
      ;
  }
  // pause to ensure everything initializes
  delay(2000);
  // SETUP RTC MODULE
  if (!rtc.begin())
  {
    Serial.println("RTC module is NOT found");
    Serial.flush();
    while (1)
      ;
  }

  target_freq = current_freq[current_band];

  init_rotary_encoder();
  init_si5351();
  while (gridSquare == "")
  {
    initGPSSync();
  }
}

void loop()
{
  rotary_loop();
  display_loop();
  set_si5351_freuency(target_freq);
  delay(50);
}

#pragma region gps_functions

void initGPSSync()
{
  // This sketch displays information every time a new sentence is correctly encoded.
  while (ss.available() > 0)
  {
    if (gps.encode(ss.read()))
    {
      displayGPSInfo();
    }
    if (gpsLocationValid == true && gpsDateValid == true && gpsTimeValid == true)
    {
      return;
    }
  }

  if (millis() > 5000 && gps.charsProcessed() < 10)
  {
    Serial.println(F("No GPS detected: check wiring."));
    while (true)
      delay(500);
  }
}

void displayGPSInfo()
{
  Serial.print(F("Location: "));
  if (gps.location.isValid())
  {
    gpsLocationValid = true;
    Serial.print(gps.location.lat(), 6);
    Serial.print(F(","));
    Serial.print(gps.location.lng(), 6);
    Serial.print(F(", Grid Square: "));
    gridSquare = get_mh(gps.location.lat(), gps.location.lng(), 4);
    Serial.println(gridSquare);
    Serial.println(F("GridSqure udpaed from GPS"));
  }
  else
  {
    gpsLocationValid = false;
    Serial.print(F("INVALID"));
  }

  Serial.print(F("  Date/Time: "));
  if (gps.date.isValid())
  {
    gpsDateValid = true;
    Serial.print(gps.date.month());
    Serial.print(F("/"));
    Serial.print(gps.date.day());
    Serial.print(F("/"));
    Serial.print(gps.date.year());
  }
  else
  {
    gpsDateValid = false;
    Serial.print(F("INVALID"));
  }

  Serial.print(F(" "));
  if (gps.time.isValid())
  {
    gpsTimeValid = true;
    if (gps.time.hour() < 10)
      Serial.print(F("0"));
    Serial.print(gps.time.hour());
    Serial.print(F(":"));
    if (gps.time.minute() < 10)
      Serial.print(F("0"));
    Serial.print(gps.time.minute());
    Serial.print(F(":"));
    if (gps.time.second() < 10)
      Serial.print(F("0"));
    Serial.print(gps.time.second());
    Serial.print(F("."));
    if (gps.time.centisecond() < 10)
      Serial.print(F("0"));
    Serial.println(gps.time.centisecond());
  }
  else
  {
    gpsTimeValid = false;
    Serial.print(F("INVALID"));
  }

  if (gpsDateValid && gpsTimeValid)
  {
    rtc.adjust(DateTime(gps.date.year(), gps.date.month(), gps.date.day(), gps.time.hour(), gps.time.minute(), gps.time.second()));
    Serial.println(F("RTC Updated with GPS Time"));
  }

  Serial.println();
  display_loop();
}

char letterize(int x)
{
  return (char)x + 65;
}

static char *get_mh(double lat, double lon, int size)
{
  static char locator[11];
  double LON_F[] = {20, 2.0, 0.083333, 0.008333, 0.0003472083333333333};
  double LAT_F[] = {10, 1.0, 0.0416665, 0.004166, 0.0001735833333333333};
  int i;
  lon += 180;
  lat += 90;

  if (size <= 0 || size > 10)
    size = 6;
  size /= 2;
  size *= 2;

  for (i = 0; i < size / 2; i++)
  {
    if (i % 2 == 1)
    {
      locator[i * 2] = (char)(lon / LON_F[i] + '0');
      locator[i * 2 + 1] = (char)(lat / LAT_F[i] + '0');
    }
    else
    {
      locator[i * 2] = letterize((int)(lon / LON_F[i]));
      locator[i * 2 + 1] = letterize((int)(lat / LAT_F[i]));
    }
    lon = fmod(lon, LON_F[i]);
    lat = fmod(lat, LAT_F[i]);
  }
  locator[i * 2] = 0;
  return locator;
}

#pragma endregion gps_functions

#pragma region rotary_encoder
// start rotary encoder functions
void init_rotary_encoder()
{
  // we must initialize rotary encoder
  rotaryEncoder.begin();
  rotaryEncoder.setup(readEncoderISR);

  // set boundaries and if values should cycle or not
  // in this example we will set possible values to the band frequencies limits;
  bool circleValues = false;
  rotaryEncoder.setBoundaries(freqswitch_low[current_band], freqswitch_high[current_band], circleValues); // minValue, maxValue, circleValues true|false (when max go to min and vice versa)
  rotaryEncoder.setEncoderValue(current_freq[current_band]);

  rotaryEncoder.disableAcceleration(); // acceleration is now enabled by default - disable if you dont need it
  rotaryEncoder.setAcceleration(250);  // or set the value - larger number = more accelearation; 0 or 1 means disabled acceleration
}

void on_button_short_click()
{
  Serial.print("button SHORT press ");
  handle_band_change();
}

void on_button_long_click()
{
  Serial.print("button LONG press ");
  send_cw_message();
}

void handle_rotary_button()
{
  static unsigned long lastTimeButtonDown = 0;
  static bool wasButtonDown = false;

  bool isEncoderButtonDown = rotaryEncoder.isEncoderButtonDown();
  // isEncoderButtonDown = !isEncoderButtonDown; // uncomment this line if your button is reversed

  if (isEncoderButtonDown)
  {
    if (!wasButtonDown)
    {
      // start measuring
      lastTimeButtonDown = millis();
    }
    // else we wait since button is still down
    wasButtonDown = true;
    return;
  }

  // button is up
  if (wasButtonDown)
  {
    // click happened, lets see if it was short click, long click or just too short
    if (millis() - lastTimeButtonDown >= longPressAfterMiliseconds)
    {
      on_button_long_click();
    }
    else if (millis() - lastTimeButtonDown >= shortPressAfterMiliseconds)
    {
      on_button_short_click();
    }
  }

  wasButtonDown = false;
}

void handle_band_change()
{
  current_band++;
  Serial.printf("button clicked, new index = %d\n", current_band);
  if (current_band > bandswitch_max)
  {
    current_band = 0;
    Serial.println("band array limit reached roll over to index 0");
  }
  Serial.print("band changed to ");
  Serial.printf("%d meters\n", bandswitch[current_band]);
  bool circleValues = false;
  rotaryEncoder.setBoundaries(freqswitch_low[current_band], freqswitch_high[current_band], circleValues); // minValue, maxValue, circleValues true|false (when max go to min and vice versa)
  rotaryEncoder.setEncoderValue(current_freq[current_band]);
  target_freq = current_freq[current_band];
}

void rotary_loop()
{
  // dont print anything unless value changed
  if (rotaryEncoder.encoderChanged())
  {
    Serial.print("Encoder Value: ");
    Serial.println(rotaryEncoder.readEncoder());
    current_freq[current_band] = rotaryEncoder.readEncoder();
    target_freq = current_freq[current_band];
  }

  // if (rotaryEncoder.isEncoderButtonClicked())
  // {
  handle_rotary_button();
  //}
}

void IRAM_ATTR readEncoderISR()
{
  rotaryEncoder.readEncoder_ISR();
}
// end rotary encoder functions
#pragma endregion rotary_encoder

#pragma region si5351
// start si5351 functions
void init_si5351()
{
  // The crystal load value needs to match in order to have an accurate calibration
  si5351.init(SI5351_CRYSTAL_LOAD_8PF, 0, 0);

  // Start on target frequency
  si5351.set_correction(cal_factor, SI5351_PLL_INPUT_XO);
  si5351.set_pll(SI5351_PLL_FIXED, SI5351_PLLA);
  uint64_t si5352_freq = target_freq * 10000ULL;
  si5351.set_freq(si5352_freq, SI5351_CLK0);
  si5351.output_enable(SI5351_CLK0, 0);
}

void set_si5351_freuency(long target_freq)
{
  uint64_t si5352_freq = target_freq * 10000ULL;
  si5351.set_freq(si5352_freq, SI5351_CLK0);
}
// end si5351 functions
#pragma endregion si5351

#pragma region oled_display
// start oled display functions
void clearDisplay()
{
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
}

void showDisplay()
{
  display.display();
}

void displayFrequency(uint64_t frequency)
{
  // Serial.printf("display frequecy: %d\n", frequency);

  uint64_t freqMhz = frequency / FREQUENCY_DIVISOR_MHZ;
  char megaHz[4];
  sprintf(megaHz, "%d", freqMhz);

  frequency -= freqMhz * FREQUENCY_DIVISOR_MHZ;
  // Serial.printf("%d\n", frequency);

  uint64_t freqKhz = frequency / FREQUENCY_DIVISOR_KHZ;
  char kiloHz[4];
  sprintf(kiloHz, "%03d", freqKhz);

  frequency -= freqKhz * FREQUENCY_DIVISOR_KHZ;
  // Serial.printf("frequency %d\n", frequency);

  uint64_t freqHz = frequency * 100;
  char hertz[4];
  sprintf(hertz, "%03d", freqHz);

  char buffer[40];
  sprintf(buffer, "%s.%s,%s", megaHz, kiloHz, hertz);
  // Serial.println(buffer);

  display.setCursor(0, 5);
  display.setTextSize(2);
  display.printf(buffer);
}

void displayBand()
{
  display.setCursor(0, 23);
  display.setTextSize(1);
  display.printf("Band: %d meters", bandswitch[current_band]);
}

void displayMode()
{
  display.setCursor(0, 33);
  display.setTextSize(1);
  display.printf("Mode: %s", modes[current_mode]);
}

void displayTime()
{
  DateTime now = rtc.now();
  display.setCursor(0, 43);
  display.setTextSize(1);
  display.printf("Time: %02d:%02d:%02d", now.hour(), now.minute(), now.second());
}

void displayGridSquare(String gridSquare)
{
  display.setCursor(0, 53);
  display.setTextSize(1);
  display.printf("GRID: %s", gridSquare);
}

void display_loop()
{
  clearDisplay();
  displayFrequency(target_freq);
  displayBand();
  displayMode();
  displayTime();
  displayGridSquare(gridSquare);
  showDisplay();
}
// end oled display functions
#pragma endregion oled_display

#pragma region cw_beacon
// start cw beacon functions
void send_cw_message()
{
  cw_string_proc(cw_message);
  delay(500); // Duration of the break at the end before the long signal - in milliseconds

  cw(true);
  delay(5000); // Duration of the long signal at the end - in milliseconds

  cw(false);
  delay(1000);
}

void cw_string_proc(String str)
{ // Processing string to characters
  for (uint8_t j = 0; j < str.length(); j++)
    cw_char_proc(str[j]);
}
//----------
void cw_char_proc(char m)
{ // Processing characters to Morse symbols
  String s;

  if (m == ' ')
  { // Pause between words
    word_space();
    return;
  }

  if (m > 96) // ACSII, case change a-z to A-Z
    if (m < 123)
      m -= 32;

  switch (m)
  { // Morse
  case 'A':
    s = ".-#";
    break;
  case 'B':
    s = "-...#";
    break;
  case 'C':
    s = "-.-.#";
    break;
  case 'D':
    s = "-..#";
    break;
  case 'E':
    s = ".#";
    break;
  case 'F':
    s = "..-.#";
    break;
  case 'G':
    s = "--.#";
    break;
  case 'H':
    s = "....#";
    break;
  case 'I':
    s = "..#";
    break;
  case 'J':
    s = ".---#";
    break;
  case 'K':
    s = "-.-#";
    break;
  case 'L':
    s = ".-..#";
    break;
  case 'M':
    s = "--#";
    break;
  case 'N':
    s = "-.#";
    break;
  case 'O':
    s = "---#";
    break;
  case 'P':
    s = ".--.#";
    break;
  case 'Q':
    s = "--.-#";
    break;
  case 'R':
    s = ".-.#";
    break;
  case 'S':
    s = "...#";
    break;
  case 'T':
    s = "-#";
    break;
  case 'U':
    s = "..-#";
    break;
  case 'V':
    s = "...-#";
    break;
  case 'W':
    s = ".--#";
    break;
  case 'X':
    s = "-..-#";
    break;
  case 'Y':
    s = "-.--#";
    break;
  case 'Z':
    s = "--..#";
    break;

  case '1':
    s = ".----#";
    break;
  case '2':
    s = "..---#";
    break;
  case '3':
    s = "...--#";
    break;
  case '4':
    s = "....-#";
    break;
  case '5':
    s = ".....#";
    break;
  case '6':
    s = "-....#";
    break;
  case '7':
    s = "--...#";
    break;
  case '8':
    s = "---..#";
    break;
  case '9':
    s = "----.#";
    break;
  case '0':
    s = "-----#";
    break;

  case '?':
    s = "..--..#";
    break;
  case '=':
    s = "-...-#";
    break;
  case ',':
    s = "--..--#";
    break;
  case '/':
    s = "-..-.#";
    break;
  }

  for (uint8_t i = 0; i < 7; i++)
  {
    switch (s[i])
    {
    case '.':
      di();
      break; // di
    case '-':
      dah();
      break; // dah
    case '#':
      char_space();
      return; // end of morse symbol
    }
  }
}
//----------
void di()
{
  cw(true); // TX di
  delay(duration);

  cw(false); // stop TX di
  delay(duration);
}
//----------
void dah()
{
  cw(true); // TX dah
  delay(3 * duration);

  cw(false); // stop TX dah
  delay(duration);
}
//----------
void char_space()
{                      // 3x, pause between letters
  delay(2 * duration); // 1x from end of character + 2x from the beginning of new character
}
//----------
void word_space()
{                      // 7x, pause between words
  delay(6 * duration); // 1x from end of the word + 6x from the beginning of new word
}
//----------
void cw(bool state)
{ // TX-CW, TX-LED, 750 Hz sound
  if (state)
  {
    si5351.output_enable(SI5351_CLK0, 1);
    digitalWrite(PIN_TX, HIGH);
    tone(PIN_SP, hz);
  }
  else
  {
    si5351.output_enable(SI5351_CLK0, 0);
    digitalWrite(PIN_TX, LOW);
    noTone(PIN_SP);
  }
}
// end cw beacon functions
#pragma endregion cw_beacon
