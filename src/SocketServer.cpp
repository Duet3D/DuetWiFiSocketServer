/*
 * SocketServer.cpp
 *
 *  Created on: 25 Mar 2017
 *      Author: David
 */

#include "ecv.h"
#undef yield
#undef array

#include <cstdarg>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include "SocketServer.h"
#include "Config.h"
#include "PooledStrings.h"
#include "HSPI.h"

#include "include/MessageFormats.h"
#include "Connection.h"
#include "Listener.h"

extern "C"
{
	#include "user_interface.h"     // for struct rst_info
	#include "lwip/stats.h"			// for stats_display()
	#include "lwip/app/netbios.h"	// for NetBIOS support
}

#define array _ecv_array

const uint32_t MaxConnectTime = 30 * 1000;		// how long we wait for WiFi to connect in milliseconds

const int DefaultWiFiChannel = 6;

// Global data
char currentSsid[SsidLength + 1];
char webHostName[HostNameLength + 1] = "Duet-WiFi";

DNSServer dns;

const char* lastError = nullptr;
const char* prevLastError = nullptr;

char lastConnectError[100];

WiFiState currentState = WiFiState::idle;

ADC_MODE(ADC_VCC);          // need this for the ESP.getVcc() call to work

static HSPIClass hspi;
static uint32_t connectStartTime;

static uint32_t transferBuffer[NumDwords(MaxDataLength + 1)];

// Look up a SSID in our remembered network list, return true if found
// This always overwrites ssidData
bool RetrieveSsidData(const char *ssid, WirelessConfigurationData& ssidData, size_t *index = nullptr)
{
	for (size_t i = 1; i <= MaxRememberedNetworks; ++i)
	{
		EEPROM.get(i * sizeof(WirelessConfigurationData), ssidData);
		if (strncmp(ssid, ssidData.ssid, sizeof(ssidData.ssid)) == 0)
		{
			if (index != nullptr)
			{
				*index = i;
			}
			return true;
		}
	}
	memset(&ssidData, 0, sizeof(ssidData));			// clear the last password out of RAM for security
	return false;
}

// Find an empty entry in the table of known networks
bool FindEmptySsidEntry(size_t *index)
{
	for (size_t i = 1; i <= MaxRememberedNetworks; ++i)
	{
		WirelessConfigurationData tempData;
		EEPROM.get(i * sizeof(WirelessConfigurationData), tempData);
		if (tempData.ssid[0] == 0xFF)
		{
			*index = i;
			return true;
		}
	}
	return false;
}

// Check socket number in range, returning trus if yes. Otherwise, set lastError and return false;
bool ValidSocketNumber(uint8_t num)
{
	if (num < NumTcpSockets)
	{
		return true;
	}
	lastError = "socket number out of range";
	return false;
}

// Reset to default settings
void FactoryReset()
{
	WirelessConfigurationData temp;
	memset(&temp, 0xFF, sizeof(temp));
	for (size_t i = 0; i <= MaxRememberedNetworks; ++i)
	{
		EEPROM.put(i * sizeof(WirelessConfigurationData), temp);
	}
	EEPROM.commit();
}

// Try to connect using the saved SSID and password, returning true if successful
void ConnectToAccessPoint(const WirelessConfigurationData& apData)
pre(currentState == NetworkState::disabled)
{
	strncpy(currentSsid, apData.ssid, ARRAY_SIZE(currentSsid) - 1);
	currentSsid[ARRAY_SIZE(currentSsid) - 1] = 0;

	wifi_station_set_hostname(webHostName);     	// must do this before calling WiFi.begin()
	WiFi.config(IPAddress(apData.ip), IPAddress(apData.gateway), IPAddress(apData.netmask), IPAddress(), IPAddress());
	WiFi.mode(WIFI_STA);
	WiFi.begin(apData.ssid, apData.password);

	currentState = WiFiState::connecting;
	connectStartTime = millis();
}

