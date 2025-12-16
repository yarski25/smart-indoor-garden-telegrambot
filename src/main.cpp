// -----------------------------
// Smart Indoor Garden
// -----------------------------
// Author: Yaroslav Saprykin
// Date: 13/4/2023
// HW =>
// capacity moisture sensor to detect water tank state
// water pump 5V
// grow lamp
// 2-channel relay to control water pump and grow lamp
// external power supply

#include <Arduino.h>
#include <xxx.h>
#ifdef ESP32
  #include <WiFi.h>
#else
  #include <ESP8266WiFi.h>
#endif
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include "time.h"

#ifdef ESP8266
  X509List cert(TELEGRAM_CERTIFICATE_ROOT);
#endif

WiFiClientSecure client;
UniversalTelegramBot bot(botToken, client);

// Checks for new messages every 1 second.
int botRequestDelay = 1000;
unsigned long lastTimeBotRan;

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 0;
const int   daylightOffset_sec = 0;

// HW configs
#define CMS_PIN 33 // Arduino pin that connects to analog pin of capacity moisture sensor (CMS)
#define CMS_AIR 3200 // low level of CMS
#define CMS_WATER 1200 // high level of CMS

// CMS intervals (air, wet, water)
// VERY wET <230;330>
// WET <330; 430>
// DRY <430; 530>

//int intervals = (AIR_VALUE - WATER_VALUE)/3;

#define RELAY_PIN_LAMP 25  // Arduino pin that connects to relay lamp

#define RELAY_PIN_PUMP 32  // Arduino pin that connects to relay pump

unsigned long myTime1;
unsigned long myTime2;

enum State{NOACTION, WATERPUMPON, DAYLIGHTLAMPON};
State actualState = NOACTION;

// function to check if string is valid number
boolean isValidNumber(String str) {
  if(str.charAt(1) == '0'){  
    return false;
  }
    
  for(byte i=1;i<str.length();i++){
    if(!isDigit(str.charAt(i))){
      return false;
    }
  }

  return true;
}


// Handle what happens when you receive new messages
void handleNewMessages(int numNewMessages) {
  Serial.println("handleNewMessages");
  Serial.println(String(numNewMessages));

  for (int i=0; i<numNewMessages; i++) {
    // Chat id of the requester
    String chat_id = String(bot.messages[i].chat_id);
    Serial.print("CHAT_ID: ");
    Serial.println(chat_id);

    if (chat_id != CHAT_ID){
      bot.sendMessage(chat_id, "Unauthorized user", "");
      continue;
    }
    
    // Print the received message
    String text = bot.messages[i].text;
    Serial.println(text);

    String from_name = bot.messages[i].from_name;
    
    if (text == "/water_status") {

      int waterLevel = analogRead(CMS_PIN);
      //bot.sendMessage(chat_id, "water level value is " + String(waterLevel), "");
      waterLevel = map(waterLevel, CMS_AIR, CMS_WATER, 0, 100);
      bot.sendMessage(chat_id, "water level is " + String(waterLevel) + "% ", "");
      delay(500);
      if (waterLevel > 80){
        bot.sendMessage(chat_id, "water level is HIGH, system OK", "");
      }
      else if( (waterLevel <= 80) && (waterLevel> 50) ){
        bot.sendMessage(chat_id, "water level is MEDIUM, please refill water tank", "");
      }
      else{
        bot.sendMessage(chat_id, "water level is too LOW, please refill water tank", "");
        delay(500);
        bot.sendMessage(chat_id, "irrigation system operation is limited", "");
      }
      delay(500);
    }

    if (text == "/daylight_lamp_on") {

      bot.sendMessage(chat_id, "please enter daylight lamp operation time in ms: ", "");
      delay(500);

      actualState = DAYLIGHTLAMPON;
    }

    if (text == "/water_pump_on") {

      bot.sendMessage(chat_id, "please enter pump operation time in ms: ", "");
      delay(500);

      actualState = WATERPUMPON;

    }

    if (isValidNumber(text)) {

      int duration = text.substring(1).toInt();

      if (actualState == WATERPUMPON){

        int waterLevel = analogRead(CMS_PIN);
        waterLevel = map(waterLevel, CMS_AIR, CMS_WATER, 0, 100);
        bot.sendMessage(chat_id, "water level is " + String(waterLevel) + "% ", "");
        delay(500);
        bot.sendMessage(chat_id, "water pump ON", "");
        delay(500);
        myTime1 = millis();
        digitalWrite(RELAY_PIN_PUMP, LOW);
        Serial.println("water pump ON");
        delay(duration);
        digitalWrite(RELAY_PIN_PUMP, HIGH);
        Serial.println("water pump OFF");
        myTime2 = millis();
        bot.sendMessage(chat_id, "water pump OFF", "");
        delay(500);
        Serial.print("water pump operated for ");
        Serial.print((myTime2-myTime1));
        Serial.println("ms");
        bot.sendMessage(chat_id, "water pump operated for " + String(myTime2-myTime1) + "ms", "");
        delay(60000);
        
        waterLevel = analogRead(CMS_PIN);
        waterLevel = map(waterLevel, CMS_AIR, CMS_WATER, 0, 100);
        bot.sendMessage(chat_id, "water level is " + String(waterLevel) + "% ", "");
        delay(500);

        actualState = NOACTION;

      }

      else if (actualState == DAYLIGHTLAMPON){

        bot.sendMessage(chat_id, "daylight lamp is ON", "");
        delay(500);
        myTime1 = millis();
        digitalWrite(RELAY_PIN_LAMP, LOW);
        Serial.println("daylight lamp ON");
        delay(duration);
        digitalWrite(RELAY_PIN_LAMP, HIGH);
        Serial.println("daylight lamp OFF");
        myTime2 = millis();
        bot.sendMessage(chat_id, "daylight lamp OFF", "");
        delay(500);
        Serial.print("daylight lamp operated for ");
        Serial.print((myTime2-myTime1));
        Serial.println("ms");
        bot.sendMessage(chat_id, "daylight lamp operated for " + String(myTime2-myTime1) + "ms", "");

        actualState = NOACTION;
      }
      
      else {
        actualState = NOACTION;
      }

    }

    if (text == "/start")
    {
      String html_msg = "Welcome to <strong>Smart Indoor Garden</strong>, " + from_name + ".\n";
      html_msg += "I'm dog bot and I will help you with this garden.\n\n";
      html_msg += "<a href='/water_status'>/water_status</a> -> <em>returns water tank state in percentage</em>\n";
      html_msg += "<a href='/water_pump_on'>/water_pump_on</a> -> <em>set water pump ON</em>\n";
      html_msg += "<a href='/daylight_lamp_on'>/daylight_lamp_on</a> -> <em>set daylight lamp ON</em>\n";

      bot.sendMessage(chat_id, html_msg, "HTML");
      delay(500);
    }
  }
}

