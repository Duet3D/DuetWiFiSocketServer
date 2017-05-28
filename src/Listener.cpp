/*
 * Listener.cpp
 *
 *  Created on: 12 Apr 2017
 *      Author: David
 */

#include "Listener.h"
#include "Connection.h"

// C interface functions
extern "C"
{
	#include "lwip/tcp.h"

	static err_t conn_accept(void *arg, tcp_pcb *pcb, err_t err)
	{
		LWIP_UNUSED_ARG(err);
		if (arg != nullptr)
		{
			return ((Listener*)arg)->Accept(pcb);
		}
		tcp_abort(pcb);
		return ERR_ABRT;
	}
}


// Static member data
Listener *Listener::activeList = nullptr;
Listener *Listener::freeList = nullptr;

// Member functions
Listener::Listener()
	: next(nullptr), listeningPcb(nullptr), ip(0), port(0), maxConnections(0), protocol(0)
{
}

err_t Listener::Accept(tcp_pcb *pcb)
{
	if (listeningPcb != nullptr)
	{
		// Allocate a free socket for this connection
		if (Connection::CountConnectionsOnPort(port) < maxConnections)
		{
			Connection * const conn = Connection::Allocate();
			if (conn != nullptr)
			{
				tcp_accepted(listeningPcb);		// keep the listening PCB running
				return conn->Accept(pcb);
			}
		}
	}
	tcp_abort(pcb);
	return ERR_ABRT;
}

void Listener::Stop()
{
	if (listeningPcb != nullptr)
	{
		tcp_arg(listeningPcb, nullptr);
		tcp_close(listeningPcb);			// stop listening and free the PCB
		listeningPcb = nullptr;
	}
}

/*static*/ bool Listener::Listen(uint32_t ip, uint16_t port, uint8_t protocol, uint16_t maxConns)
{
	// See if we are already listing for this
	for (Listener *p = activeList; p != nullptr; )
	{
		Listener *n = p->next;
		if (p->port == port)
		{
			if (p->ip == IPADDR_ANY || p->ip == ip)
			{
				// already listening, so nothing to do
				return true;
			}
			if (ip == IPADDR_ANY)
			{
				p->Stop();
				Unlink(p);
				Release(p);
			}
		}
		p = n;
	}

	// If we get here then we need to set up a new listener
	Listener * const p = Allocate();
	if (p == nullptr)
	{
		return false;
	}
	p->ip = ip;
	p->port = port;
	p->protocol = protocol;
	p->maxConnections = maxConns;

	// Call LWIP to set up a listener
	tcp_pcb* tempPcb = tcp_new();
	if (tempPcb == nullptr)
	{
		Release(p);
		return false;
	}

	ip_addr tempIp;
	tempIp.addr = ip;
	tcp_bind(tempPcb, &tempIp, port);
	p->listeningPcb = tcp_listen(tempPcb);
	tcp_arg(p->listeningPcb, p);
	tcp_accept(p->listeningPcb, conn_accept);
	p->next = activeList;
	activeList = p;
	return true;
}

/*static*/ void Listener::StopListening(uint16_t port)
{
	for (Listener *p = activeList; p != nullptr; )
	{
		Listener *n = p->next;
		if (p->port == port)
		{
			p->Stop();
			Unlink(p);
			Release(p);
		}
		p = n;
	}
}

/*static*/ uint16_t Listener::GetPortByProtocol(uint8_t protocol)
{
	for (Listener *p = activeList; p != nullptr; p = p->next)
	{
		if (p->protocol == protocol)
		{
			return p->port;
		}
	}
	return 0;
}

/*static*/ Listener *Listener::Allocate()
{
	Listener *ret = freeList;
	if (ret != nullptr)
	{
		freeList = ret->next;
		ret->next = nullptr;
	}
	else
	{
		ret = new Listener;
	}
	return ret;
}

/*static*/ void Listener::Unlink(Listener *lst)
{
	Listener **pp = &activeList;
	while (*pp != nullptr)
	{
		if (*pp == lst)
		{
			*pp = lst->next;
			lst->next = nullptr;
			return;
		}
	}
}

/*static*/ void Listener::Release(Listener *lst)
{
	lst->next = freeList;
	freeList = lst;
}

// End
