// sinve all the logging seems borken, here is a macro that allows tinerking
#ifndef iolog_h
#define iolog_h

#define LOG_V(format, ... )  Serial.printf(format, __VA_ARGS__)
#define LOG_D(format, ... )  Serial.printf(format, __VA_ARGS__)
#define LOG_W(format, ... )  Serial.printf(format, __VA_ARGS__)
#define LOG_I(format, ... )  Serial.printf(format, __VA_ARGS__)
#define LOG_E(format, ... )  Serial.printf(format, __VA_ARGS__)

#endif
