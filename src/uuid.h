// Library for generating a UUID4 on ESP32
// Mike Abbott - 2019
// MIT License

#ifndef UUID_GEN
#define UUID_GEN
// For a 32 bit int, returnVar must be of length 8 or greater.
void IntToHex(const unsigned int inInt, char *returnVar);

// returnUUID must be of length 37 or greater
// (For the null terminator)
void UUIDGen(char *returnUUID);

String StringUUIDGen();

#endif