/*
 * Socket.cpp
 *
 *  Created on: 11 Apr 2017
 *      Author: David
 */

#include "Connection.h"
#include "algorithm"			// for std::min
#include "Arduino.h"			// for millis
#include "Config.h"

const uint32_t MaxWriteTime = 2000;		// how long we wait for a write operation to complete before it is cancelled
const uint32_t MaxAckTime = 4000;		// how long we wait for a connection to acknowledge the remaining data before it is closed

// C interface functions
extern "C"
{
	#include "lwip/init.h"				// for version info
	#include "lwip/tcp.h"

	static void conn_err(void *arg, err_t err)
	{
		if (arg != nullptr)
		{
			((Connection*)arg)->ConnError(err);
		}
	}

	static err_t conn_recv(void *arg, tcp_pcb *pcb, pbuf *p, err_t err)
	{
		if (arg != nullptr)
		{
			return ((Connection*)arg)->ConnRecv(p, err);
		}
		return ERR_ABRT;
	}

	static err_t conn_sent(void *arg, tcp_pcb *pcb, u16_t len)
	{
		if (arg != nullptr)
		{
			return ((Connection*)arg)->ConnSent(len);
		}
		return ERR_ABRT;
	}
}

// Public interface
Connection::Connection(uint8_t num)
	: number(num), state(ConnState::free), localPort(0), remotePort(0), remoteIp(0), writeTimer(0), closeTimer(0),
	  unAcked(0), readIndex(0), alreadyRead(0), ownPcb(nullptr), pb(nullptr)
{
}

void Connection::GetStatus(ConnStatusResponse& resp) const
{
	resp.socketNumber = number;
	resp.state = state;
	resp.bytesAvailable = CanRead();
	resp.writeBufferSpace = CanWrite();
	resp.localPort = localPort;
	resp.remotePort = remotePort;
	resp.remoteIp = remoteIp;
}

// Close the connection gracefully
void Connection::Close()
{
	switch(state)
	{
	case ConnState::connected:						// both ends are still connected
		if (unAcked != 0)
		{
			closeTimer = millis();
			SetState(ConnState::closePending);		// wait for the remaining data to be sent before closing
			break;
		}
		// no break
	case ConnState::otherEndClosed:					// the other end has already closed the connection
	case ConnState::closeReady:						// the other end has closed and we were already closePending
	default:										// should not happen
		if (ownPcb != nullptr)
		{
			tcp_recv(ownPcb, nullptr);
			tcp_sent(ownPcb, nullptr);
			tcp_err(ownPcb, nullptr);
			tcp_close(ownPcb);
			ownPcb = nullptr;
		}
		unAcked = 0;
		FreePbuf();
		SetState(ConnState::free);
		break;

	case ConnState::closePending:					// we already asked to close
		// Should not happen, but if it does just let the close proceed when sending is complete or timeout
		break;
	}
}

// Terminate the connection
void Connection::Terminate()
{
	if (ownPcb != nullptr)
	{
		tcp_recv(ownPcb, nullptr);
		tcp_sent(ownPcb, nullptr);
		tcp_err(ownPcb, nullptr);
		tcp_abort(ownPcb);
		ownPcb = nullptr;
	}
	unAcked = 0;
	FreePbuf();
	SetState(ConnState::free);
}

// Perform housekeeping tasks
void Connection::Poll()
{
	if (state == ConnState::connected)
	{
		// Are we still waiting for data to be written?
		if (writeTimer > 0 && millis() - writeTimer >= MaxWriteTime)
		{
			// Terminate it
			Terminate();
		}
	}
	else if (state == ConnState::closeReady)
	{
		// Deferred close, possibly outside the ISR
		Close();
	}
	else if (state == ConnState::closePending)
	{
		// We're about to close this connection and we're still waiting for the remaining data to be acknowledged
		if (unAcked == 0)
		{
			// All data has been received, close this connection next time
			SetState(ConnState::closeReady);
		}
		else if (millis() - closeTimer >= MaxAckTime)
		{
			// The acknowledgment timer has expired, abort this connection
			Terminate();
		}
	}
}

