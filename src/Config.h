// Configuration for RepRapWiFi

#ifndef CONFIG_H_INCLUDED
#define CONFIG_H_INCLUDED

#define NO_WIFI_SLEEP	0

#define VERSION_MAIN	"1.21"

#if NO_WIFI_SLEEP
#define VERSION_SLEEP	"-nosleep"
#else
#define VERSION_SLEEP	""
#endif

#ifdef DEBUG
#define VERSION_DEBUG	"-D"
#else
#define VERSION_DEBUG	""
#endif

const char* const firmwareVersion = VERSION_MAIN VERSION_DEBUG VERSION_SLEEP;

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

const uint8_t Backlog = 8;

#define ARRAY_SIZE(_x) (sizeof(_x)/sizeof((_x)[0]))

#ifdef DEBUG
#define debugPrint(_str)			printf("%s(%d): %s", __FILE__, __LINE__, _str)
#define debugPrintf(_format, ...)	printf("%s(%d): ", __FILE__, __LINE__); printf(_format, __VA_ARGS__)
#else
#define debugPrint(_format)			do {} while(false)
#define debugPrintf(_format, ...)	do {} while(false)
#endif

#define debugPrintAlways(_str)			printf("%s(%d): %s", __FILE__, __LINE__, _str)
#define debugPrintfAlways(_format, ...)	printf("%s(%d): ", __FILE__, __LINE__); printf(_format, __VA_ARGS__)

#endif