void ConnectPoll()
{
	if (currentState == WiFiState::connecting)
	{
		if (WiFi.status() == WL_CONNECTED)
		{
			currentState = WiFiState::connected;
			digitalWrite(EspReqTransferPin, LOW);				// force a status update when complete
			debugPrintln("Connected to AP");
		}
		else if (millis() - connectStartTime >= MaxConnectTime)
		{
			Serial.println("WIFI ERROR");
			WiFi.mode(WIFI_STA);
			WiFi.disconnect();
			delay(100);
			currentState = WiFiState::idle;
			strcpy(lastConnectError, "failed to connect to access point ");
			strncat(lastConnectError, currentSsid, ARRAY_SIZE(lastConnectError) - strlen(lastConnectError) - 1);
			digitalWrite(EspReqTransferPin, LOW);				// force a status update when complete
			debugPrintln("Connection timeout");
		}
	}
}

void StartClient(const char * array ssid)
{
	WirelessConfigurationData ssidData;

	if (ssid == nullptr || ssid[0] == 0)
	{
		// Auto scan for strongest known network, then try to connect to it
		int8_t num_ssids = WiFi.scanNetworks(false, true);
		if (num_ssids < 0)
		{
			lastError = "network scan failed";
		}

		// Find the strongest network that we know about
		int8_t strongestNetwork = -1;
		for (int8_t i = 0; i < num_ssids; ++i)
		{
			if ((strongestNetwork < 0 || WiFi.RSSI(i) > WiFi.RSSI(strongestNetwork)) && RetrieveSsidData(WiFi.SSID(i).c_str(), ssidData))
			{
				strongestNetwork = i;
			}
		}
		if (strongestNetwork < 0)
		{
			lastError = "no known networks found";
		}
	}
	else if (!RetrieveSsidData(ssid, ssidData))
	{
		lastError = "no data found for requested SSID";
	}

	ConnectToAccessPoint(ssidData);
}

bool CheckValidString(const char * array s, size_t n, bool isSsid)
{
	for (size_t i = 0; i < n; ++i)
	{
		char c = s[i];
		if (c < 0x20 || c == 0x7F)
		{
			return false;					// bad character
		}
		if (c == 0)
		{
			return i != 0 || !isSsid;		// the SSID may not be empty but the password can be
		}
	}
	return false;							// no null terminator
}

// Check that the access point data is valid
bool ValidApData(const WirelessConfigurationData &apData)
{
	// Check the IP address
	if (apData.ip == 0 || apData.ip == 0xFFFFFFFF)
	{
		return false;
	}

	// Check the channel. 0 means auto so it OK.
	if (apData.channel > 13)
	{
		return false;
	}

	return CheckValidString(apData.ssid, SsidLength, true) && CheckValidString(apData.password, PasswordLength, false);
}

void StartAccessPoint()
{
	WirelessConfigurationData apData;
	EEPROM.get(0, apData);

	if (ValidApData(apData))
	{
		WiFi.mode(WIFI_AP);
		WiFi.softAPConfig(apData.ip, apData.ip, IPAddress(255, 255, 255, 0));
		WiFi.softAP(apData.ssid, apData.password, (apData.channel == 0) ? DefaultWiFiChannel : apData.channel);
		Serial.println("WiFi -> DuetWiFi");
		dns.setErrorReplyCode(DNSReplyCode::NoError);
		dns.start(53, "*", apData.ip);
		currentState = WiFiState::runningAsAccessPoint;
		debugPrintln("AP started");
	}
	else
	{
		lastError = "invalid access point configuration";
	}
}

static union
{
	MessageHeaderSamToEsp hdr;			// the actual header
	uint32_t asDwords[headerDwords];	// to force alignment
} messageHeaderIn;

static union
{
	MessageHeaderEspToSam hdr;
	uint32_t asDwords[headerDwords];	// to force alignment
} messageHeaderOut;

