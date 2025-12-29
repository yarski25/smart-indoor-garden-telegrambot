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
#include <freertos/queue.h>
#include <freertos/event_groups.h>

#ifdef ESP8266
  X509List cert(TELEGRAM_CERTIFICATE_ROOT);
#endif

WiFiClientSecure client;
UniversalTelegramBot bot(botToken, client);
String chat_id;

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

// Bot Queue (Outgoing to Telegram)
#define BOT_MSG_LEN 512 // Telegram message length
typedef struct
{
  char text[BOT_MSG_LEN];
  char parse_mode[12];
} BotMsg;

QueueHandle_t botQueue;

// Command queue (from Telegram to Control)
typedef enum
{
  CMD_NONE,
  CMD_START,
  CMD_GET_WATER_STATUS,
  CMD_PUMP_ON,
  CMD_LAMP_ON
} CommandType;

typedef enum {
  WAIT_NONE,
  WAIT_LAMP_HOURS,
  WAIT_PUMP_MIN
} PendingState;

PendingState pendingState = WAIT_NONE;

typedef struct
{
  CommandType type;
  uint32_t value; // minutes | hours | unused
} CommandMessage;

QueueHandle_t commandQueue;

// Control -> HW queues
QueueHandle_t pumpQueue;
QueueHandle_t lampQueue;

// handlers
TaskHandle_t ControlTaskHandle = NULL;
TaskHandle_t WiFiTaskHandle = NULL;
TaskHandle_t TelegramTaskHandle = NULL;
TaskHandle_t TimeTaskHandle = NULL;
TaskHandle_t DayLightTaskHandle = NULL;
TaskHandle_t WaterPumpTaskHandle = NULL;

/*********
 * UTILS *
 *********/

/**
 * Creates message in C friendly format
 * @param name - message input
 * @param out - message output (char msg[size]; needs to be defined)
 * @param size - size to protect overflow
 */
