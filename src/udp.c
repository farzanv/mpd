
/*
 * udp.c
 *
 * Written by Archie Cobbs <archie@freebsd.org>
 * Copyright (c) 1995-1999 Whistle Communications, Inc. All rights reserved.
 * See ``COPYRIGHT.whistle''
 */

#include "ppp.h"
#include "phys.h"
#include "mbuf.h"
#include "udp.h"
#include "ngfunc.h"

#include <netgraph/ng_socket.h>
#include <netgraph/ng_message.h>
#include <netgraph/ng_ksocket.h>
#include <netgraph.h>

/*
 * XXX this is currently broken, as it can deliver out-of-order frames.
 * we need to use a node type that prepends sequence numbers
 */

/*
 * DEFINITIONS
 */

  #define UDP_MTU		2048
  #define UDP_MRU		2048

  #define UDP_MAX_ERRORS	10

  #define UDP_REOPEN_PAUSE	10

  struct udpinfo {
    struct in_addr	self_addr;	/* Configured local IP address */
    struct in_addr	peer_addr;	/* Configured peer IP address */
    u_int16_t		self_port;	/* Configured local port */
    u_int16_t		peer_port;	/* Configured peer port */
    u_int16_t		rxSeq;		/* Last seq received */
    u_int16_t		txSeq;		/* Last seq sent */
  };
  typedef struct udpinfo	*UdpInfo;

/* Set menu options */

  enum {
    SET_PEERADDR,
    SET_SELFADDR,
  };

/*
 * INTERNAL FUNCTIONS
 */

  static int	UdpInit(PhysInfo p);
  static void	UdpOpen(PhysInfo p);
  static void	UdpClose(PhysInfo p);
  static void	UdpStat(PhysInfo p);

  static void	UdpDoClose(UdpInfo udp);
  static int	UdpSetCommand(int ac, char *av[], void *arg);

/*
 * GLOBAL VARIABLES
 */

  const struct phystype gUdpPhysType = {
    "udp",
    TRUE, UDP_REOPEN_PAUSE,
    UDP_MTU, UDP_MRU,
    UdpInit,
    UdpOpen,
    UdpClose,
    NULL,
    NULL,	/* XXX when another node is involved, need a function here */
    UdpStat,
    NULL,
  };

  const struct cmdtab UdpSetCmds[] = {
    { "self ip [port]",			"Set local IP address",
	UdpSetCommand, NULL, (void *) SET_SELFADDR },
    { "peer ip [port]",			"Set remote IP address",
	UdpSetCommand, NULL, (void *) SET_PEERADDR },
    { NULL },
  };

/*
 * UdpInit()
 */

static int
UdpInit(PhysInfo p)
{
  UdpInfo	udp;

  udp = (UdpInfo) (p->info = Malloc(MB_PHYS, sizeof(*udp)));
  return(0);
}

/*
 * UdpOpen()
 */

