/*
 * Copyright (C) 1997-2001 Id Software, Inc.
 * Copyright (C) 2026 Quake II PS3 contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * =======================================================================
 *
 * PlayStation 3 UDP networking using PSL1GHT's BSD socket wrappers.
 * IPv4 is intentionally used here: it covers LAN/Internet Quake II play,
 * DNS, and broadcast server discovery without depending on incomplete
 * IPv6 support in older PSL1GHT releases.
 *
 * =======================================================================
 */

#include "../../common/header/common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/net.h>
#include <net/netdb.h>
#include <net/socket.h>
#include <netinet/in.h>
#include <sys/systime.h>
#include <unistd.h>

#define MAX_LOOPBACK 4

typedef struct
{
	byte data[MAX_MSGLEN];
	int datalen;
} loopmsg_t;

typedef struct
{
	loopmsg_t msgs[MAX_LOOPBACK];
	int get;
	int send;
} loopback_t;

netadr_t net_local_adr;

static loopback_t loopbacks[2];
static int ip_sockets[2] = {-1, -1};
static qboolean network_initialized = false;

static qboolean NET_InitializeLibrary(void);

static void
NET_CloseSocket(int socket_descriptor)
{
	if (socket_descriptor >= 0)
	{
		/* libnet's socket() wrapper marks descriptors with SOCKET_FD_MASK. */
		netClose(socket_descriptor & ~SOCKET_FD_MASK);
	}
}

static void
NET_NetadrToSockadr(const netadr_t *address, struct sockaddr_in *socket_address)
{
	memset(socket_address, 0, sizeof(*socket_address));
	socket_address->sin_len = sizeof(*socket_address);
	socket_address->sin_family = AF_INET;
	socket_address->sin_port = address->port;

	if (address->type == NA_BROADCAST)
	{
		socket_address->sin_addr.s_addr = INADDR_BROADCAST;
	}
	else
	{
		memcpy(&socket_address->sin_addr.s_addr, address->ip, 4);
	}
}

static void
NET_SockadrToNetadr(const struct sockaddr_in *socket_address, netadr_t *address)
{
	memset(address, 0, sizeof(*address));
	address->type = NA_IP;
	memcpy(address->ip, &socket_address->sin_addr.s_addr, 4);
	address->port = socket_address->sin_port;
}

static qboolean
NET_GetLoopPacket(netsrc_t sock, netadr_t *from, sizebuf_t *message)
{
	int index;
	loopback_t *loop = &loopbacks[sock];

	if (loop->send - loop->get > MAX_LOOPBACK)
	{
		loop->get = loop->send - MAX_LOOPBACK;
	}

	if (loop->get >= loop->send)
	{
		return false;
	}

	index = loop->get & (MAX_LOOPBACK - 1);
	loop->get++;

	memcpy(message->data, loop->msgs[index].data,
		loop->msgs[index].datalen);
	message->cursize = loop->msgs[index].datalen;
	memset(from, 0, sizeof(*from));
	from->type = NA_LOOPBACK;

	return true;
}

static void
NET_SendLoopPacket(netsrc_t sock, int length, const void *data)
{
	int index;
	loopback_t *loop = &loopbacks[sock ^ 1];

	index = loop->send & (MAX_LOOPBACK - 1);
	loop->send++;

	memcpy(loop->msgs[index].data, data, length);
	loop->msgs[index].datalen = length;
}

static qboolean
NET_StringToSockadr(const char *text, struct sockaddr_in *address)
{
	char copy[128];
	char *port_text;
	struct hostent *host;
	unsigned int octet[4];
	char trailing;
	int port = 0;

	if (!text || !text[0] || strlen(text) >= sizeof(copy))
	{
		return false;
	}

	Q_strlcpy(copy, text, sizeof(copy));
	port_text = strrchr(copy, ':');

	if (port_text)
	{
		*port_text++ = '\0';

		if (!port_text[0])
		{
			return false;
		}

		port = (int)strtol(port_text, NULL, 10);

		if (port < 1 || port > 65535)
		{
			return false;
		}
	}

	memset(address, 0, sizeof(*address));
	address->sin_len = sizeof(*address);
	address->sin_family = AF_INET;
	address->sin_port = htons((unsigned short)port);

	/* Parse numeric IPv4 locally. This path is also used by the single-player
	 * server before libnet has been initialized. */
	if (sscanf(copy, "%u.%u.%u.%u%c", &octet[0], &octet[1], &octet[2],
		&octet[3], &trailing) == 4 && octet[0] <= 255 && octet[1] <= 255 &&
		octet[2] <= 255 && octet[3] <= 255)
	{
		address->sin_addr.s_addr = htonl((octet[0] << 24) |
			(octet[1] << 16) | (octet[2] << 8) | octet[3]);
		return true;
	}

	if (!NET_InitializeLibrary())
	{
		return false;
	}

	host = gethostbyname(copy);

	if (!host || host->h_addrtype != AF_INET || host->h_length != 4 ||
		!host->h_addr_list || !host->h_addr_list[0])
	{
		Com_Printf("NET: couldn't resolve '%s'\n", copy);
		return false;
	}

	memcpy(&address->sin_addr, host->h_addr_list[0], 4);
	return true;
}