void createMsg(const char* name, char* out, const size_t size)
{
  snprintf(out, size, "%s", name);
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
 * sends telegram messages
 * @param msg
 * @param parse_mode - empty (standard text), HTML
 */
void enqueueBotMessage(String msg, String parse_mode="") {
  BotMsg m;
  msg.toCharArray(m.text, BOT_MSG_LEN);
  parse_mode.toCharArray(m.parse_mode, 12);
  xQueueSend(botQueue, &m, 0);
}

/**
 * inits HW components
 */
void initComponents()
{
  // init measurement sensor
  pinMode(CMS_PIN, INPUT);
  vTaskDelay(200 / portTICK_PERIOD_MS);

  // init water pump
  pinMode(RELAY_PIN_PUMP, OUTPUT);
  vTaskDelay(200 / portTICK_PERIOD_MS);

  // set water pump relay off
  digitalWrite(RELAY_PIN_PUMP, HIGH);
  vTaskDelay(200 / portTICK_PERIOD_MS);

  // init daylight lamp
  pinMode(RELAY_PIN_LAMP, OUTPUT);
  vTaskDelay(200 / portTICK_PERIOD_MS);

  // set daylight lamp relay off
  digitalWrite(RELAY_PIN_LAMP, HIGH);
  vTaskDelay(200 / portTICK_PERIOD_MS);
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
int getWaterLevelPercent() {
  int raw = analogRead(CMS_PIN);
  raw = map(raw, CMS_AIR, CMS_WATER, 0, 100);
  raw = constrain(raw, 0, 100);
  return raw;
}

/**
 * Evaluates water level and returns text feedback
 * @param waterLevel
 * @return pointer to char array
 */
const char* evaluateWaterLevel(int waterLevel)
{
  if (waterLevel > 80)
    return "water level is HIGH, system OK";
  else if (waterLevel > 50)
    return "water level is MEDIUM, please refill water tank";
  else
    return "water level is too LOW, please refill water tank, irrigation system operation is limited";
}

/**
 * Sets WiFi modem to power save mode
 */
void enableWiFiPowerSave() {
  // Minimum power-saving mode
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  vTaskDelay(200 / portTICK_PERIOD_MS);
  Serial.println("WiFi set to modem sleep (idle) mode");
}

/*********
 * TASKS *
 *********/

/**
 * control task
 * @param pvParameters
 */
void ControlTask(void *pvParameters) {
  Serial.println("ControlTask started");

  CommandMessage cmd;

  while (true) {
    if (xQueueReceive(commandQueue, &cmd, portMAX_DELAY)) {

      switch(cmd.type) {

        case CMD_START: {
          // String from_name = bot.messages[i].from_name;
          String html_msg = "Welcome to <strong>Smart Indoor Garden</strong>.\n";
          html_msg += "I'm dog bot and I will help you with this garden.\n\n";
          html_msg += "<a href='/water_status'>/water_status</a> -> <em>returns water tank state in percentage</em>\n";
          html_msg += "<a href='/water_pump_on'>/water_pump_on</a> -> <em>set water pump ON</em>\n";
          html_msg += "<a href='/daylight_lamp_on'>/daylight_lamp_on</a> -> <em>set daylight lamp ON</em>\n";
          enqueueBotMessage(html_msg, "HTML");
          break;
        }

        case CMD_GET_WATER_STATUS: {
          // int water = analogRead(CMS_PIN);
          // water = map(water, CMS_AIR, CMS_WATER, 0, 100);
          const int water = getWaterLevelPercent();
          enqueueBotMessage("Water level: " + String(water) + "%");
          break;
        }

        case CMD_PUMP_ON: {
          enqueueBotMessage("Pump ON for " + String(cmd.value) + " min");
          xQueueSend(pumpQueue, &cmd.value, portMAX_DELAY);
          break;
        }

        case CMD_LAMP_ON: {
          enqueueBotMessage("Lamp ON for " + String(cmd.value) + " hr");
          xQueueSend(lampQueue, &cmd.value, portMAX_DELAY);
          break;
        }
        default:
          break;
      }
    }
  }
}

/**
 * Handles WiFi connection
 * @param pvParameters
 */
void WiFiTask(void *pvParameters) {
  Serial.println("WiFiTask started");
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

    vTaskDelay(30000 / portTICK_PERIOD_MS);
  }
}

/**
 * Handles time synchronization
 * @param pvParameters
 */
void TimeTask(void *pvParameters) {
  Serial.println("TimeTask started");
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
  Serial.println("DayLightTask started");
  // pinMode(RELAY_PIN_LAMP, OUTPUT);
  uint32_t durationHr;

  while (true) {
    if (xQueueReceive(lampQueue, &durationHr, portMAX_DELAY)) {

      digitalWrite(RELAY_PIN_LAMP, LOW);
      vTaskDelay(200 / portTICK_PERIOD_MS);
      Serial.println("LAMP ON");
      printFreeStack("Day light");

      unsigned long offTime = millis() + durationHr * 3600000UL;

      while ((long)(millis() - offTime) < 0) {
        vTaskDelay(500 / portTICK_PERIOD_MS);
      }

      digitalWrite(RELAY_PIN_LAMP, HIGH);
      vTaskDelay(200 / portTICK_PERIOD_MS);
      Serial.println("LAMP OFF");

      enqueueBotMessage("Lamp OFF");
    }
  }
}

/**
 * Handles water pump task
 * @param pvParameters
 */
void WaterPumpTask(void *pvParameters) {
  Serial.println("WaterPumpTask started");
  uint32_t durationMin;

  while (true) {
    if (xQueueReceive(pumpQueue, &durationMin, portMAX_DELAY)) {

      digitalWrite(RELAY_PIN_PUMP, LOW);
      vTaskDelay(200 / portTICK_PERIOD_MS);
      Serial.println("PUMP ON");

      unsigned long offTime = millis() + durationMin * 60000UL;

      while ((long)(millis() - offTime) < 0) {
        vTaskDelay(500 / portTICK_PERIOD_MS);
      }

      digitalWrite(RELAY_PIN_PUMP, HIGH);
      vTaskDelay(200 / portTICK_PERIOD_MS);
      Serial.println("PUMP OFF");

      enqueueBotMessage("Pump OFF");
    }
  }
}

/**
 * Handles telegram bot messages
 * @param pvParameters
 */
void TelegramTask(void *pvParameters) {
  Serial.println("TelegramTask started");

  while (true) {

    // ---- SEND OUTGOING ----
    BotMsg out;
    while (xQueueReceive(botQueue, &out, 0) == pdTRUE) {
      bot.sendMessage(CHAT_ID, out.text, out.parse_mode);
    }

    // ---- CHECK INCOMING ----
    int numMessages = bot.getUpdates(bot.last_message_received + 1);

    while (numMessages) {
      for (int i = 0; i < numMessages; i++) {

        String text = bot.messages[i].text;

        CommandMessage cmd = {CMD_NONE, 0};

        if(text.startsWith("/start"))
          cmd.type = CMD_START;

        else if (pendingState == WAIT_LAMP_HOURS) {
          if (isValidNumber(text)) {
            cmd.type = CMD_LAMP_ON;
            cmd.value = text.substring(1).toInt();
          } else {
            enqueueBotMessage("Invalid value. Please enter a number > 0");
          }

          pendingState = WAIT_NONE;
        }

        else if (pendingState == WAIT_PUMP_MIN) {

          if (isValidNumber(text)) {
            cmd.type = CMD_PUMP_ON;
            cmd.value = text.substring(1).toInt();
          } else {
            enqueueBotMessage("Invalid value. Please enter a number > 0");
          }

          pendingState = WAIT_NONE;
        }

        else if (text.startsWith("/water_status"))
          cmd.type = CMD_GET_WATER_STATUS;

        else if (text.startsWith("/water_pump_on")) {
          enqueueBotMessage("Please send irrigation duration in minutes");
          pendingState = WAIT_PUMP_MIN;
        }

        else if (text.startsWith("/daylight_lamp_on")) {
          enqueueBotMessage("Please send daylight duration in hours");
          pendingState = WAIT_LAMP_HOURS;
        }

        if (cmd.type != CMD_NONE)
          xQueueSend(commandQueue, &cmd, 0);
      }

      numMessages = bot.getUpdates(bot.last_message_received + 1);
    }

    vTaskDelay(2000 / portTICK_PERIOD_MS);
  }
}

/**
 * init setup
 */
void setup() {
  Serial.begin(115200);
  vTaskDelay(200 / portTICK_PERIOD_MS);
  Serial.println("system init...");
  Serial.println("");

  Serial.println("starting at default frequency...");
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
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    Serial.println("connecting to WiFi..");
  }
  // print WiFi signal power
  Serial.print("RSSI=");
  Serial.print(WiFi.RSSI());
  Serial.println("dBm");

  // print ESP32 Local IP Address
  Serial.println(WiFi.localIP());

  // init HW components
  initComponents();

  // init queues
  botQueue = xQueueCreate(10, sizeof(BotMsg));
  commandQueue = xQueueCreate(10, sizeof(CommandMessage));
  pumpQueue = xQueueCreate(5, sizeof(uint32_t));
  lampQueue = xQueueCreate(5, sizeof(uint32_t));

  // notify telegram bot ready to operate
  bot.sendMessage(CHAT_ID, "bot started", "");

  // init tasks
  xTaskCreatePinnedToCore(WiFiTask, "WiFiTask", 2048, NULL, 3, &WiFiTaskHandle, 0);
  xTaskCreatePinnedToCore(ControlTask, "ControlTask", 2048, NULL, 2, &ControlTaskHandle, 1);
  xTaskCreatePinnedToCore(TimeTask, "TimeTask", 2048, NULL, 1, &TimeTaskHandle, 1);
  xTaskCreatePinnedToCore(TelegramTask, "TelegramTask", 8192, NULL, 2, &TelegramTaskHandle, 1);
  xTaskCreatePinnedToCore(DayLightTask, "DayLightTask", 2048, NULL, 1,  &DayLightTaskHandle, 1);
  xTaskCreatePinnedToCore(WaterPumpTask, "WaterPumpTask", 2048, NULL, 1,  &WaterPumpTaskHandle, 1);

  Serial.println("system READY...");
  Serial.println("");

}

/**
 * main loop
 */
void loop() {
  vTaskDelay(portMAX_DELAY);
}