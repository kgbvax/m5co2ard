#include <Wire.h>
#include <M5Stack.h>
#include "time.h"

//I/O
#include "SparkFun_SCD30_Arduino_Library.h"
#include "MutichannelGasSensor.h"
#undef DEFAULT_I2C_ADDR
#include <Tomoto_HM330X.h> 
#include <FastLED.h>

//networking
#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>

//not in SCM, provide your own defining WIFI_SSID, WIFI_PASSWORD
#include "envs.h"
#include "version.h"

//we dweet right now
//#include "dweet.h"
#include <ArduinoJson.h>

//for watchdog
#include "esp_system.h"
const int loopTimeCtl = 0;
hw_timer_t *timer = NULL;

void IRAM_ATTR resetModule()
{
    ets_printf("reboot\n");
    esp_restart();
}

//undefined in m5StickC
//#define pdMS_TO_TICKS( xTimeInMs ) ( ( TickType_t ) ( ( ( TickType_t ) ( xTimeInMs ) * ( TickType_t ) configTICK_RATE_HZ ) / ( TickType_t ) 1000 ) )

Tomoto_HM330X *pMSensor;
// Tomoto_HM330X sensor(Wire1); // to use the alternative wire

//M5Stack onboard leds
#define M5STACK_FIRE_NEO_NUM_LEDS 10
#define M5STACK_FIRE_NEO_DATA_PIN 15
CRGB leds[M5STACK_FIRE_NEO_NUM_LEDS];
TaskHandle_t Task1;

#define USE_WIFI
const char *ssid{WIFI_SSID};         // write your WiFi SSID (2.4GHz)
const char *password{WIFI_PASSWORD}; // write your WiFi password
const char *ntpServer = "europe.pool.ntp.org";
const long gmtOffset_sec = 3600;
const int daylightOffset_sec = 3600;
const String dweetThingName = "ottermuehle-co2ampel-2";

const int watchdogTimeoutUs = 15000000; //in usec
const int loopDelay = 5000;            //main loop() delay

// sensor setting
SCD30 airSensor;
#define SENSOR_INTERVAL_S 2  // get sensor value every SENSOR_INTERVAL_S [s]
#define UPLOAD_INTERVAL_S 60 // upload data to Ambient every UPLOAD_INTERVAL_S [s]

// TFT setting
//#define TFT_WIDTH 320
//#define TFT_HEIGHT 240
#define SPRITE_WIDTH 320
#define SPRITE_HEIGHT 160

// CO2 limits/setting
#define CO2_MIN_PPM 0
#define CO2_MAX_PPM 2500
#define CO2_RANGE_INT 500
#define CO2_CAUTION_PPM 750
#define CO2_WARNING_PPM 1500

enum
{
    LEVEL_NORMAL,
    LEVEL_CAUTION,
    LEVEL_WARNING
};

// variables
TFT_eSprite graph_co2 = TFT_eSprite(&M5.Lcd);
TFT_eSprite spr_values = TFT_eSprite(&M5.Lcd);
uint16_t co2_ppm;
float temperature_c, humidity_p;
int p_cau, p_war;
int elapsed_time = 0;
int co2_level_last = LEVEL_NORMAL;
int co2_level_now = LEVEL_NORMAL;

#define TRIANGLE(x1,y1,x2,y2 ,x3,y3) M5.Lcd.fillTriangle(pad+(x1),pad+(y1),pad+(x2),pad+(y2),pad+(x3),pad+(y3), WHITE)
void drawM() {
  const int pad=4; 
  const int my = M5.Lcd.height()-pad*2;
  const int mx = M5.Lcd.width()-pad*2;
 
  TRIANGLE(0,my/2,mx/2,my/2,mx/4, 0); // ▲ Links
  TRIANGLE(mx/2,my/2,mx/2+mx/4,0,mx, my/2); // ▲ Rechts
  TRIANGLE(0,my/2,mx/8,my-my/4, mx/4,my/2); // ▾ Links
  TRIANGLE(mx-(mx/4),my/2,mx,my/2,mx-(mx/8),my-my/4); // ▾ Rechts
  TRIANGLE(mx/4,my/2,mx/2,my, mx-(mx/4),my/2); // ▼
}