static int
NET_IPSocket(const char *net_interface, int port)
{
	struct sockaddr_in address;
	int newsocket;
	int enabled = 1;

	newsocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	if (newsocket < 0)
	{
		Com_Printf("NET_IPSocket: socket failed: %s\n", strerror(errno));
		return -1;
	}

	if (setsockopt(newsocket, SOL_SOCKET, SO_NBIO, &enabled,
			sizeof(enabled)) < 0)
	{
		Com_Printf("NET_IPSocket: nonblocking mode failed: %s\n",
			strerror(errno));
		NET_CloseSocket(newsocket);
		return -1;
	}

	if (setsockopt(newsocket, SOL_SOCKET, SO_BROADCAST, &enabled,
			sizeof(enabled)) < 0)
	{
		Com_Printf("NET_IPSocket: SO_BROADCAST failed: %s\n",
			strerror(errno));
		NET_CloseSocket(newsocket);
		return -1;
	}

	/* A quick server restart should not leave the Quake II port occupied. */
	setsockopt(newsocket, SOL_SOCKET, SO_REUSEADDR, &enabled,
		sizeof(enabled));

	memset(&address, 0, sizeof(address));
	address.sin_len = sizeof(address);
	address.sin_family = AF_INET;
	address.sin_port = (port == PORT_ANY) ? 0 : htons((unsigned short)port);

	if (!net_interface || !net_interface[0] ||
		!Q_stricmp(net_interface, "localhost") ||
		!Q_stricmp(net_interface, "0.0.0.0"))
	{
		address.sin_addr.s_addr = INADDR_ANY;
	}
	else if (!inet_aton(net_interface, &address.sin_addr))
	{
		struct hostent *host = gethostbyname(net_interface);

		if (!host || host->h_addrtype != AF_INET || host->h_length != 4 ||
			!host->h_addr_list || !host->h_addr_list[0])
		{
			Com_Printf("NET_IPSocket: invalid interface '%s'\n", net_interface);
			NET_CloseSocket(newsocket);
			return -1;
		}

		memcpy(&address.sin_addr, host->h_addr_list[0], 4);
	}

	if (bind(newsocket, (struct sockaddr *)&address, sizeof(address)) < 0)
	{
		Com_Printf("NET_IPSocket: bind failed: %s\n", strerror(errno));
		NET_CloseSocket(newsocket);
		return -1;
	}

	return newsocket;
}

static qboolean
NET_InitializeLibrary(void)
{
	int result;

	if (network_initialized)
	{
		return true;
	}

	result = netInitialize();

	if (result != 0)
	{
		Com_Printf("PS3 network initialization failed: 0x%x\n", result);
		return false;
	}

	network_initialized = true;
	Com_Printf("PS3 IPv4 UDP networking initialized.\n");
	return true;
}

static void
NET_OpenIP(void)
{
	cvar_t *ip = Cvar_Get("ip", "0.0.0.0", CVAR_NOSET);
	cvar_t *port = Cvar_Get("port", va("%i", PORT_SERVER), CVAR_NOSET);

	if (ip_sockets[NS_SERVER] < 0)
	{
		ip_sockets[NS_SERVER] = NET_IPSocket(ip->string, (int)port->value);
	}

	if (ip_sockets[NS_CLIENT] < 0)
	{
		ip_sockets[NS_CLIENT] = NET_IPSocket(ip->string, PORT_ANY);
	}
}

void
NET_Init(void)
{
	/* libnet is initialized lazily by NET_Config(true). This keeps normal
	 * single-player startup independent of the console's network state. */
}

void
NET_Shutdown(void)
{
	if (!network_initialized)
	{
		return;
	}

	NET_Config(false);
	netDeinitialize();
	network_initialized = false;
}

