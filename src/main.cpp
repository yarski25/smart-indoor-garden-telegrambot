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
// #define configUSE_TRACE_FACILITY 1
// #define configUSE_STATS_FORMATTING_FUNCTIONS 1

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
#include "esp_wifi.h"

#ifdef ESP8266
  X509List cert(TELEGRAM_CERTIFICATE_ROOT);
#endif

WiFiClientSecure client;
UniversalTelegramBot bot(botToken, client);
String chat_id;

// Checks for new messages every 1 second.
// int botRequestDelay = 1000;
// unsigned long lastTimeBotRan;

// const char* ntpServer = "pool.ntp.org";
// const long  gmtOffset_sec = 0;
// const int   daylightOffset_sec = 0;

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

// unsigned long myTime1;
// unsigned long myTime2;

bool dayLightActive = false;
bool waterPumpActive = false;

unsigned long dayLightOffTime = 0;
unsigned long dayLightOnTime = 0;
unsigned long waterPumpOffTime = 0;
unsigned long waterPumpOnTime = 0;

enum State{NOACTION, WATERPUMPON, DAYLIGHTLAMPON};
State actualState = NOACTION;

SemaphoreHandle_t botMutex;

/**
 * prints free stack for task
 * @param taskName
 */
void printFreeStack(const char *taskName)
{
  UBaseType_t freeStack = uxTaskGetStackHighWaterMark(NULL);
  Serial.print(taskName);
  Serial.print(" free stack words = ");
  Serial.println(freeStack);   // each word = 4 bytes
}

/**
 * Checks if string is valid number
 * @param str String
 * @return boolean
 */
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

/**
 * Checks bot availability
 */
void isBotAlive(){
  // Verify the bot info
  if (bot.getMe()) {
    Serial.println("Bot is working!");
  } else {
    Serial.println("Error: Bot is not responding!");
  }
}

/**
 * Checks water level sensor
 */
void checkWaterLevel()
{
  printFreeStack("CheckWaterLevel");
  int waterLevel = analogRead(CMS_PIN);
  waterLevel = map(waterLevel, CMS_AIR, CMS_WATER, 0, 100);
  xSemaphoreTake(botMutex, portMAX_DELAY);
  bot.sendMessage(chat_id, "water level is " + String(waterLevel) + "% ", "");
  xSemaphoreGive(botMutex);
  if (waterLevel > 80){
    xSemaphoreTake(botMutex, portMAX_DELAY);
    bot.sendMessage(chat_id, "water level is HIGH, system OK", "");
    xSemaphoreGive(botMutex);
  }
  else if( (waterLevel <= 80) && (waterLevel> 50) ){
    xSemaphoreTake(botMutex, portMAX_DELAY);
    bot.sendMessage(chat_id, "water level is MEDIUM, please refill water tank", "");
    xSemaphoreGive(botMutex);
  }
  else{
    xSemaphoreTake(botMutex, portMAX_DELAY);
    bot.sendMessage(chat_id, "water level is too LOW, please refill water tank", "");
    xSemaphoreGive(botMutex);
    xSemaphoreTake(botMutex, portMAX_DELAY);
    bot.sendMessage(chat_id, "irrigation system operation is limited", "");
    xSemaphoreGive(botMutex);
  }
}

/**
 * Sets WiFi modem to power save mode
 */
void enableWiFiPowerSave() {
  // Minimum power-saving mode
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  Serial.println("WiFi set to modem sleep (idle) mode");
}

/**
 * Handles WiFi connection
 * @param pvParameters
 */
void WiFiTask(void *pvParameters) {
  Serial.println("WiFiTask()");
  while (true) {
    // printFreeStack("WiFiTask");
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi disconnected, reconnecting...");
      WiFi.begin(ssid, password);

      uint8_t tries = 0;
      while (WiFi.status() != WL_CONNECTED && tries < 20) {
        vTaskDelay(500 / portTICK_PERIOD_MS); // each 0.5 s
        tries++;
      }

      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi connected");
        enableWiFiPowerSave(); // set modem sleep
      }
    }

    // Internet test (HTTPS)
    // if (client.connect("www.google.com", 443)) {
    //   Serial.println("Internet OK");
    //   client.stop();
    // } else {
    //   Serial.println("Internet connection LOST");
    // }

    vTaskDelay(30000 / portTICK_PERIOD_MS);
  }
}