void displaySwStats() {
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(0, 0,2);
  M5.Lcd.setTextSize(1);
  esp_chip_info_t ci;
  esp_chip_info(&ci);
  Serial.println("chip model " + String(ci.model));
  Serial.println("chip revision " + String(ci.revision));
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  char macBuf[20]; //xx-xx-xx-xx-xx-xx
  sprintf(macBuf, "%x-%x-%x-%x-%x-%x", (unsigned int)mac[0], (unsigned int)mac[1], (unsigned int)mac[2], (unsigned int)mac[3], (unsigned int)mac[4], (unsigned int)mac[5]);
  macStr = String(macBuf);
  M5.Lcd.setTextSize(2);
  Serial.println("MAC: " + macStr);
  M5.Lcd.println("MAC: " + macStr);
  Serial.println("Version: " + String(VERSION));
  M5.Lcd.println("Version: " + String(VERSION));
  M5.Lcd.setTextSize(1);
  Serial.println("MD5: " + EspClass().getSketchMD5());
  M5.Lcd.println("MD5: " + EspClass().getSketchMD5());
  Serial.println("Build timestamp: " + String(BUILD_TIMESTAMP));
  M5.Lcd.println("Build timestamp:  " + String(BUILD_TIMESTAMP));
  M5.Lcd.println("configured SSID: " + String(ssid));
}

uint16_t getColor(uint8_t red, uint8_t green, uint8_t blue)
{
    return ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3);
}

int getPositionY(int ppm)
{
    return SPRITE_HEIGHT - (int32_t)((float)SPRITE_HEIGHT / (CO2_MAX_PPM - CO2_MIN_PPM) * ppm);
}