void
NET_Config(qboolean multiplayer)
{
	int index;

	if (multiplayer && !NET_InitializeLibrary())
	{
		return;
	}

	if (!network_initialized)
	{
		return;
	}

	if (multiplayer)
	{
		NET_OpenIP();
		return;
	}

	for (index = 0; index < 2; index++)
	{
		if (ip_sockets[index] >= 0)
		{
			NET_CloseSocket(ip_sockets[index]);
			ip_sockets[index] = -1;
		}
	}
}

qboolean
NET_GetPacket(netsrc_t sock, netadr_t *from, sizebuf_t *message)
{
	struct sockaddr_in socket_from;
	socklen_t from_length = sizeof(socket_from);
	int result;

	if (NET_GetLoopPacket(sock, from, message))
	{
		return true;
	}

	if (!network_initialized || ip_sockets[sock] < 0)
	{
		return false;
	}

	result = recvfrom(ip_sockets[sock], message->data, message->maxsize, 0,
		(struct sockaddr *)&socket_from, &from_length);

	if (result < 0)
	{
		if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ECONNREFUSED)
		{
			Com_Printf("NET_GetPacket: %s\n", strerror(errno));
		}

		return false;
	}

	NET_SockadrToNetadr(&socket_from, from);

	if (result >= message->maxsize)
	{
		Com_Printf("Oversize packet from %s\n", NET_AdrToString(*from));
		return false;
	}

	message->cursize = result;
	return true;
}

void
NET_SendPacket(netsrc_t sock, int length, void *data, netadr_t to)
{
	struct sockaddr_in address;
	int result;

	if (to.type == NA_LOOPBACK)
	{
		NET_SendLoopPacket(sock, length, data);
		return;
	}

	if (to.type != NA_IP && to.type != NA_BROADCAST)
	{
		Com_Printf("NET_SendPacket: unsupported PS3 address type %d\n",
			to.type);
		return;
	}

	if (!network_initialized || ip_sockets[sock] < 0)
	{
		return;
	}

	NET_NetadrToSockadr(&to, &address);
	result = sendto(ip_sockets[sock], data, length, 0,
		(struct sockaddr *)&address, sizeof(address));

	if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
	{
		Com_Printf("NET_SendPacket: %s to %s\n", strerror(errno),
			NET_AdrToString(to));
	}
}

qboolean
NET_CompareAdr(netadr_t a, netadr_t b)
{
	if (a.type != b.type)
	{
		return false;
	}

	if (a.type == NA_LOOPBACK)
	{
		return true;
	}

	return a.type == NA_IP && !memcmp(a.ip, b.ip, 4) && a.port == b.port;
}

qboolean
NET_CompareBaseAdr(netadr_t a, netadr_t b)
{
	if (a.type != b.type)
	{
		return false;
	}

	if (a.type == NA_LOOPBACK)
	{
		return true;
	}

	return a.type == NA_IP && !memcmp(a.ip, b.ip, 4);
}

qboolean
NET_StringToAdr(const char *text, netadr_t *address)
{
	struct sockaddr_in socket_address;

	memset(address, 0, sizeof(*address));

	if (!Q_stricmp(text, "localhost"))
	{
		address->type = NA_LOOPBACK;
		return true;
	}

	if (!NET_StringToSockadr(text, &socket_address))
	{
		return false;
	}

	NET_SockadrToNetadr(&socket_address, address);
	return true;
}

qboolean
NET_IsLocalAddress(netadr_t address)
{
	if (address.type == NA_LOOPBACK)
	{
		return true;
	}

	return address.type == NA_IP && address.ip[0] == 127;
}

char *
NET_BaseAdrToString(netadr_t address)
{
	static char text[64];

	switch (address.type)
	{
		case NA_LOOPBACK:
			Q_strlcpy(text, "loopback", sizeof(text));
			break;
		case NA_BROADCAST:
			Q_strlcpy(text, "255.255.255.255", sizeof(text));
			break;
		case NA_IP:
			Com_sprintf(text, sizeof(text), "%u.%u.%u.%u", address.ip[0],
				address.ip[1], address.ip[2], address.ip[3]);
			break;
		default:
			Q_strlcpy(text, "<unsupported>", sizeof(text));
			break;
	}

	return text;
}

char *
NET_AdrToString(netadr_t address)
{
	static char text[80];

	if (address.type == NA_LOOPBACK)
	{
		Q_strlcpy(text, "loopback", sizeof(text));
	}
	else
	{
		Com_sprintf(text, sizeof(text), "%s:%u", NET_BaseAdrToString(address),
			(unsigned int)ntohs(address.port));
	}

	return text;
}

void
NET_Sleep(int msec)
{
	if (msec > 0)
	{
		sysUsleep((u32)msec * 1000u);
	}
}
