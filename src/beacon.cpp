
#include "sys/time.h"
#include "BLEDevice.h"
#include "BLEUtils.h"
#include "BLEServer.h"
#include "BLEBeacon.h"
#include "esp_sleep.h"

#define GPIO_DEEP_SLEEP_DURATION 10      // sleep x seconds and then wake up
//RTC_DATA_ATTR static time_t last;        // remember last boot in RTC Memory
RTC_DATA_ATTR static uint32_t bootcount; // remember number of boots in RTC Memory

// See the following for generating UUIDs:
// https://www.uuidgenerator.net/
BLEAdvertising *pAdvertising; // BLE Advertisement type
struct timeval now;

#define BEACON_UUID "1EA69257-1327-404D-99C7-439DC76BC315" // UUID 1 128-Bit (may use linux tool uuidgen or random numbers via https://www.uuidgenerator.net/)


void startBeacon()
{
    // Create the BLE Device
    BLEDevice::init("ESP32 as iBeacon");
    // Create the BLE Server
    //BLEServer *pServer = BLEDevice::createServer(); // <-- no longer required to instantiate BLEServer, less flash and ram usage
    //pAdvertising = BLEDevice::getAdvertising();
    BLEDevice::startAdvertising();
        
    BLEBeacon oBeacon = BLEBeacon();
    oBeacon.setManufacturerId(0x4C00); // fake Apple 0x004C LSB (ENDIAN_CHANGE_U16!)
    oBeacon.setProximityUUID(BLEUUID(BEACON_UUID));
    oBeacon.setMajor((bootcount & 0xFFFF0000) >> 16);
    oBeacon.setMinor(bootcount & 0xFFFF);
    BLEAdvertisementData oAdvertisementData = BLEAdvertisementData();
    BLEAdvertisementData oScanResponseData = BLEAdvertisementData();

    oAdvertisementData.setFlags(0x04); // BR_EDR_NOT_SUPPORTED 0x04

    std::string strServiceData = "";

    strServiceData += (char)26;   // Len
    strServiceData += (char)0xFF; // Type
    strServiceData += oBeacon.getData();
    oAdvertisementData.addData(strServiceData);

    pAdvertising->setAdvertisementData(oAdvertisementData);
    pAdvertising->setScanResponseData(oScanResponseData);

    // Start advertising
    pAdvertising->start();
    //delay(100);
    //pAdvertising->stop();
    //Serial.printf("enter deep sleep\n");
    //esp_deep_sleep(1000000LL * GPIO_DEEP_SLEEP_DURATION);
    //Serial.printf("in deep sleep\n");
}