/**
 * Handles time synchronization
 * @param pvParameters
 */
void TimeTask(void *pvParameters) {
  Serial.println("TimeTask()");
  configTime(0, 0, "pool.ntp.org");

  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)) {
    // printFreeStack("TimeTask");
    Serial.println("Waiting for time...");
    vTaskDelay(2000 / portTICK_PERIOD_MS);
  }

  Serial.println("Time synchronized");
  vTaskDelete(NULL);
}

/**
 * Handles day lamp task
 * @param pvParameters
 */
void DayLightTask(void *pvParameters) {
  Serial.println("DayLightTask()");
  while (true) {
    // printFreeStack("DayLightTask");
    if (dayLightActive && (long)(millis() - dayLightOffTime) >= 0){
      digitalWrite(RELAY_PIN_LAMP, LOW);
      dayLightActive = false;
      Serial.println("Daylight lamp OFF (timer expired)");
      Serial.print("Daylight lamp operated for ");
      Serial.print((dayLightOffTime-dayLightOnTime));
      Serial.println("ms");
      xSemaphoreTake(botMutex, portMAX_DELAY);
      bot.sendMessage(chat_id, "daylight lamp operated for " + String(dayLightOffTime-dayLightOnTime) + "ms", "");
      xSemaphoreGive(botMutex);
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

/**
 * Handles water pump task
 * @param pvParameters
 */
void WaterPumpTask(void *pvParameters){
  Serial.println("WaterPumpTask()");
  while (true) {
    // printFreeStack("WaterPumpTask");
    if (waterPumpActive && (long)(millis() - waterPumpOffTime) >= 0){
      digitalWrite(RELAY_PIN_PUMP, LOW);
      waterPumpActive = false;
      Serial.println("Water pump OFF (timer expired)");
      Serial.print("Water pump operated for ");
      Serial.print((waterPumpOffTime-waterPumpOnTime));
      Serial.println("ms");
      xSemaphoreTake(botMutex, portMAX_DELAY);
      bot.sendMessage(chat_id, "water pump operated for " + String(waterPumpOffTime-waterPumpOnTime) + "ms", "");
      xSemaphoreGive(botMutex);

      int waterLevel = analogRead(CMS_PIN);
      waterLevel = map(waterLevel, CMS_AIR, CMS_WATER, 0, 100);
      xSemaphoreTake(botMutex, portMAX_DELAY);
      bot.sendMessage(chat_id, "water level is " + String(waterLevel) + "% ", "");
      xSemaphoreGive(botMutex);

    }
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

/**
 * Handles telegram bot messages
 * @param pvParameters
 */
void TelegramTask(void *pvParameters) {
  Serial.println("TelegramTask()");
  while (true) {
    // printFreeStack("TelegramTask");
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);

    // Serial.println("handleNewMessages");
    // Serial.println(String(numNewMessages));

    while (numNewMessages) {
      for (int i = 0; i < numNewMessages; i++) {

        // Chat id of the requester
        chat_id = String(bot.messages[i].chat_id);
        // Serial.print("CHAT_ID: ");
        // Serial.println(chat_id);

        if (chat_id != CHAT_ID){
          xSemaphoreTake(botMutex, portMAX_DELAY);
          bot.sendMessage(chat_id, "Unauthorized user", "");
          xSemaphoreGive(botMutex);
          continue;
        }

        // Print the received message
        String text = bot.messages[i].text;
        Serial.println(text);

        if (text.startsWith("/water_status")) {
          checkWaterLevel();
        }

        if (text.startsWith("/daylight_lamp_on")) {
          xSemaphoreTake(botMutex, portMAX_DELAY);
          bot.sendMessage(chat_id, "please enter daylight lamp operation time in hours: ", "");
          xSemaphoreGive(botMutex);
          actualState = DAYLIGHTLAMPON;
        }

        if (text.startsWith("/water_pump_on")) {
          xSemaphoreTake(botMutex, portMAX_DELAY);
          bot.sendMessage(chat_id, "please enter pump operation time in minutes: ", "");
          xSemaphoreGive(botMutex);
          actualState = WATERPUMPON;
        }

        if (isValidNumber(text)) {

          int duration = text.substring(1).toInt();

          if (actualState == WATERPUMPON){

            int waterLevel = analogRead(CMS_PIN);
            waterLevel = map(waterLevel, CMS_AIR, CMS_WATER, 0, 100);
            xSemaphoreTake(botMutex, portMAX_DELAY);
            bot.sendMessage(chat_id, "water level is " + String(waterLevel) + "% ", "");
            xSemaphoreGive(botMutex);

            digitalWrite(RELAY_PIN_PUMP, HIGH);
            Serial.println("Water pump ON");
            waterPumpActive = true;
            waterPumpOffTime = millis() + (unsigned long)duration * 60000UL;
            waterPumpOnTime = millis();
            xSemaphoreTake(botMutex, portMAX_DELAY);
            bot.sendMessage(chat_id, "water pump ON for " + String(duration) + " minutes", "");
            xSemaphoreGive(botMutex);

            actualState = NOACTION;

          }

          else if (actualState == DAYLIGHTLAMPON){

            digitalWrite(RELAY_PIN_LAMP, HIGH);
            Serial.println("Daylight lamp ON");
            dayLightActive = true;
            dayLightOffTime = millis() + (unsigned long)duration * 3600000UL;
            dayLightOnTime = millis();
            xSemaphoreTake(botMutex, portMAX_DELAY);
            bot.sendMessage(chat_id, "daylight lamp ON for " + String(duration) + " hours", "");
            xSemaphoreGive(botMutex);

            actualState = NOACTION;
          }

          else {
            actualState = NOACTION;
          }

        }

        if (text.startsWith("/start"))
        {
          String from_name = bot.messages[i].from_name;
          String html_msg = "Welcome to <strong>Smart Indoor Garden</strong>, " + from_name + ".\n";
          html_msg += "I'm dog bot and I will help you with this garden.\n\n";
          html_msg += "<a href='/water_status'>/water_status</a> -> <em>returns water tank state in percentage</em>\n";
          html_msg += "<a href='/water_pump_on'>/water_pump_on</a> -> <em>set water pump ON</em>\n";
          html_msg += "<a href='/daylight_lamp_on'>/daylight_lamp_on</a> -> <em>set daylight lamp ON</em>\n";
          xSemaphoreTake(botMutex, portMAX_DELAY);
          bot.sendMessage(chat_id, html_msg, "HTML");
          xSemaphoreGive(botMutex);
        }
      }

      numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    }

    vTaskDelay(10000 / portTICK_PERIOD_MS);
  }
}

/**
 * init setup
 */
void setup() {
  Serial.begin(115200);
  Serial.println("system init...");
  Serial.println("");

  Serial.println("starting at default frequency...");
  // Set CPU to 80 MHz for power saving
  setCpuFrequencyMhz(80);
  Serial.println("CPU frequency set to 80 MHz.");
  Serial.print("current CPU freq: ");
  Serial.print(getCpuFrequencyMhz());
  Serial.println(" MHz");

  #ifdef ESP8266
    client.setTrustAnchors(&cert); // Add root certificate for api.telegram.org
  #endif

  // Connect to Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
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

  bot.sendMessage(CHAT_ID, "bot started", "");

  pinMode(CMS_PIN, INPUT);
  delay(100);

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

  botMutex = xSemaphoreCreateMutex();

  xTaskCreatePinnedToCore(WiFiTask, "WiFiTask", 4096, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(TimeTask, "TimeTask", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(TelegramTask, "TelegramTask", 8192, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(DayLightTask, "DayLightTask", 2048, NULL, 1,  NULL, 1);
  xTaskCreatePinnedToCore(WaterPumpTask, "WaterPumpTask", 8192, NULL, 2,  NULL, 1);

}

/**
 * main loop
 */
void loop() {
  vTaskDelay(portMAX_DELAY);
}