static void
UdpOpen(PhysInfo p)
{
  UdpInfo		const udp = (UdpInfo) lnk->phys->info;
  char        		path[NG_PATHLEN+1];
  struct ngm_mkpeer	mkp;
  struct sockaddr_in	addr;

  /* Attach ksocket node to PPP node */
  snprintf(mkp.type, sizeof(mkp.type), "%s", NG_KSOCKET_NODE_TYPE);
  snprintf(mkp.ourhook, sizeof(mkp.ourhook),
    "%s%d", NG_PPP_HOOK_LINK_PREFIX, lnk->bundleIndex);
  snprintf(mkp.peerhook, sizeof(mkp.peerhook), "inet/dgram/udp");
  if (NgSendMsg(bund->csock, MPD_HOOK_PPP, NGM_GENERIC_COOKIE,
      NGM_MKPEER, &mkp, sizeof(mkp)) < 0) {
    Log(LG_PHYS, ("[%s] can't attach %s node: %s",
      lnk->name, NG_KSOCKET_NODE_TYPE, strerror(errno)));
    PhysDown(STR_ERROR, NULL);
    return;
  }
  snprintf(path, sizeof(path), "%s.%s", MPD_HOOK_PPP, mkp.ourhook);

  /* Bind socket */
  memset(&addr, 0, sizeof(addr));
  addr.sin_len = sizeof(addr);
  addr.sin_family = AF_INET;
  addr.sin_addr = udp->self_addr;
  addr.sin_port = htons(udp->self_port);
  if (NgSendMsg(bund->csock, path, NGM_KSOCKET_COOKIE,
      NGM_KSOCKET_BIND, &addr, sizeof(addr)) < 0) {
    Log(LG_PHYS, ("[%s] can't bind %s node: %s",
      lnk->name, NG_KSOCKET_NODE_TYPE, strerror(errno)));
    UdpDoClose(udp);
    PhysDown(STR_ERROR, NULL);
    return;
  }

  /* Connect socket if peer address and port is specified */
  if (udp->peer_addr.s_addr != 0 && udp->peer_port != 0) {
    memset(&addr, 0, sizeof(addr));
    addr.sin_len = sizeof(addr);
    addr.sin_family = AF_INET;
    addr.sin_addr = udp->peer_addr;
    addr.sin_port = htons(udp->peer_port);
    if (NgSendMsg(bund->csock, path, NGM_KSOCKET_COOKIE,
	NGM_KSOCKET_CONNECT, &addr, sizeof(addr)) < 0
        && errno != EINPROGRESS) {	/* happens in -current (weird) */
      Log(LG_PHYS, ("[%s] can't connect %s node: %s",
	lnk->name, NG_KSOCKET_NODE_TYPE, strerror(errno)));
      UdpDoClose(udp);
      PhysDown(STR_ERROR, NULL);
      return;
    }
  }

  /* Reset sequence numbers */
  udp->rxSeq = 0;		/* XXX not used */
  udp->txSeq = 0;		/* XXX not used */

  /* OK */
  PhysUp();
}

/*
 * UdpClose()
 */

static void
UdpClose(PhysInfo p)
{
  UdpDoClose((UdpInfo) p->info);
  PhysDown(0, NULL);
}

/*
 * UdpDoClose()
 */

static void
UdpDoClose(UdpInfo udp)
{
  char	hook[NG_HOOKLEN + 1];

  snprintf(hook, sizeof(hook),
    "%s%d", NG_PPP_HOOK_LINK_PREFIX, lnk->bundleIndex);
  NgFuncDisconnect(MPD_HOOK_PPP, hook);
}


#if 0
READ {
  /* Check sequence number to avoid out of order packets */
  seq = ntohs(((u_int16_t *) buf)[0]);
  if ((int) seq - (int) udp->rxSeq <= 0)
    return;
  udp->rxSeq = seq;
  LinkInput(mbwrite(mballoc(MB_FRAME_IN, len - 2), buf + 2, len - 2));
}

WRITE{
  /* Prepend sequence number */
  if (proto != PROTO_UNKNOWN) {
    Mbuf	hdr;

    udp->txSeq++;
    hdr = mballoc(MB_FRAME_OUT, 2);
    ((u_int16_t *) MBDATA(hdr))[0] = htons(udp->txSeq);
    hdr->next = frame;
    frame = hdr;
  }
}
#endif

/*
 * UdpStat()
 */

void
UdpStat(PhysInfo p)
{
  UdpInfo	const udp = (UdpInfo) lnk->phys->info;

  printf("UDP configuration:\n");
  printf("\tSelf address : %s, port %u\n",
    inet_ntoa(udp->self_addr), udp->self_port);
  printf("\tPeer address : %s, port %u\n",
    inet_ntoa(udp->peer_addr), udp->peer_port);
}

/*
 * UdpSetCommand()
 */

static int
UdpSetCommand(int ac, char *av[], void *arg)
{
  UdpInfo		const udp = (UdpInfo) lnk->phys->info;
  struct in_addr	*ap;
  u_short		*pp;

  switch ((intptr_t)arg) {
    case SET_PEERADDR:
      ap = &udp->peer_addr;
      pp = &udp->peer_port;
      goto getAddrPort;
    case SET_SELFADDR:
      ap = &udp->self_addr;
      pp = &udp->self_port;
getAddrPort:
      if (ac < 1 || ac > 2)
	return(-1);
      if (!inet_aton(av[0], ap)) {
	Log(LG_ERR, ("Bad ip address \"%s\"", av[0]));
	return(-1);
      }
      if (ac > 1) {
	if (atoi(av[1]) <= 0) {
	  Log(LG_ERR, ("Bad port \"%s\"", av[1]));
	  return(-1);
	}
	*pp = atoi(av[1]);
      }
      break;

    default:
      assert(0);
  }
  return(0);
}


