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

const uint32_t MaxSendTime = 5000;		// how long we wait for a send to complete before closing the port

// C interface functions
extern "C"
{
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
	: number(num), state(ConnState::free), push(false),
	  localPort(0), remotePort(0), remoteIp(0), closeTimer(0),
	  writeIndex(0), unSent(0), unAcked(0), readIndex(0), alreadyRead(0),
	  ownPcb(nullptr), pb(nullptr)
{
	writeBuffer = new uint8_t[WriteBufferLength];
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
		if (unAcked == 0)
		{
			tcp_close(ownPcb);
			ownPcb = nullptr;
			SetState(ConnState::free);
		}
		else
		{
			closeTimer = millis();
			SetState(ConnState::closePending);
		}
		break;

	case ConnState::otherEndClosed:					// the other end has already closed the connection
		tcp_close(ownPcb);
		ownPcb = nullptr;
		SetState(ConnState::free);
		break;

	case ConnState::closePending:
		// Should not happen, but if it does just let the close proceed when sending is complete or timeout
		break;

	default:
		// should not happen
		SetState(ConnState::free);
		break;
	}
}

// Terminate the connection
void Connection::Terminate()
{
	FreePbuf();
	if (ownPcb != nullptr)
	{
		tcp_abort(ownPcb);
		ownPcb = nullptr;
	}
	SetState(ConnState::free);
}

// Perform housekeeping tasks
void Connection::Poll()
{
	if (state == ConnState::closeReady)
	{
		tcp_close(ownPcb);
		SetState(ConnState::free);
	}
	else
	{
		// Check whether this socket needs to have data sent
		if (unSent != 0 && (state == ConnState::connected || state == ConnState::closePending))
		{
			TrySendData();
		}

		// Check whether this sockets should be closed
		if (state == ConnState::closePending && ((unSent == 0 && unAcked == 0) || millis() - closeTimer >= MaxSendTime))
		{
			tcp_close(ownPcb);
			SetState(ConnState::free);
		}
	}
}

// Write data to the connection. The amount of data may be zero.
size_t Connection::Write(const uint8_t *data, size_t length, bool doPush, bool closeAfterSending)
{
	if (state != ConnState::connected)
	{
		return 0;
	}

	// Copy as much data as we can to the buffer
	const size_t amount = std::min<size_t>(length, CanWrite());
	if (amount != 0)
	{
		if (writeIndex + amount >= WriteBufferLength)
		{
			const size_t chunk = WriteBufferLength - writeIndex;
			memcpy(writeBuffer + writeIndex, data, chunk);
			memcpy(writeBuffer, data + chunk, amount - chunk);
			writeIndex = amount - chunk;
		}
		else
		{
			memcpy(writeBuffer + writeIndex, data, amount);
			writeIndex += amount;
		}
		unSent += amount;
	}

	if (amount == length)
	{
		push = doPush || closeAfterSending;
	}

	TrySendData();

	if (closeAfterSending && amount == length)
	{
		closeTimer = millis();
		SetState(ConnState::closePending);
	}
	return amount;
}

// Try to send some buffered data to the connection
void Connection::TrySendData()
{
	size_t toSend = std::min<size_t>(tcp_sndbuf(ownPcb), unSent);
	if (toSend != 0)
	{
		tcp_sent(ownPcb, conn_sent);
		size_t unsentIndex = (writeIndex - unSent) % WriteBufferLength;
		if (unsentIndex + toSend > WriteBufferLength)
		{
			const size_t chunk = WriteBufferLength - unsentIndex;
			if (tcp_write(ownPcb, writeBuffer + unsentIndex, chunk, TCP_WRITE_FLAG_MORE) != ERR_OK)
			{
				return;
			}
			unSent -= chunk;
			toSend -= chunk;
			unAcked += chunk;
			unsentIndex = 0;
		}

		if (tcp_write(ownPcb, writeBuffer + unsentIndex, toSend, (push && toSend == unSent) ? 0 : TCP_WRITE_FLAG_MORE) == ERR_OK)
		{
			unSent -= toSend;
			unAcked += toSend;
		}
	}
}

size_t Connection::CanWrite() const
{
	if (state != ConnState::connected)
	{
		return 0;
	}
	return WriteBufferLength - unSent - unAcked;		// return the amount of free space in the wrkite buffer
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
		if (pb == nullptr)
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
#ifdef DEBUG
	// The following must be kept in the same order as the declarations in class ConnState
	static const char* const connStateText[] =
	{
		" free",
		" connecting",			// socket is trying to connect
		" connected",			// socket is connected
		" remoteClosed",		// the other end has closed the connection

		" aborted",				// an error has occurred
		" closePending",		// close this socket when sending is complete
		" closeReady"			// about to be closed
	};

	const unsigned int st = (int)state;
	if (st < ARRAY_SIZE(connStateText))
	{
		debugPrint(connStateText[st]);
	}
	else
	{
		debugPrint(" unknown");
	}
	if (state != ConnState::free)
	{
		debugPrint(" ");
		debugPrint(localPort);
		debugPrint(",");
		debugPrint(remotePort);
		debugPrint(",");
		debugPrint(remoteIp & 255);
		debugPrint(".");
		debugPrint((remoteIp >> 8) & 255);
		debugPrint(".");
		debugPrint((remoteIp >> 16) & 255);
		debugPrint(".");
		debugPrint((remoteIp >> 24) & 255);
	}
#endif
}

// Callback functions
err_t Connection::Accept(tcp_pcb *pcb)
{
	ownPcb = pcb;
	tcp_arg(pcb, this);				// tell LWIP that this is the structure we wish to be passed for our callbacks
	tcp_recv(pcb, conn_recv);		// tell LWIP that we wish to be informed of incoming data by a call to the conn_recv() function
	tcp_err(pcb, conn_err);
	SetState(ConnState::connected);
	localPort = pcb->local_port;
	remotePort = pcb->remote_port;
	remoteIp = pcb->remote_ip.addr;
	writeIndex = unSent = unAcked = readIndex = alreadyRead = 0;
	push = false;

	return ERR_OK;
}

void Connection::ConnError(err_t err)
{
	//TODO tell client about the error
	SetState(ConnState::aborted);
	ownPcb = nullptr;
}

err_t Connection::ConnRecv(pbuf *p, err_t err)
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
			// We could perhaps call tcp_close here, but better to do it outside the ISR
			state = ConnState::closeReady;
		}
	}
	else if (pb != nullptr)
	{
		// Should not happen
		return ERR_ABRT;
	}
	else
	{
		// We assume that LWIP is configured with sufficient read buffer space, so we just store the pbuf pointer
		pb = p;
		readIndex = alreadyRead = 0;
	}
	return ERR_OK;
}

// This is called when sent data has been acknowledged
err_t Connection::ConnSent(uint16_t len)
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
#ifdef DEBUG
	if (connectionsChanged)
	{
		debugPrint("Connections:");
		for (size_t i = 0; i < MaxConnections; ++i)
		{
			connectionList[i]->Report();
		}
		debugPrintln();
		connectionsChanged = false;
	}
#endif
}

// Static data
Connection *Connection::connectionList[MaxConnections] = { 0 };
size_t Connection::nextConnectionToPoll = 0;
bool Connection::connectionsChanged = true;

// End