// Rebuild the mDNS services
void RebuildServices()
{
	MDNS.deleteServices();

	// Unfortunately the official ESP8266 mDNS library only reports one service.
	// I (chrishamm) tried to use the old mDNS responder, which is also capable of sending
	// mDNS broadcasts, but the packets it generates are broken and thus not of use.
	const uint16_t httpPort = Listener::GetPortByProtocol(0);
	if (httpPort != 0)
	{
		MDNS.addService("http", "tcp", httpPort);
		MDNS.addServiceTxt("http", "tcp", "product", "DuetWiFi");
		MDNS.addServiceTxt("http", "tcp", "version", firmwareVersion);
	}
	else
	{
		const uint16_t ftpPort = Listener::GetPortByProtocol(1);
		if (ftpPort != 0)
		{
			MDNS.addService("ftp", "tcp", ftpPort);
			MDNS.addServiceTxt("ftp", "tcp", "product", "DuetWiFi");
			MDNS.addServiceTxt("ftp", "tcp", "version", firmwareVersion);
		}
		else
		{
			const uint16_t telnetPort = Listener::GetPortByProtocol(2);
			if (telnetPort != 0)
			{
				MDNS.addService("telnet", "tcp", telnetPort);
				MDNS.addServiceTxt("telnet", "tcp", "product", "DuetWiFi");
				MDNS.addServiceTxt("telnet", "tcp", "version", firmwareVersion);
			}
		}
	}
}

// Send a response.
// 'response' is the number of byes of response if positive, or the error code if negative.
// Use only to respond to commands which don't include a data block, or when we don't want to read the data block.
void SendResponse(int32_t response)
{
	(void)hspi.transfer32(response);
	if (response > 0)
	{
		hspi.transferDwords(transferBuffer, nullptr, NumDwords((size_t)response));
	}
}

