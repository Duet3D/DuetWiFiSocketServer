/*
 * Socket.h
 *
 *  Created on: 11 Apr 2017
 *      Author: David
 *
 * Simplified socket class to run in ESP8266 in Duet WiFi
 */

#ifndef SRC_CONNECTION_H_
#define SRC_CONNECTION_H_

#include <cstdint>
#include <cstddef>
#include "include/MessageFormats.h"			// for ConnState

// If we #include "tcp.h" here we get clashes between two different ip_addr.h files, so don't do that here
class tcp_pcb;
class pbuf;
typedef signed char err_t;					// compatible with lwip's s8_t

class Connection
{
public:
	Connection(uint8_t num);

	// Public interface
	ConnState GetState() const { return state; }
	void GetStatus(ConnStatusResponse& resp) const;

	void Close();
	void Terminate();
	size_t Write(const uint8_t *data, size_t length, bool doPush, bool closeAfterSending);
	size_t CanWrite() const;
	size_t Read(uint8_t *data, size_t length);
	size_t CanRead() const;
	void Poll();

	// Callback functions
	err_t Accept(tcp_pcb *pcb);
	void ConnError(err_t err);
	err_t ConnRecv(pbuf *p, err_t err);
	err_t ConnSent(uint16_t len);

	// Static functions
	static void Init();
	static Connection *Allocate();
	static Connection& Get(uint8_t num) { return *connectionList[num]; }
	static uint16_t CountConnectionsOnPort(uint16_t port);
	static void PollOne();
	static void ReportConnections();
	static void GetSummarySocketStatus(uint16_t& connectedSockets, uint16_t& otherEndClosedSockets);
	static void TerminateAll();

private:
	void FreePbuf();
	void Report();

	void SetState(ConnState st)
	{
		state = st; connectionsChanged = true;
	}

	uint8_t number;
	volatile ConnState state;

	uint16_t localPort;
	uint16_t remotePort;

	uint32_t remoteIp;
	uint32_t writeTimer;
	uint32_t closeTimer;
	volatile size_t unAcked;	// how much data we have sent but hasn't been acknowledged
	size_t readIndex;			// how much data we have already read from the current pbuf
	size_t alreadyRead;			// how much data we read from previous pbufs and didn't tell LWIP about yet
	tcp_pcb *ownPcb;
	pbuf *pb;

	static Connection *connectionList[MaxConnections];
	static size_t nextConnectionToPoll;
	static volatile bool connectionsChanged;
};

#endif /* SRC_CONNECTION_H_ */
