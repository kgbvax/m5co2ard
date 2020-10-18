#define CONFIG_ESP_INT_WDT y

/*
 Interrupt based watchdog, uses HW timer 3 
 */

/* initialize the WDT, should be the _first_ instruction in setup()
   please avoid timeOutSec > 30 sec */
void setupWdt(short timeOutSec,const char* appName);

void feedWatchdog();

#define REBOOTWDT(reason) rebootWdt(__FILE__,__LINE__,reason)
void rebootWdt(const char* file,  const int line, const char* reason);