// This is called when the SAM is asking to transfer data
void ProcessRequest()
{
	// Set up our own header
	messageHeaderOut.hdr.formatVersion = MyFormatVersion;
	messageHeaderOut.hdr.state = currentState;

	bool deferCommand = false;

	// Begin the transaction
	digitalWrite(SamSSPin, LOW);            // assert CS to SAM
	hspi.beginTransaction();

	// Exchange headers, except for the last dword which will contain our response
	hspi.transferDwords(messageHeaderOut.asDwords, messageHeaderIn.asDwords, headerDwords - 1);
	const size_t dataBufferAvailable = std::min<size_t>(messageHeaderIn.hdr.dataBufferAvailable, MaxDataLength);

	if (messageHeaderIn.hdr.formatVersion != MyFormatVersion)
	{
		SendResponse(ResponseUnknownFormat);
	}
	else if (messageHeaderIn.hdr.dataLength > MaxDataLength)
	{
		SendResponse(ResponseBadDataLength);
	}
	else
	{
		// See what command we have received and take appropriate action
		switch (messageHeaderIn.hdr.command)
		{
		case NetworkCommand::nullCommand:					// no command being sent, SAM just wants the network status
			SendResponse(ResponseEmpty);
			break;

		case NetworkCommand::networkStartClient:			// connect to an access point
		case NetworkCommand::networkStartAccessPoint:		// run as an access point
		case NetworkCommand::networkFactoryReset:			// clear remembered list, reset factory defaults
			if (currentState == WiFiState::idle)
			{
				deferCommand = true;
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
			}
			else
			{
				SendResponse(ResponseWrongState);
			}
			break;

		case NetworkCommand::networkStop:					// disconnect from an access point, or close down our own access point
			deferCommand = true;
			messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
			break;

		case NetworkCommand::networkGetStatus:				// get the network connection status
			{
				NetworkStatusResponse *response = reinterpret_cast<NetworkStatusResponse*>(transferBuffer);
				response->ipAddress = static_cast<uint32_t>(WiFi.localIP());
				response->freeHeap = ESP.getFreeHeap();
				response->resetReason = ESP.getResetInfoPtr()->reason;
				response->flashSize = ESP.getFlashChipRealSize();
				response->rssi = WiFi.RSSI();
				response->vcc = ESP.getVcc();
			    wifi_get_macaddr(STATION_IF, response->macAddress);
				strncpy(response->versionText, firmwareVersion, sizeof(response->versionText));
				strncpy(response->hostName, webHostName, sizeof(response->hostName));
				strncpy(response->ssid, currentSsid, sizeof(response->ssid));
				SendResponse(sizeof(NetworkStatusResponse));
			}
			break;

		case NetworkCommand::networkAddSsid:				// add to our known access point list
		case NetworkCommand::networkConfigureAccessPoint:	// configure our own access point details
			if (messageHeaderIn.hdr.dataLength == sizeof(WirelessConfigurationData))
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
				WirelessConfigurationData localClientData;
				hspi.transferDwords(nullptr, transferBuffer, SIZE_IN_DWORDS(WirelessConfigurationData));
				const WirelessConfigurationData * const receivedClientData = reinterpret_cast<const WirelessConfigurationData *>(transferBuffer);
				size_t index;
				bool found;
				if (messageHeaderIn.hdr.command == NetworkCommand::networkConfigureAccessPoint)
				{
					index = 0;
					found = true;
				}
				else
				{
					found = RetrieveSsidData(receivedClientData->ssid, localClientData, &index);
					if (!found)
					{
						found = FindEmptySsidEntry(&index);
					}
				}

				if (found)
				{
					EEPROM.put(index * sizeof(WirelessConfigurationData), *receivedClientData);
					EEPROM.commit();
				}
				else
				{
					lastError = "SSID table full";
				}
			}
			else
			{
				SendResponse(ResponseBadDataLength);
			}
			break;

		case NetworkCommand::networkDeleteSsid:				// delete a network from our access point list
			if (messageHeaderIn.hdr.dataLength == SsidLength)
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
				hspi.transferDwords(nullptr, transferBuffer, NumDwords(SsidLength));

				WirelessConfigurationData ssidData;
				size_t index;
				if (RetrieveSsidData(reinterpret_cast<char*>(transferBuffer), ssidData, &index))
				{
					memset(&ssidData, 0xFF, sizeof(ssidData));
					EEPROM.put(index * sizeof(WirelessConfigurationData), ssidData);
					EEPROM.commit();
				}
				else
				{
					lastError = "SSID not found";
				}
			}
			else
			{
				SendResponse(ResponseBadDataLength);
			}
			break;

		case NetworkCommand::networkListSsids:				// list the access points we know about
			{
				char *p = reinterpret_cast<char*>(transferBuffer);
				for (size_t i = 1; i <= MaxRememberedNetworks; ++i)
				{
					WirelessConfigurationData tempData;
					EEPROM.get(i * sizeof(WirelessConfigurationData), tempData);
					if (tempData.ssid[0] != 0xFF)
					{
						for (size_t j = 0; j < SsidLength && tempData.ssid[j] != 0; ++j)
						{
							*p++ = tempData.ssid[j];
						}
						*p++ = '\n';
					}
				}
				*p++ = 0;
				const size_t numBytes = p - reinterpret_cast<char*>(transferBuffer);
				if (numBytes <= dataBufferAvailable)
				{
					SendResponse(numBytes);
				}
				else
				{
					SendResponse(ResponseBufferTooSmall);
				}
			}
			break;

		case NetworkCommand::networkSetHostName:			// set the host name
			if (messageHeaderIn.hdr.dataLength == HostNameLength)
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
				hspi.transferDwords(nullptr, transferBuffer, NumDwords(HostNameLength));
				memcpy(webHostName, transferBuffer, HostNameLength);
				webHostName[HostNameLength] = 0;			// ensure null terminator

				// The following can be called multiple times
				MDNS.begin(webHostName);
			}
			else
			{
				SendResponse(ResponseBadDataLength);
			}
			break;

		case NetworkCommand::networkGetLastError:
			if (lastError == nullptr)
			{
				SendResponse(0);
			}
			else
			{
				const size_t len = strlen(lastError) + 1;
				if (dataBufferAvailable >= len)
				{
					strcpy(reinterpret_cast<char*>(transferBuffer), lastError);		// copy to 32-bit aligned buffer
					SendResponse(len);
				}
				else
				{
					SendResponse(ResponseBufferTooSmall);
				}
				lastError = nullptr;
			}
			break;

		case NetworkCommand::networkListen:				// listen for incoming connections
			if (messageHeaderIn.hdr.dataLength == sizeof(ListenOrConnectData))
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
				ListenOrConnectData lcData;
				hspi.transferDwords(nullptr, reinterpret_cast<uint32_t*>(&lcData), NumDwords(sizeof(lcData)));
				const bool ok = Listener::Listen(lcData.remoteIp, lcData.port, lcData.protocol, lcData.maxConnections);
				if (ok)
				{
					debugPrint("Listening on port ");
					debugPrintln(lcData.port);
				}
				else
				{
					lastError = "Listen failed";
					debugPrintln("Listen failed");
				}
				RebuildServices();
			}
			break;

		case NetworkCommand::connAbort:					// terminate a socket rudely
			if (ValidSocketNumber(messageHeaderIn.hdr.socketNumber))
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
				Connection::Get(messageHeaderIn.hdr.socketNumber).Terminate();
			}
			else
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseBadParameter);
			}
			break;

		case NetworkCommand::connClose:					// close a socket gracefully
			if (ValidSocketNumber(messageHeaderIn.hdr.socketNumber))
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
				Connection::Get(messageHeaderIn.hdr.socketNumber).Close();
			}
			else
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseBadParameter);
			}
			break;

		case NetworkCommand::connRead:					// read data from a connection
			if (ValidSocketNumber(messageHeaderIn.hdr.socketNumber))
			{
				Connection& conn = Connection::Get(messageHeaderIn.hdr.socketNumber);
				const size_t amount = conn.Read(reinterpret_cast<uint8_t *>(transferBuffer), std::min<size_t>(messageHeaderIn.hdr.dataBufferAvailable, MaxDataLength));
				messageHeaderIn.hdr.param32 = hspi.transfer32(amount);
				hspi.transferDwords(transferBuffer, nullptr, NumDwords(amount));
			}
			else
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseBadParameter);
			}
			break;

		case NetworkCommand::connWrite:					// write data to a connection
			if (ValidSocketNumber(messageHeaderIn.hdr.socketNumber))
			{
				Connection& conn = Connection::Get(messageHeaderIn.hdr.socketNumber);
				const size_t requestedlength = messageHeaderIn.hdr.dataLength;
				const size_t amount = std::min<size_t>(conn.CanWrite(), std::min<size_t>(requestedlength, MaxDataLength));
				const bool closeAfterSending = amount == requestedlength && (messageHeaderIn.hdr.flags & MessageHeaderSamToEsp::FlagCloseAfterWrite) != 0;
				const bool push = amount == requestedlength && (messageHeaderIn.hdr.flags & MessageHeaderSamToEsp::FlagPush) != 0;
				messageHeaderIn.hdr.param32 = hspi.transfer32(amount);
				hspi.transferDwords(nullptr, transferBuffer, NumDwords(amount));
				const size_t written = conn.Write(reinterpret_cast<uint8_t *>(transferBuffer), amount, push, closeAfterSending);
				if (written != amount)
				{
					lastError = "incomplete write";
				}
			}
			else
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseBadParameter);
			}
			break;

		case NetworkCommand::connGetStatus:				// get the status of a socket, and summary status for all sockets
			if (ValidSocketNumber(messageHeaderIn.hdr.socketNumber))
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(sizeof(ConnStatusResponse));
				Connection& conn = Connection::Get(messageHeaderIn.hdr.socketNumber);
				ConnStatusResponse resp;
				conn.GetStatus(resp);
				Connection::GetSummarySocketStatus(resp.connectedSockets, resp.otherEndClosedSockets);
				hspi.transferDwords(reinterpret_cast<const uint32_t *>(&resp), nullptr, sizeof(resp));
			}
			else
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseBadParameter);
			}
			break;

		case NetworkCommand::diagnostics:				// print some debug info over the UART line
			stats_display();
			SendResponse(ResponseEmpty);
			break;

		case NetworkCommand::connCreate:				// create a connection
		default:
			SendResponse(ResponseUnknownCommand);
			break;
		}
	}

	digitalWrite(SamSSPin, HIGH);     // de-assert CS to SAM to end the transaction and tell SAM the transfer is complete
	hspi.endTransaction();

	// If we deferred the command until after sending the response (e.g. because it may take some time to execute), complete it now
	if (deferCommand)
	{
		// The following functions must set up lastError if an error occurs.
		digitalWrite(EspReqTransferPin, LOW);				// force a status update when complete
		lastError = nullptr;								// assume no error
		switch (messageHeaderIn.hdr.command)
		{
		case NetworkCommand::networkStartClient:			// connect to an access point
			StartClient(nullptr);
			break;

		case NetworkCommand::networkStartAccessPoint:		// run as an access point
			StartAccessPoint();
			break;

		case NetworkCommand::networkStop:					// disconnect from an access point, or close down our own access point
			Connection::TerminateAll();
			//TODO close all sockets if we were connected
			WiFi.disconnect();
			delay(100);
			currentState = WiFiState::idle;
			break;

		case NetworkCommand::networkFactoryReset:			// clear remembered list, reset factory defaults
			FactoryReset();
			break;

		default:
			lastError = "bad deferred command";
			break;
		}
	}
}