// Write data to the connection. The amount of data may be zero.
size_t Connection::Write(const uint8_t *data, size_t length, bool doPush, bool closeAfterSending)
{
	// Can we write anything at all?
	if (CanWrite() == 0)
	{
		if (writeTimer == 0)
		{
			// No - there is no space left. Don't wait forever until there is any available
			writeTimer = millis();
		}
		return 0;
	}

	// Send one SPI packet at once
	const bool push = doPush || closeAfterSending;
	err_t result = tcp_write(ownPcb, data, length, push ? TCP_WRITE_FLAG_COPY : TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE);
	if (result == ERR_OK)
	{
		// Data could be successfully written
		writeTimer = 0;
		unAcked += length;
	}
	else
	{
		// Something went wrong. Let the main firmware deal with this
#if LWIP_VERSION_MAJOR == 2
		// chrishamm: Not sure if this helps with LwIP v1.4.3 but it is mandatory for proper error handling with LwIP 2.0.3
		tcp_abort(ownPcb);
		ownPcb = nullptr;
		SetState(ConnState::aborted);
		FreePbuf();
#endif
		return 0;
	}

	// See if we need to push the remaining data
	if (push || tcp_sndbuf(ownPcb) <= TCP_SNDLOWAT)
	{
		tcp_output(ownPcb);
	}

	// Close the connection again when we're done
	if (closeAfterSending)
	{
		closeTimer = millis();
		SetState(ConnState::closePending);
	}
	return length;
}

size_t Connection::CanWrite() const
{
	// Return the amount of free space in the write buffer
	return (state == ConnState::connected) ? tcp_sndbuf(ownPcb) : 0;
}

size_t Connection::Read(uint8_t *data, size_t length)
{
	size_t lengthRead = 0;
	if (pb != nullptr && length != 0 && (state == ConnState::connected || state == ConnState::otherEndClosed))
	{
		do
		{
			const size_t toRead = std::min<size_t>(pb->len - readIndex, length);
			memcpy(data + lengthRead, (uint8_t *)pb->payload + readIndex, toRead);
			lengthRead += toRead;
			readIndex += toRead;
			length -= toRead;
			if (readIndex != pb->len)
			{
				break;
			}
			pbuf * const currentPb = pb;
			pb = pb->next;
			currentPb->next = nullptr;
			pbuf_free(currentPb);
			readIndex = 0;
		} while (pb != nullptr && length != 0);

		alreadyRead += lengthRead;
		if (pb == nullptr || alreadyRead >= TCP_MSS)
		{
			tcp_recved(ownPcb, alreadyRead);
			alreadyRead = 0;
		}
	}
	return lengthRead;
}

size_t Connection::CanRead() const
{
	return ((state == ConnState::connected || state == ConnState::otherEndClosed) && pb != nullptr)
			? pb->tot_len - readIndex
				: 0;
}

void Connection::Report()
{
	// The following must be kept in the same order as the declarations in class ConnState
	static const char* const connStateText[] =
	{
		"free\n",
		"connecting",			// socket is trying to connect
		"connected",			// socket is connected
		"remoteClosed",			// the other end has closed the connection

		"aborted",				// an error has occurred
		"closePending",			// close this socket when sending is complete
		"closeReady"			// about to be closed
	};

	const unsigned int st = (int)state;
	printf("%s", (st < ARRAY_SIZE(connStateText)) ? connStateText[st]: "unknown");
	if (state != ConnState::free)
	{
		printf(" %u, %u, %u.%u.%u.%u\n", localPort, remotePort, remoteIp & 255, (remoteIp >> 8) & 255, (remoteIp >> 16) & 255, (remoteIp >> 24) & 255);
	}
}

