// NimBLEWrapper.h
#ifndef NIMBLE_WRAPPER_H
#define NIMBLE_WRAPPER_H

#ifdef __cplusplus
extern "C" {
#endif

void setMyDeviceByAddress(const char* address);
void handleUartCommunicationWrapper(const char* input);
_Bool wrapperNormalizeRTCValue(const char* rtcValue, char* normalizedRTC);
int validate_and_format_value(const char* value, char* output);

void rescan();
#ifdef __cplusplus
}
#endif

#endif // NIMBLE_WRAPPER_H