void setup()
{
	// Enable serial port for debugging
	Serial.begin(115200);
	Serial.setDebugOutput(true);
	delay(20);

	// Reserve some flash space for use as EEPROM. The maximum EEPROM supported by the core is API_FLASH_SEC_SIZE (4Kb).
	const size_t eepromSizeNeeded = (MaxRememberedNetworks + 1) * sizeof(WirelessConfigurationData);
	static_assert(eepromSizeNeeded <= SPI_FLASH_SEC_SIZE, "Insufficient EEPROM");
	EEPROM.begin(eepromSizeNeeded);
	delay(20);

	// Set up the SPI subsystem
    pinMode(SamTfrReadyPin, INPUT);
    pinMode(EspReqTransferPin, OUTPUT);
    digitalWrite(EspReqTransferPin, LOW);				// not ready to transfer data yet
    pinMode(SamSSPin, OUTPUT);
    digitalWrite(SamSSPin, HIGH);

    // Set up the fast SPI channel
    hspi.begin();
    hspi.setBitOrder(MSBFIRST);
    hspi.setDataMode(SPI_MODE1);
    hspi.setFrequency(spiFrequency);

    Connection::Init();
    Listener::Init();
    netbios_init();
    lastError = nullptr;
    debugPrintln("Init completed");
}

void loop()
{
	digitalWrite(EspReqTransferPin, HIGH);				// tell the SAM we are ready to receive a command

	// See whether there is a request from the SAM
	if (digitalRead(SamTfrReadyPin) == HIGH)
	{
		ProcessRequest();
		if (lastError == nullptr)
		{
			prevLastError = nullptr;
		}
		else if (lastError != prevLastError)
		{
			prevLastError = lastError;
			debugPrint("Signalling error: ");
			debugPrintln(lastError);
			digitalWrite(EspReqTransferPin, LOW);
			delayMicroseconds(1);						// force a low to high transition
		}
	}

	ConnectPoll();
	Connection::PollOne();
	Connection::ReportConnections();

	// Let the WiFi subsystem get on with its stuff
	//yield();
}

// End
