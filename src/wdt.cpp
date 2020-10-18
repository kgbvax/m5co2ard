#include "esp_system.h"
#include "esp32-hal-timer.h"
#include <Preferences.h>

#include "wdt.h"
 


static hw_timer_t *wdtTimer = NULL;

static Preferences wdtPreferences;
const char* resetReasonKey= "resetReason";

void IRAM_ATTR wdtFailed() {
    REBOOTWDT("WDR expired");
}

void IRAM_ATTR rebootWdt(const char* file,  const int line, const char* reason)
{
    ets_printf("reset requested\n");
    if (reason!=NULL) {
        wdtPreferences.putString(resetReasonKey,reason);
        log_e("about to reset: %s", reason);
        ets_printf("reset reason %s\n",reason);
    }
    wdtPreferences.end();
    delay(250);//maybe busy wait better choice?
    esp_restart();
}

 String getResetReason() {
    String empty("");
    String val=wdtPreferences.getString(resetReasonKey,empty);
    return val;
}

void setupWdt(short timeOutSec,const char* appName) {

      //setup watchdog early to allow trigger even during setup
    wdtTimer = timerBegin(3 , 80, true); //timer 0, div 80
    timerAttachInterrupt(wdtTimer, &wdtFailed, true);
    timerAlarmWrite(wdtTimer, timeOutSec*1000000, false); //set time in us
    timerAlarmEnable(wdtTimer);    
    wdtPreferences.begin(appName, false);
}

void feedWatchdog()
{
    if(wdtTimer!=NULL) {
        timerWrite(wdtTimer, 0); //reset timer (feed watchdog)
    } else {
        ets_printf("watchdog not initialized.");
    }
}
