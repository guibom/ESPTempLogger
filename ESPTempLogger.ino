// ESP8266 + Attiny85 + DHT22 Temperature Logger
// Copyright (c) 2014, Guilherme Ramos <longinus@gmail.com>
// https://github.com/guibom/ESPTempLogger
// Released under the MIT license. See LICENSE file for details.

#include <avr/wdt.h>
#include <avr/sleep.h>
#include <stdlib.h>
#include <SoftwareSerial.h>
#include <DHT22.h>

//Pin definition for Attiny85 and regular arduino
#if defined(__AVR_ATtiny85__)
  #define ESP_ENABLE_PIN  2
  #define SOFT_RX_PIN     1
  #define SOFT_TX_PIN     0
  #define DHT22_PIN       3
  #define DHT22_POWER_PIN 4
#else
  //ATMEGA328
  #define ESP_ENABLE_PIN  8
  #define SOFT_RX_PIN     7
  #define SOFT_TX_PIN     6
  #define DHT22_PIN       4
  #define DHT22_POWER_PIN 3
#endif

//WiFi AP information
#define SSID "SSID"
#define PASS "PASS"

//Sensor id for MQTT message
#define SENSOR_ID "1"  //Ideally something more clever should be used, like MAC address, etc

//How many times to retry talking to module before giving up
#define RETRY_COUNT   5
uint8_t retry_attempt = 0;

//Comment to turn off Debug mode
//#define DEBUG

#ifdef DEBUG
  #define DEBUG_CONNECT(x)  Serial.begin(x)
  #define DEBUG_PRINT(x)    Serial.println(x)
  #define DEBUG_FLUSH()     Serial.flush()
  //To catch when I forget to turn off debug on attiny85
  #if defined(__AVR_ATtiny85__)
    #error("No space for debug information on attiny85. Comment #define DEBUG line!");
  #endif
#else
  #define DEBUG_CONNECT(x)
  #define DEBUG_PRINT(x)
  #define DEBUG_FLUSH()
#endif

//Disabling ADC saves ~230uAF. Needs to be re-enable for the internal voltage check
#define adc_disable() (ADCSRA &= ~(1<<ADEN)) // disable ADC
#define adc_enable()  (ADCSRA |=  (1<<ADEN)) // re-enable ADC

// Setup a DHT22 instance
DHT22 myDHT22(DHT22_PIN);

//Software serial connected to ESP8266
SoftwareSerial SoftSerial(SOFT_RX_PIN, SOFT_TX_PIN); // RX, TX

void setup() {

  //Callibration for internal oscillator from TinyTuner (will change for each MCU)
  #if defined(__AVR_ATtiny85__)  
    OSCCAL = 0xA9;
  #endif

  //Setup pins
  pinMode(DHT22_POWER_PIN, OUTPUT);
  pinMode(ESP_ENABLE_PIN, OUTPUT);

  digitalWrite(ESP_ENABLE_PIN, LOW);

  //Start with DHT22 power pin high to turn on the status LED
  digitalWrite(DHT22_POWER_PIN, HIGH);

  //Debug serial print only works if DEBUG is defined
  DEBUG_CONNECT(9600);
  DEBUG_PRINT(F("Arduino started"));

  //Make sure ESP8266 is set to 9600
  SoftSerial.begin(9600);

  //Small delay on first powerup
  delay(1000);
}


void loop(){
  //Turn on ESP, get temperature and send to UDP server
  updateTemp();
  
  //Disable ADC
  adc_disable();
  
  DEBUG_PRINT(F("Going to sleep..."));
  DEBUG_FLUSH();

  //Powerdown for 5 minutes
  powerdownDelay(300000);

  //Start back here when woken from sleep
  DEBUG_PRINT(F("Woke up from sleep!"));

  //Re-enable ADC 
  adc_enable();
}