//FRTOS task
void updateLedTask(void *parameter)
{
    for (;;)
    {
        for (uint8_t n = 0; n < M5STACK_FIRE_NEO_NUM_LEDS; n++)
        {
            if (co2_level_now == LEVEL_NORMAL)
            {
                leds[n] = CRGB::DarkGreen;
                FastLED.setBrightness(5);
                M5.Lcd.setBrightness(100);
            }
            else if (co2_level_now == LEVEL_CAUTION)
            {
                leds[n] = CRGB::Yellow;
                FastLED.setBrightness(25);
                M5.Lcd.setBrightness(200);
            }
            else if (co2_level_now == LEVEL_WARNING)
            {
                leds[n] = CRGB::Red;
                FastLED.setBrightness(255);
                M5.Lcd.setBrightness(255);
            }
            else
            {
                leds[n] = CRGB::DarkGray;
            }
        }
        FastLED.show();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void feedWatchdog()
{
    timerWrite(timer, 0); //reset timer (feed watchdog)
}

void setup()
{
    //setup watchdog early to allow trigger even during setup
    timer = timerBegin(0, 80, true); //timer 0, div 80
    timerAttachInterrupt(timer, &resetModule, true);
    timerAlarmWrite(timer, watchdogTimeoutUs, false); //set time in us
    timerAlarmEnable(timer);                          //enable interrupt

    M5.begin();
    M5.Power.begin();
    drawM();
    delay(3000);
#ifdef MULTICHANNEL_GAS
    gas.begin(0x04);//the default I2C address of the slave is 0x04
    gas.powerOff();
    Serial.print("Firmware Version = ");
    Serial.println(gas.getVersion());
#endif

    M5.Lcd.fillScreen(TFT_BLACK);
    M5.Lcd.setCursor(0, 0);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Lcd.setTextSize(2);
    Serial.begin(115200);
    FastLED.addLeds<SK6812, M5STACK_FIRE_NEO_DATA_PIN, GRB>(leds, M5STACK_FIRE_NEO_NUM_LEDS); // GRB ordering is assumed

    // Wifi setup
    M5.Lcd.print("WiFi setup...");
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }
    M5.Lcd.println("done");
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.print(WiFi.localIP());
    feedWatchdog();
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    // sensor setup
    pMSensor = new Tomoto_HM330X();

    if (!pMSensor->begin())
    {
        Serial.println("PM sensor init failed");
        pMSensor = nullptr;
    }

    M5.Lcd.print("SCD30 setup...");
    Wire.begin();
    if (airSensor.begin() == false)
    {
        M5.Lcd.println("fail!");
        Serial.println("Air sensor not detected. Please check wiring. Freezing...");
        while (1)
            ;
    }
    airSensor.setMeasurementInterval(5);
    airSensor.setAltitudeCompensation(50);
    airSensor.setTemperatureOffset(0);
    airSensor.setAutoSelfCalibration(true);
    float offset = airSensor.getTemperatureOffset();

    Serial.print("Current temp offset: ");
    Serial.print(offset, 2);
    Serial.println("C");
    M5.Lcd.println("done");
    delay(1000);

    M5.Lcd.fillScreen(TFT_BLACK);
    graph_co2.setColorDepth(8);
    graph_co2.createSprite(SPRITE_WIDTH, SPRITE_HEIGHT);
    graph_co2.fillSprite(TFT_BLACK);
    p_cau = getPositionY(CO2_CAUTION_PPM);
    p_war = getPositionY(CO2_WARNING_PPM);
    graph_co2.fillRect(0, 0, SPRITE_WIDTH, p_cau + 1, getColor(50, 50, 0));
    graph_co2.fillRect(0, 0, SPRITE_WIDTH, p_war + 1, getColor(50, 0, 0));

    spr_values.setColorDepth(8);
    spr_values.createSprite(SPRITE_WIDTH, TFT_HEIGHT - SPRITE_HEIGHT);
    spr_values.fillSprite(TFT_BLACK);

    xTaskCreatePinnedToCore(
        &updateLedTask,
        "updateLedTask",
        1000,
        NULL,
        1,
        &Task1,
        1);
}

void printValue(const char *label, int value)
{
    Serial.print(label);
    Serial.print(": ");
    Serial.println(value);
}

void printPMSensor()
{
    if (pMSensor != NULL && !pMSensor->readSensor())
    {
        Serial.println("Failed to read HM330X");
    }
    else
    {
        printValue("Sensor number", pMSensor->getSensorNumber());

        Serial.println("Concentration based on CF=1 standard particlate matter (ug/m^3) --");
        printValue("PM1.0", pMSensor->std.getPM1());
        printValue("PM2.5", pMSensor->std.getPM2_5());
        printValue("PM10", pMSensor->std.getPM10());

        Serial.println("Concentration based on atmospheric environment (ug/m^3) --");
        printValue("PM1.0", pMSensor->atm.getPM1());
        printValue("PM2.5", pMSensor->atm.getPM2_5());
        printValue("PM10", pMSensor->atm.getPM10());

        // Maybe supported or not, depending on the sensor model
        Serial.println("Number of particles with diameter of (/0.1L) --");
        printValue(">=0.3um", pMSensor->count.get0_3());
        printValue(">=0.5um", pMSensor->count.get0_5());
        printValue(">=1.0um", pMSensor->count.get1());
        printValue(">=2.5um", pMSensor->count.get2_5());
        printValue(">=5.0um", pMSensor->count.get5());
        printValue(">=10um", pMSensor->count.get10());

        Serial.println();
    }
}

void updateDisplay()
{
    static int y_prev = 0;
    // write value
    spr_values.setCursor(0, 0);
    spr_values.setTextSize(3);
    spr_values.fillSprite(TFT_BLACK);
    if (co2_ppm < CO2_CAUTION_PPM)
        spr_values.setTextColor(TFT_WHITE);
    else if (co2_ppm < CO2_WARNING_PPM)
        spr_values.setTextColor(TFT_YELLOW);
    else
        spr_values.setTextColor(TFT_RED);
    spr_values.printf("CO2:  %4d ppm\n", co2_ppm);
    spr_values.setTextSize(2);
    spr_values.setTextColor(TFT_WHITE);
    spr_values.printf("T:%4.1f  H:%4.1f%%\n", temperature_c, humidity_p);
    struct tm timeinfo;
    if (getLocalTime(&timeinfo))
    {
        spr_values.printf("Time: %d:%d\n", timeinfo.tm_hour, timeinfo.tm_min);
    }

    // draw lines
    graph_co2.drawFastVLine(SPRITE_WIDTH - 1, 0, p_war + 1, getColor(50, 0, 0));
    graph_co2.drawFastVLine(SPRITE_WIDTH - 1, p_war + 1, p_cau - p_war, getColor(50, 50, 0));

    // draw co2
    int32_t y = getPositionY(co2_ppm);
    if (y > y_prev)
        graph_co2.drawFastVLine(SPRITE_WIDTH - 1, y_prev, abs(y - y_prev) + 1, TFT_WHITE);
    else
        graph_co2.drawFastVLine(SPRITE_WIDTH - 1, y, abs(y - y_prev) + 1, TFT_WHITE);
    y_prev = y;

    // update
    spr_values.pushSprite(0, 0);
    graph_co2.pushSprite(0, TFT_HEIGHT - SPRITE_HEIGHT);
    graph_co2.scroll(-1, 0);
}


void doDweet()
{
    StaticJsonDocument<512> doc;
    
    doc["co2_ppm"] = airSensor.getCO2();
    doc["humidity_pct"] = airSensor.getHumidity();
    doc["temp_DegC"] = airSensor.getTemperature();

    doc["PM1"] = pMSensor->std.getPM1();
    doc["PM2_5"] = pMSensor->std.getPM2_5();
    doc["PM10"] = pMSensor->std.getPM10();
    

    struct tm timeinfo;
    if (getLocalTime(&timeinfo))
    {
        doc["local-timestamp"] =  String(timeinfo.tm_hour) + String(":")+ String(timeinfo.tm_min) + String(":")+ String(timeinfo.tm_sec);
    }

    String output;
    serializeJson(doc, output);

    WiFiClient client;

    HTTPClient http;
    String base="http://dweet.io:80/dweet/for/";
    
    http.begin(base+dweetThingName);
    http.addHeader("X-DWEED-AUTH",X_DWEED_AUTH);
    http.setUserAgent("net.kgbvax.co2ampelard");
    http.addHeader("accept","application/json");
    http.addHeader("content-type","application/json");
    http.POST(output.c_str());
}

void printGasSensor(){
     float c;
    c = gas.measure_NH3();
    Serial.print("The concentration of NH3 is ");
    if(c>=0) Serial.print(c);
    else Serial.print("invalid");
    Serial.println(" ppm");
 
    c = gas.measure_CO();
    Serial.print("The concentration of CO is ");
    if(c>=0) Serial.print(c);
    else Serial.print("invalid");
    Serial.println(" ppm");
 
    c = gas.measure_NO2();
    Serial.print("The concentration of NO2 is ");
    if(c>=0) Serial.print(c);
    else Serial.print("invalid");
    Serial.println(" ppm");
 
    c = gas.measure_C3H8();
    Serial.print("The concentration of C3H8 is ");
    if(c>=0) Serial.print(c);
    else Serial.print("invalid");
    Serial.println(" ppm");
 
    c = gas.measure_C4H10();
    Serial.print("The concentration of C4H10 is ");
    if(c>=0) Serial.print(c);
    else Serial.print("invalid");
    Serial.println(" ppm");
 
    c = gas.measure_CH4();
    Serial.print("The concentration of CH4 is ");
    if(c>=0) Serial.print(c);
    else Serial.print("invalid");
    Serial.println(" ppm");
 
    c = gas.measure_H2();
    Serial.print("The concentration of H2 is ");
    if(c>=0) Serial.print(c);
    else Serial.print("invalid");
    Serial.println(" ppm");
 
    c = gas.measure_C2H5OH();
    Serial.print("The concentration of C2H5OH is ");
    if(c>=0) Serial.print(c);
    else Serial.print("invalid");
    Serial.println(" ppm");
}
void loop()
{
    if (airSensor.dataAvailable())
    {
        // get sensor data
        co2_ppm = airSensor.getCO2();
        Serial.print("co2(ppm):");
        Serial.print(co2_ppm);

        temperature_c = airSensor.getTemperature();
        Serial.print(" temp(C):");
        Serial.print(temperature_c, 1);

        humidity_p = airSensor.getHumidity();
        Serial.print(" humidity(%):");
        Serial.print(humidity_p, 1);

        Serial.println();
        updateDisplay();

        // check co2 level
        if (co2_ppm < CO2_CAUTION_PPM)
            co2_level_now = LEVEL_NORMAL;
        else if (co2_ppm < CO2_WARNING_PPM)
            co2_level_now = LEVEL_CAUTION;
        else
            co2_level_now = LEVEL_WARNING;
    }
    else
        Serial.print(".");
 //   printGasSensor();    
    printPMSensor();
    feedWatchdog();
    doDweet();
    delay(loopDelay);
}
