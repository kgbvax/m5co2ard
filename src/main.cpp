#define AUDIO 1
#define SUPPORT_PM330x 1
#undef SUPPORT_MULTICHANNELGAS
#define CORE_DEBUG_LEVEL 5

//Europe/Berlin
#ifndef TZ
#define TZ "CET-1CEST,M3.5.0,M10.5.0/3"
#endif

#ifdef ARDUINO_ARCH_ESP32
#include "esp32-hal-log.h"
#endif

#include <Wire.h>

#define ESP32_PARALLEL yes
#define LGFX_AUTODETECT

#include <M5Stack.h>
#include <LovyanGFX.hpp>

#include "time.h"

//I/O
#include "SparkFun_SCD30_Arduino_Library.h"

#ifdef SUPPORT_PM330x
#undef DEFAULT_I2C_ADDR
#include <Tomoto_HM330X.h>
#endif

#include <FastLED.h>

//optional audio
#ifdef AUDIO
#include "SPIFFS.h"
#include "AudioFileSourceSPIFFS.h"
#include "AudioFileSourceID3.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

AudioGeneratorMP3 *mp3;
AudioFileSourceSPIFFS *file;
AudioOutputI2S *out;
AudioFileSourceID3 *id3;
#endif

#include "wdt.h"
#include "iolog.h"

//networking
#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>

//not in SCM, provide your own defining WIFI_SSID, WIFI_PASSWORD
//and INFLUXDB_URL,INFLUXDB_TOKEN,INFLUXDB_ORG,INFLUXDB_BUCKET
#include "secrets.h"
#include "Version.h"

//Include also InfluxClould 2 CA certificate
#include <InfluxDbCloud.h>
#include <InfluxDbClient.h>
static int influxFailures=-5;


//actual definition included from secrets.h

#ifdef SUPPORT_PM330x
Tomoto_HM330X *pMSensor;
#endif

//M5Stack onboard leds
#define M5STACK_FIRE_NEO_NUM_LEDS 10
#define M5STACK_FIRE_NEO_DATA_PIN 15
CRGB leds[M5STACK_FIRE_NEO_NUM_LEDS];
TaskHandle_t ledUpdateTaskH;

#define USE_WIFI
const char *ssid{WIFI_SSID};         // write your WiFi SSID (2.4GHz)
const char *password{WIFI_PASSWORD}; // write your WiFi password

const int watchdogTimeoutUs = 15 * 1000 * 1000; //in usec
const int loopDelay = 7 * 1000;                 //main loop() delay in msec


// sensor setting
SCD30 airSensor;

// TFT setting
#define SPRITE_WIDTH 320
#define SPRITE_HEIGHT 160

// CO2 limits/setting
#define CO2_MIN_PPM 0
#define CO2_MAX_PPM 2500
#define CO2_RANGE_INT 500
#define CO2_CAUTION_PPM 900
#define CO2_WARNING_PPM 1000
#define CO2_WARNING2_PPM 1500

enum
{
    LEVEL_NORMAL,
    LEVEL_CAUTION,
    LEVEL_WARNING,
    LEVEL_WARNING2
};

// variables

//TFT_eSprite graph_co2 = TFT_eSprite(&M5.Lcd);
TFT_eSprite spr_values = TFT_eSprite(&M5.Lcd);
TFT_eSprite theM = TFT_eSprite(&M5.Lcd);

uint16_t co2_ppm;
int p_cau, p_war;
float temperature_c, humidity_p;
int elapsed_time = 0;
int co2_level_last = LEVEL_NORMAL;
int co2_level_now = LEVEL_NORMAL;
String macStr;

InfluxDBClient client(INFLUXDB_URL, INFLUXDB_ORG, INFLUXDB_BUCKET, INFLUXDB_TOKEN, InfluxDbCloud2CACert);
Point influxSensor = Point("co2ard");

#define TRIANGLE(x1, y1, x2, y2, x3, y3, col) M5.Lcd.drawTriangle(pad + (x1), pad + (y1), pad + (x2), pad + (y2), pad + (x3), pad + (y3), col)
//#define TRIFILLED(x1, y1, x2, y2, x3, y3, col) M5.Lcd.drawTriangle(pad + (x1), pad + (y1), pad + (x2), pad + (y2), pad + (x3), pad + (y3), col)