void isBotAlive(){
  // Verify the bot info
  if (bot.getMe()) {
    Serial.println("Bot is working!");
  } else {
    Serial.println("Error: Bot is not responding!");
  }
}

void setTimezone(String timezone){
  //Serial.printf("  Setting Timezone to %s\n",timezone.c_str());
  setenv("TZ",timezone.c_str(),1);  //  Now adjust the TZ.  Clock settings are adjusted to show the new local time
  tzset();
}

void initTime(String timezone){
  struct tm timeinfo;

  //get time via NTP
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  if(!getLocalTime(&timeinfo)){
    Serial.println("failed to obtain time");
    return;
  }
  // Now we can set the real timezone
  setTimezone(timezone);
}

void printLocalTime()
{
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    Serial.println("failed to obtain time");
    return;
  }
  Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S zone %Z %z ");
}

void setup() {
  Serial.begin(115200);
  Serial.print("system init...");
  Serial.println("");

  #ifdef ESP8266
    client.setTrustAnchors(&cert); // Add root certificate for api.telegram.org
  #endif

  // Connect to Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  //WiFi.begin(SSID, PASSWORD);
  #ifdef ESP32
    client.setCACert(TELEGRAM_CERTIFICATE_ROOT); // Add root certificate for api.telegram.org
  #endif
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("connecting to WiFi..");
  }
  // print WiFi signal power
  Serial.print("RSSI=");
  Serial.print(WiFi.RSSI());
  Serial.println("dBm");

  // Print ESP32 Local IP Address
  Serial.println(WiFi.localIP());

  // Check NTP time
  // Serial.println("");
  initTime(actualTZ);
  printLocalTime();

  bot.sendMessage(CHAT_ID, "bot started", "");
  delay(500);

  pinMode(CMS_PIN, INPUT);
  delay(100);
  //analogReadResolution(12);                   // Sets the sample bits and read resolution, default is 12-bit (0 - 4095), range is 9 - 12 bits
  //delay(100);
  //analogSetWidth(12);                         // Sets the sample bits and read resolution, default is 12-bit (0 - 4095), range is 9 - 12 bits
  //delay(100);

  pinMode(RELAY_PIN_PUMP, OUTPUT);
  delay(100);
  digitalWrite(RELAY_PIN_PUMP, HIGH);
  delay(3000);
  pinMode(RELAY_PIN_LAMP, OUTPUT);
  delay(100);
  digitalWrite(RELAY_PIN_LAMP, HIGH);
  delay(3000);
  Serial.println("system READY...");
  Serial.println("");
}

void loop() {

  if (millis() > lastTimeBotRan + botRequestDelay) {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);

    while(numNewMessages) {
      Serial.println("response received");
      handleNewMessages(numNewMessages);
      numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    }
    lastTimeBotRan = millis();
  }

}