void updateTemp(){

  //Hold DHT22 information
  DHT22_ERROR_t errorCode;

  //Power on DHT22
  digitalWrite(DHT22_POWER_PIN, HIGH);
  DEBUG_PRINT(F("DHT22 Power ON"));  

  //Try to enable ESP8266 Module. Return if can't
  if (!enableESP()) {
    DEBUG_PRINT(F("Can't talk to the module"));
    disableESP();

    //Powerdown DHT22
    digitalWrite(DHT22_POWER_PIN, LOW);
    DEBUG_PRINT(F("DHT22 Power OFF"));

    return;
  }

  //Set transmission mode
  SoftSerial.println(F("AT+CIPMODE=1"));
  SoftSerial.flush();
  delay(500);
  
  //Connect to UDP server
  SoftSerial.println(F("AT+CIPSTART=\"UDP\",\"192.168.0.175\",6969"));
  SoftSerial.flush();
  delay(1000);

  //Check for connection errors
  if (!SoftSerial.find("OK")) {
    DEBUG_PRINT(F("Error connecting to server"));
    cleanUp(); //Close connection and clean up
    return;
  }
    
  //Connected fine to server
  DEBUG_PRINT(F("Connected to server!"));

  //Read DHT22 that will be sent
  errorCode = myDHT22.readData();

  //If DHT22 is not returning valid data
  if (errorCode != DHT_ERROR_NONE) {    
    DEBUG_PRINT(F("Could not get DHT22 Sensor values. Error:"));
    DEBUG_PRINT(errorCode);
    cleanUp(); //Close connection and clean up
    return;
  }
  
  SoftSerial.println(F("AT+CIPSEND"));
  SoftSerial.flush();
  delay(500);

  if (SoftSerial.find(">")) {

    DEBUG_PRINT(F("Ready to send..."));

    //Create MQTT messages array
    char cmd[64];
    sprintf(cmd, "{\"T\":\"home/sensor/%s/temperature\",\"P\":%hi.%01hi,\"R\":1}", SENSOR_ID, myDHT22.getTemperatureCInt()/10, abs(myDHT22.getTemperatureCInt()%10));

    SoftSerial.println(cmd);
    SoftSerial.flush();
    DEBUG_PRINT(cmd);
    delay(250);

    sprintf(cmd, "{\"T\":\"home/sensor/%s/humidity\",\"P\":%i.%01i,\"R\":1}", SENSOR_ID, myDHT22.getHumidityInt()/10, myDHT22.getHumidityInt()%10);

    SoftSerial.println(cmd);
    SoftSerial.flush();
    DEBUG_PRINT(cmd);
    delay(250);

    sprintf(cmd, "{\"T\":\"home/sensor/%s/battery\",\"P\":%d,\"R\":1}", SENSOR_ID, readVcc());

    SoftSerial.println(cmd);
    SoftSerial.flush();
    DEBUG_PRINT(cmd);
    delay(500);
    
    DEBUG_PRINT(F("All messages sent!"));      
    cleanUp(); //Close connection and clean up

    return;
  }    
  
  DEBUG_PRINT(F("Error waiting for input or sending"));
  cleanUp(); //Close connection and clean up
  return;
}

//Only connect to AP if it's not already connected.
//It should always reconnect automatically.
boolean connectWiFi() {

  //Check if already connected
  SoftSerial.println(F("AT+CWJAP?"));
  SoftSerial.flush();
  delay(100);
    
  if (SoftSerial.find("+CWJAP:\"")) {  
    #ifdef DEBUG
      char buf[12];
      SoftSerial.readBytesUntil(0x22, buf, sizeof(buf));
      DEBUG_PRINT(F("Connected to AP:"));
      DEBUG_PRINT(buf);
    #endif

    return true;
  }

  //If not connected, connect now
  DEBUG_PRINT(F("Not connected to AP yet"));

  SoftSerial.println("AT+CWMODE=1");
  DEBUG_PRINT(F("STA Mode set"));
  delay(500);
  
  //Create connection string
  char cmd[48];
  sprintf(cmd, "AT+CWJAP=\"%s\",\"%s\"", SSID, PASS);
  SoftSerial.println(cmd);
  SoftSerial.flush();
  DEBUG_PRINT(cmd);
  delay(10000);

  if(SoftSerial.find("OK")){
    return true;
  }else{
    return false;
  }
}

void cleanUp() {
  SoftSerial.println(F("AT+CIPCLOSE"));
  SoftSerial.flush();
  DEBUG_PRINT(F("Closed connection"));

  //Clear 64byte receive buffer
  clearBuffer();
  DEBUG_PRINT(F("Buffer Empty"));

  //Powerdown DHT22
  digitalWrite(DHT22_POWER_PIN, LOW);
  DEBUG_PRINT(F("DHT22 Power OFF"));

  //Bring chip enable pin low
  disableESP();
}

//Remove all bytes from the buffer
void clearBuffer() {

  while (SoftSerial.available())
    SoftSerial.read();
}

//Bring Enable pin up, wait for module to wake-up/connect and turn off echo.
bool enableESP() {

  //Enable ESP8266 Module
  digitalWrite(ESP_ENABLE_PIN, HIGH);
  DEBUG_PRINT(F("ESP8266 Enabled. Waiting to spin up..."));

  //Wait for module to boot up and connect to AP
  delay(6000);  
  clearBuffer(); //Clear local buffer

  //Local echo off
  SoftSerial.println(F("ATE0"));
  SoftSerial.flush();
  delay(1000);

  if (SoftSerial.find("OK")) {
    DEBUG_PRINT(F("Local echo OFF"));
    DEBUG_PRINT(F("ESP8266 UART link okay"));

    //Connect to AP if not already
    return connectWiFi();    
  }

  //Couldn't connect to the module
  DEBUG_PRINT(F("Error initializing the module"));
  
  //Try again until retry counts expire
  if (retry_attempt <= RETRY_COUNT) {
    delay(2000);
    retry_attempt++;
    return enableESP();
  } else {
    retry_attempt = 0;
    return false;
  }
}

