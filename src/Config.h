// Configuration for RepRapWiFi

#ifndef CONFIG_H_INCLUDED
#define CONFIG_H_INCLUDED

const char* const firmwareVersion = "1.20beta2";

// Define the maximum length (bytes) of file upload data per SPI packet. Use a multiple of the SD card file or cluster size for efficiency.
// ************ This must be kept in step with the corresponding value in RepRapFirmwareWiFi *************
const uint32_t maxSpiFileData = 2048;

// Define the SPI clock frequency
// The SAM occasionally transmits incorrect data at 40MHz, so we now use 26.7MHz.
const uint32_t spiFrequency = 27000000;     // This will get rounded down to 80MHz/3

// Pin numbers
const int SamSSPin = 15;          // GPIO15, output to SAM, SS pin for SPI transfer
const int EspReqTransferPin = 0;  // GPIO0, output, indicates to the SAM that we want to send something
const int SamTfrReadyPin = 4;     // GPIO4, input, indicates that SAM is ready to execute an SPI transaction

#define ARRAY_SIZE(_x) (sizeof(_x)/sizeof((_x)[0]))

#ifdef DEBUG
#define debugPrint(_s)		Serial.print(_s)
#define debugPrintln(_s)	Serial.println(_s)
#else
#define debugPrint(_s)		do {} while(false)
#define debugPrintln(_s)	do {} while(false)
#endif

#endif
