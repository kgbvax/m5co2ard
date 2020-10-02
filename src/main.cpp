#include <Wire.h>
#include <M5Stack.h>
#include "time.h"

#include "SparkFun_SCD30_Arduino_Library.h"
#include <Tomoto_HM330X.h>

#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "envs.h"

#include <FastLED.h>
#include <ArduinoJson.h>
#include "dweet.h"

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
const char *dweetThingName = "ottermuehle-co2ampel";

const int watchdogTimeoutUs = 9000000; //in usec
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

// CO2 level setting
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
    StaticJsonDocument<400> doc;
    doc["co2_ppm"] = airSensor.getCO2();

    String output;
    serializeJson(doc, output);

    WiFiClientSecure secureClient;

    Dweet *d = new Dweet(secureClient);
    d->dweet(dweetThingName, output.c_str());
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
    printPMSensor();
    feedWatchdog();
    doDweet();
    delay(loopDelay);
}