//Bring Enable pin down
bool disableESP() {

  //Disable ESP8266 Module
  digitalWrite(ESP_ENABLE_PIN, LOW);
  DEBUG_PRINT(F("ESP8266 Disabled"));
  delay(100);

  return true;
}

//-----------------------------------------------------------------------------
//Sleep code from Narcoleptic library -- https://code.google.com/p/narcoleptic
//Tweaked for Attiny85 and used straight for small code size
//-----------------------------------------------------------------------------
SIGNAL(WDT_vect) {
  wdt_disable();
  wdt_reset();
#if defined(__AVR_ATtiny85__) || defined(__AVR_ATtiny45__) || defined(__AVR_ATtiny25__)
  WDTCR &= ~_BV(WDIE);
#else
  WDTCSR &= ~_BV(WDIE);
#endif
}

void powerdown(uint8_t wdt_period) {
  wdt_enable(wdt_period);
  wdt_reset();

#if defined(__AVR_ATtiny85__) || defined(__AVR_ATtiny45__) || defined(__AVR_ATtiny25__)
  WDTCR |= _BV(WDIE);
#else
  WDTCSR |= _BV(WDIE);
#endif

  set_sleep_mode(SLEEP_MODE_PWR_DOWN);

  // Set sleep enable (SE) bit:
  sleep_enable();

  //Going to sleep
  sleep_mode();

  //Woke up
  sleep_disable();
  
  wdt_disable();

#if defined(__AVR_ATtiny85__) || defined(__AVR_ATtiny45__) || defined(__AVR_ATtiny25__)
  WDTCR &= ~_BV(WDIE);
#else
  WDTCSR &= ~_BV(WDIE);
#endif
}

void powerdownDelay(unsigned long milliseconds) {
  while (milliseconds >= 8000) { powerdown(WDTO_8S); milliseconds -= 8000; }
  if (milliseconds >= 4000)    { powerdown(WDTO_4S); milliseconds -= 4000; }
  if (milliseconds >= 2000)    { powerdown(WDTO_2S); milliseconds -= 2000; }
  if (milliseconds >= 1000)    { powerdown(WDTO_1S); milliseconds -= 1000; }
  if (milliseconds >= 500)     { powerdown(WDTO_500MS); milliseconds -= 500; }
  if (milliseconds >= 250)     { powerdown(WDTO_250MS); milliseconds -= 250; }
  if (milliseconds >= 125)     { powerdown(WDTO_120MS); milliseconds -= 120; }
  if (milliseconds >= 64)      { powerdown(WDTO_60MS); milliseconds -= 60; }
  if (milliseconds >= 32)      { powerdown(WDTO_30MS); milliseconds -= 30; }
  if (milliseconds >= 16)      { powerdown(WDTO_15MS); milliseconds -= 15; }
}

//-----------------------------------------------------------------------------
//Code from https://code.google.com/p/tinkerit/wiki/SecretVoltmeter
//-----------------------------------------------------------------------------
int readVcc() {
  // Read 1.1V reference against AVcc
  // set the reference to Vcc and the measurement to the internal 1.1V reference
#if defined(__AVR_ATmega32U4__) || defined(__AVR_ATmega1280__) || defined(__AVR_ATmega2560__)
  ADMUX = _BV(REFS0) | _BV(MUX4) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
#elif defined (__AVR_ATtiny24__) || defined(__AVR_ATtiny44__) || defined(__AVR_ATtiny84__)
  ADMUX = _BV(MUX5) | _BV(MUX0);
#elif defined (__AVR_ATtiny25__) || defined(__AVR_ATtiny45__) || defined(__AVR_ATtiny85__)
  ADMUX = _BV(MUX3) | _BV(MUX2);
#else
  ADMUX = _BV(REFS0) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
#endif 

  delay(2); // Wait for Vref to settle
  ADCSRA |= _BV(ADSC); // Start conversion
  while (bit_is_set(ADCSRA,ADSC)); // measuring

  uint8_t low  = ADCL; // must read ADCL first - it then locks ADCH 
  uint8_t high = ADCH; // unlocks both

  long result = (high<<8) | low;

  //result = 1126400L / result; // Calculate Vcc (in mV);
  result = 1074835L / result;
  return (int)result; // Vcc in millivolts
}