// Callback functions
int Connection::Accept(tcp_pcb *pcb)
{
	ownPcb = pcb;
	tcp_arg(pcb, this);				// tell LWIP that this is the structure we wish to be passed for our callbacks
	tcp_recv(pcb, conn_recv);		// tell LWIP that we wish to be informed of incoming data by a call to the conn_recv() function
	tcp_sent(pcb, conn_sent);
	tcp_err(pcb, conn_err);
	SetState(ConnState::connected);
	localPort = pcb->local_port;
	remotePort = pcb->remote_port;
	remoteIp = pcb->remote_ip.addr;
	writeTimer = closeTimer = 0;
	unAcked = readIndex = alreadyRead = 0;

	return ERR_OK;
}

void Connection::ConnError(int err)
{
	tcp_sent(ownPcb, nullptr);
	tcp_recv(ownPcb, nullptr);
	tcp_err(ownPcb, nullptr);
	ownPcb = nullptr;
	SetState(ConnState::aborted);
}

int Connection::ConnRecv(pbuf *p, int err)
{
	if (p == nullptr)
	{
		// The other end has closed the connection
		if (state == ConnState::connected)
		{
			SetState(ConnState::otherEndClosed);
		}
		else if (state == ConnState::closePending)
		{
			// We could perhaps call tcp_close here, but perhaps better to do it outside the callback
			state = ConnState::closeReady;
		}
	}
	else if (pb != nullptr)
	{
		pbuf_cat(pb, p);
	}
	else
	{
		pb = p;
		readIndex = alreadyRead = 0;
	}
	debugPrint("Packet rcvd\n");
	return ERR_OK;
}

// This is called when sent data has been acknowledged
int Connection::ConnSent(uint16_t len)
{
	if (len <= unAcked)
	{
		unAcked -= len;
	}
	else
	{
		// Something is wrong, more data has been acknowledged than has been sent (hopefully this will never occur)
		unAcked = 0;
	}
	return ERR_OK;
}

void Connection::FreePbuf()
{
	if (pb != nullptr)
	{
		pbuf_free(pb);
		pb = nullptr;
	}
}

// Static functions
/*static*/ Connection *Connection::Allocate()
{
	for (size_t i = 0; i < MaxConnections; ++i)
	{
		if (connectionList[i]->state == ConnState::free)
		{
			return connectionList[i];
		}
	}
	return nullptr;
}

/*static*/ void Connection::Init()
{
	for (size_t i = 0; i < MaxConnections; ++i)
	{
		connectionList[i] = new Connection((uint8_t)i);
	}
}

/*static*/ uint16_t Connection::CountConnectionsOnPort(uint16_t port)
{
	uint16_t count = 0;
	for (size_t i = 0; i < MaxConnections; ++i)
	{
		if (connectionList[i]->localPort == port)
		{
			const ConnState state = connectionList[i]->state;
			if (state == ConnState::connected || state == ConnState::otherEndClosed || state == ConnState::closePending)
			{
				++count;
			}
		}
	}
	return count;
}

/*static*/ void Connection::PollOne()
{
	Connection::Get(nextConnectionToPoll).Poll();
	++nextConnectionToPoll;
	if (nextConnectionToPoll == MaxConnections)
	{
		nextConnectionToPoll = 0;
	}
}

/*static*/ void Connection::TerminateAll()
{
	for (size_t i = 0; i < MaxConnections; ++i)
	{
		Connection::Get(i).Terminate();
	}
}

/*static*/ void Connection::GetSummarySocketStatus(uint16_t& connectedSockets, uint16_t& otherEndClosedSockets)
{
	connectedSockets = 0;
	otherEndClosedSockets = 0;
	for (size_t i = 0; i < MaxConnections; ++i)
	{
		if (Connection::Get(i).GetState() == ConnState::connected)
		{
			connectedSockets |= (1 << i);
		}
		else if (Connection::Get(i).GetState() == ConnState::otherEndClosed)
		{
			otherEndClosedSockets |= (1 << i);
		}
	}
}

/*static*/ void Connection::ReportConnections()
{
	for (size_t i = 0; i < MaxConnections; ++i)
	{
		printf("Conn %u: ", i);
		connectionList[i]->Report();
	}
}

// Static data
Connection *Connection::connectionList[MaxConnections] = { 0 };
size_t Connection::nextConnectionToPoll = 0;

// End