void drawM(int pad, u16_t color)
{

    const int my = /* M5.Lcd.height() */ 240 - pad * 2;
    const int mx = /* M5.Lcd.width()*/ 320 - pad * 2;

    TRIANGLE(0, my / 2, mx / 2, my / 2, mx / 4, 0, color);                          // ▲ Links
    TRIANGLE(mx / 2, my / 2, mx / 2 + mx / 4, 0, mx, my / 2, color);                // ▲ Rechts
    TRIANGLE(mx / 4, my / 2, mx / 2, my, mx - (mx / 4), my / 2, color);             // ▼
    TRIANGLE(0, my / 2, mx / 8, my - my / 4, mx / 4, my / 2, color);                // ▾ Links
    TRIANGLE(mx - (mx / 4), my / 2, mx, my / 2, mx - (mx / 8), my - my / 4, color); // ▾ Rechts
}

void disp(const String msg)
{
    log_i("%s", msg);
    M5.Lcd.println(msg);
}

void displaySwStats()
{
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(0, 0, 2);
    M5.Lcd.setTextSize(1);
    esp_chip_info_t ci;
    esp_chip_info(&ci);

    disp("core " + String(xPortGetCoreID()));

    M5.Lcd.setTextSize(2);
    disp("MAC: " + macStr);
    disp("Version: " + String(VERSION));
    M5.Lcd.setTextSize(1);
    disp("MD5: " + EspClass().getSketchMD5());
    disp("Build timestamp: " + String(BUILD_TIMESTAMP));
    disp("configured SSID: " + String(ssid));
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
    
    boolean left = false;
 
    for (;;)
    {
        left = !left;
        
        if (co2_level_now == LEVEL_NORMAL)
        {
            for (uint8_t n = 0; n < M5STACK_FIRE_NEO_NUM_LEDS; n++)
            {
                leds[n] = CRGB::DarkGreen;
            }
            FastLED.setBrightness(5);
            M5.Lcd.setBrightness(80);
        }
        else if (co2_level_now == LEVEL_CAUTION)
        {
            for (uint8_t n = 0; n < M5STACK_FIRE_NEO_NUM_LEDS; n++)
            {
                leds[n] = CRGB::Yellow;
            }
            FastLED.setBrightness(20);
            M5.Lcd.setBrightness(160);
        }
        else if (co2_level_now == LEVEL_WARNING)
        {
            for (uint8_t n = 0; n < M5STACK_FIRE_NEO_NUM_LEDS; n++)
            {
                leds[n] = CRGB::Red;
            }
            FastLED.setBrightness(255);
            M5.Lcd.setBrightness(255);
        }
        else if (co2_level_now == LEVEL_WARNING2)
        {
            FastLED.setBrightness(255);
            M5.Lcd.setBrightness(255);
            for (uint8_t n = 0; n < M5STACK_FIRE_NEO_NUM_LEDS; n++)
            {
                if (left)
                {
                    leds[n] = CRGB::Violet;
                }
                else
                {
                    leds[n] = CRGB::Red;
                }
            }
        }
        else
        {
            for (uint8_t n = 0; n < M5STACK_FIRE_NEO_NUM_LEDS; n++)
            {
                leds[n] = CRGB::DarkGray;
            }
        }
        if(left) 
            leds[0]=CRGB::Black; 
        else
            leds[0]=CRGB::DarkBlue;

        FastLED.show();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void meepmeep()
{
#ifdef AUDIO
    SPIFFS.begin();

    file = new AudioFileSourceSPIFFS("/meepmeep.mp3");
    if (file && file->isOpen())
    {
        id3 = new AudioFileSourceID3(file);
        out = new AudioOutputI2S(0, 1); // Output to builtInDAC
        out->SetOutputModeMono(true);

        //out->SetGain(0.4f);
        mp3 = new AudioGeneratorMP3();
        mp3->begin(id3, out);

        log_d("mp3 play start");
        while (mp3->isRunning())
        {
            if (!mp3->loop())
                mp3->stop();
        }
    }
    log_d("mp3 play stop");
    file->close();
    SPIFFS.end();

#endif
}

 

void setup()
{
    Serial.begin(115200);

    setupWdt(60,"m5co2ard");

    M5.begin(true,false,true,true);
 
    int prev = -1;
    for (int pad = 90; pad > 4; pad -= 4)
    { //make some show for philip
        if (prev >= 0)
        {
            //drawM(prev, BLACK);
            M5.Lcd.clear(BLACK);
        }
        drawM(pad, WHITE);
        delay(35);
        prev = pad;
    }

    feedWatchdog();

    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    char macBuf[20]; //xx-xx-xx-xx-xx-xx
    sprintf(macBuf, "%x-%x-%x-%x-%x-%x", (unsigned int)mac[0], (unsigned int)mac[1], (unsigned int)mac[2], (unsigned int)mac[3], (unsigned int)mac[4], (unsigned int)mac[5]);
    macStr = String(macBuf);

    displaySwStats();
    // meepmeep();
    feedWatchdog();

    delay(3000);

    M5.Lcd.fillScreen(TFT_BLACK);
    M5.Lcd.setCursor(0, 0);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Lcd.setTextSize(2);
    FastLED.addLeds<SK6812, M5STACK_FIRE_NEO_DATA_PIN, GRB>(leds, M5STACK_FIRE_NEO_NUM_LEDS); // GRB ordering is assumed
    feedWatchdog();
    // Wifi setup
    M5.Lcd.print("WiFi setup...");
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        feedWatchdog();
        LOG_D(".","");
    }
    M5.Lcd.println("done");
    LOG_D("WiFi connected","");
    LOG_I("IP address: %s", WiFi.localIP().toString().c_str());

    feedWatchdog();
    delay(500);

    //configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    timeSync(TZ, "europe.pool.ntp.org", "pool.ntp.org", "time.nis.gov");
    if (!client.validateConnection())
    {
        REBOOTWDT("Cant connect InfluxDB");
    }
    

    influxSensor.addTag("mac", macStr);
    influxSensor.addTag("SSID", WiFi.SSID());
    influxSensor.addTag("version", VERSION);

    if (!Wire.begin())
    {
        REBOOTWDT("i2c init failed");
    }
#ifdef SUPPORT_PM330x
    // sensor setup
    pMSensor = new Tomoto_HM330X();

    if (!pMSensor->begin())
    {
        LOG_W("No HM330X PM sensor detected.","");
        pMSensor = nullptr;
    }
#endif

    M5.Lcd.print("SCD30 setup...");
    if (airSensor.begin() == false)
    {
        REBOOTWDT("Air sensor not detected.");
    }
    airSensor.setMeasurementInterval(5);
    airSensor.setAltitudeCompensation(50);

    airSensor.setTemperatureOffset(0);
    airSensor.setAutoSelfCalibration(true);

    M5.Lcd.fillScreen(TFT_BLACK);
    //graph_co2.setColorDepth(8);
    //graph_co2.createSprite(SPRITE_WIDTH, SPRITE_HEIGHT);
    //graph_co2.fillSprite(TFT_BLACK);
    //p_cau = getPositionY(CO2_CAUTION_PPM);
    //p_war = getPositionY(CO2_WARNING_PPM);
    //graph_co2.fillRect(0, 0, SPRITE_WIDTH, p_cau + 1, getColor(50, 50, 0));
    //graph_co2.fillRect(0, 0, SPRITE_WIDTH, p_war + 1, getColor(50, 0, 0));

    spr_values.setColorDepth(8);
    spr_values.createSprite(SPRITE_WIDTH, TFT_HEIGHT - SPRITE_HEIGHT);
    spr_values.fillSprite(TFT_BLACK);

    xTaskCreatePinnedToCore(
        &updateLedTask,
        "updateLedTask",
        1000,
        NULL,
        1,
        &ledUpdateTaskH,
        0);
}

void printValue(const char *label, int value)
{
    LOG_I("%s: %s\n",label,value);

}

#ifdef SUPPORT_PM330x
void printPMSensor()
{
    if (pMSensor != NULL)
    {

        log_d("Concentration based on CF=1 standard particlate matter (ug/m^3) --");
        printValue("PM1.0", pMSensor->std.getPM1());
        printValue("PM2.5", pMSensor->std.getPM2_5());
        printValue("PM10", pMSensor->std.getPM10());

        log_d("Concentration based on atmospheric environment (ug/m^3) --");
        printValue("PM1.0", pMSensor->atm.getPM1());
        printValue("PM2.5", pMSensor->atm.getPM2_5());
        printValue("PM10", pMSensor->atm.getPM10());

        // Maybe supported or not, depending on the sensor model
        log_d("Number of particles with diameter of (/0.1L) --");
        printValue(">=0.3um", pMSensor->count.get0_3());
        printValue(">=0.5um", pMSensor->count.get0_5());
        printValue(">=1.0um", pMSensor->count.get1());
        printValue(">=2.5um", pMSensor->count.get2_5());
        printValue(">=5.0um", pMSensor->count.get5());
        printValue(">=10um", pMSensor->count.get10());

    }
}
#endif

void updateDisplay()
{
    static int y_prev = 0;
    // write value
    spr_values.setCursor(0, 0);
    spr_values.setTextSize(4);
    spr_values.fillSprite(TFT_BLACK);
    if (co2_ppm < CO2_CAUTION_PPM)
        spr_values.setTextColor(TFT_WHITE);
    else if (co2_ppm < CO2_WARNING_PPM)
        spr_values.setTextColor(TFT_YELLOW);
    else
        spr_values.setTextColor(TFT_RED);
    spr_values.printf("CO2: %4d ppm\n", co2_ppm);
    spr_values.setTextSize(2);
    spr_values.setTextColor(TFT_WHITE);
    spr_values.printf("T:%4.1f  H:%4.1f%%\n", temperature_c, humidity_p);
    struct tm timeinfo;
    if (getLocalTime(&timeinfo))
    {
        spr_values.printf("Time: %d:%d\n", timeinfo.tm_hour, timeinfo.tm_min);
    }

    spr_values.pushSprite(0, 0);
 }

void loop()
{
    //loopCnt++;
    feedWatchdog();
    if (airSensor.dataAvailable())
    {
        // get sensor data
        co2_ppm = airSensor.getCO2();
        LOG_D("co2(ppm): %d ", co2_ppm);

        temperature_c = airSensor.getTemperature();
        LOG_D(" temp(C): %f ", temperature_c);

        humidity_p = airSensor.getHumidity();
        LOG_D(" humidity(%): %f ", humidity_p);

        updateDisplay();

        // check co2 level
        if (co2_ppm < CO2_CAUTION_PPM)
            co2_level_now = LEVEL_NORMAL;
        else if (co2_ppm < CO2_WARNING_PPM)
            co2_level_now = LEVEL_CAUTION;
        else if (co2_ppm < CO2_WARNING2_PPM)
            co2_level_now = LEVEL_WARNING;
        else
            co2_level_now = LEVEL_WARNING2;

        influxSensor.clearFields();
        influxSensor.addField("uptime", millis());
        influxSensor.addField("rssi", WiFi.RSSI());
        influxSensor.addField("co2", co2_ppm);
        influxSensor.addField("temp", temperature_c);
        influxSensor.addField("humidity", humidity_p);
#ifdef SUPPORT_PM330x
        if (pMSensor != NULL)
        {
            if (!pMSensor->readSensor())
            {
                LOG_E("failed to read sensor","");
            }
            else
            {
                influxSensor.addField("PM1", pMSensor->std.getPM1());
                influxSensor.addField("PM2.5", pMSensor->std.getPM2_5());
                influxSensor.addField("PM10", pMSensor->std.getPM10());
                influxSensor.addField("PtCnt0.3", pMSensor->count.get0_3());
                influxSensor.addField("PtCnt0.5", pMSensor->count.get0_5());
                influxSensor.addField("PtCnt1.0", pMSensor->count.get1());
                influxSensor.addField("PtCnt2.5", pMSensor->count.get2_5());
                influxSensor.addField("PtCnt5.0", pMSensor->count.get5());
                influxSensor.addField("PtCnt10", pMSensor->count.get10());
                // printPMSensor();
            }
        }
#endif
        if (1)
        {
            //loopCnt = 0;
            if (!client.writePoint(influxSensor))
            {
                LOG_E("InfluxDB write failed: %s %d", client.getLastErrorMessage(), client.getLastStatusCode());
                influxFailures++;
                if (influxFailures>0) {
                    REBOOTWDT("repeated influx fail");
                }
            }
            else
            {
                LOG_D("uploaded to influxdb\n","");
            }
        }
    }
    else
    {
        LOG_V(".","");
    }

    if (co2_level_now == LEVEL_WARNING2)
    {
        meepmeep();
    }
    feedWatchdog();

    delay(loopDelay);
}
