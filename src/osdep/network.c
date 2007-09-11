/*-
 * Copyright (c) 2007, Andrea Bittau <a.bittau@cs.ucl.ac.uk>
 *
 * OS dependent API for using card via network.
 *
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <sys/select.h>

#include "osdep.h"
#include "network.h"

#define QUEUE_MAX 666

struct queue {
	unsigned char	q_buf[2048];
	int		q_len;

	struct queue	*q_next;
	struct queue	*q_prev;
};

struct priv_net {
	int		pn_s;
	struct queue	pn_queue;
	struct queue	pn_queue_free;
	int		pn_queue_len;
};

int net_send(int s, int command, void *arg, int len)
{
        struct timeval tv;
	struct net_hdr nh;
        fd_set fds;
	char* buffer;

	buffer = (char*) malloc(sizeof(nh) + len);
	if(buffer == NULL)
	{
		perror("malloc");
		return -1;
	}

        FD_ZERO(&fds);
        FD_SET(s, &fds);

        tv.tv_sec=0;
        tv.tv_usec=10;

	memset(&nh, 0, sizeof(nh));
	nh.nh_type	= command;
	nh.nh_len	= htonl(len);

	memcpy(buffer, &nh, sizeof(nh));
	if(len != 0)
		memcpy(buffer + sizeof(nh), arg, len);

        if( select(s+1, NULL, &fds, NULL, &tv) != 1)
	{
		free(buffer);
        	return -1;
	}
	if (send(s, buffer, sizeof(nh)+len, 0) != (signed)(sizeof(nh)+len))
	{
		free(buffer);
        	return -1;
	}

	free(buffer);

	return 0;
}

int net_read_exact(int s, void *arg, int len)
{
	unsigned char *p = arg;
	int rc;
        struct timeval tv;
        fd_set fds;

	while (len) {
            FD_ZERO(&fds);
            FD_SET(s, &fds);

            tv.tv_sec=0;
            tv.tv_usec=10;

		rc = read(s, p, len);
		if (rc <= 0)
			return -1;
		p += rc;
		len -= rc;

		assert(rc > 0);
	}

	return 0;
}

int net_get(int s, void *arg, int *len)
{
	struct net_hdr nh;
	int plen;

	if (net_read_exact(s, &nh, sizeof(nh)) == -1)
        {
		return -1;
        }

	plen = ntohl(nh.nh_len);
	if (!(plen <= *len))
		printf("PLEN %d type %d len %d\n",
			plen, nh.nh_type, *len);
	assert(plen <= *len); /* XXX */

	*len = plen;
	if ((*len) && (net_read_exact(s, arg, *len) == -1))
        {
            return -1;
        }

	return nh.nh_type;
}

static void queue_del(struct queue *q)
{
	q->q_prev->q_next = q->q_next;
	q->q_next->q_prev = q->q_prev;
}

static void queue_add(struct queue *head, struct queue *q)
{
	struct queue *pos = head->q_prev;

	q->q_prev = pos;
	q->q_next = pos->q_next;
	q->q_next->q_prev = q;
	pos->q_next = q;
}

#if 0
static int queue_len(struct queue *head)
{
	struct queue *q = head->q_next;
	int i = 0;

	while (q != head) {
		i++;
		q = q->q_next;
	}

	return i;
}
#endif

static struct queue *queue_get_slot(struct priv_net *pn)
{
	struct queue *q = pn->pn_queue_free.q_next;

	if (q != &pn->pn_queue_free) {
		queue_del(q);
		return q;
	}

	if (pn->pn_queue_len++ > QUEUE_MAX)
		return NULL;

	return malloc(sizeof(*q));
}

static void net_enque(struct priv_net *pn, void *buf, int len)
{
	struct queue *q;

	q = queue_get_slot(pn);
	if (!q)
		return;

	q->q_len = len;
	assert((int) sizeof(q->q_buf) >= q->q_len);
	memcpy(q->q_buf, buf, q->q_len);
	queue_add(&pn->pn_queue, q);
}

static int net_get_nopacket(struct priv_net *pn, void *arg, int *len)
{
	unsigned char buf[2048];
	int l = sizeof(buf);
	int c;

	while (1) {
		l = sizeof(buf);
		c = net_get(pn->pn_s, buf, &l);

		if (c != NET_PACKET && c > 0)
			break;

                if(c > 0)
                    net_enque(pn, buf, l);
	}

	assert(l <= *len);
	memcpy(arg, buf, l);
	*len = l;

	return c;
}

static int net_cmd(struct priv_net *pn, int command, void *arg, int alen)
{
	uint32_t rc;
	int len;
	int cmd;

	if (net_send(pn->pn_s, command, arg, alen) == -1)
        {
		return -1;
        }

	len = sizeof(rc);
	cmd = net_get_nopacket(pn, &rc, &len);
	if (cmd == -1)
        {
		return -1;
        }
	assert(cmd == NET_RC);
	assert(len == sizeof(rc));

	return ntohl(rc);
}

static int queue_get(struct priv_net *pn, void *buf, int len)
{
	struct queue *head = &pn->pn_queue;
	struct queue *q = head->q_next;

	if (q == head)
		return 0;

	assert(q->q_len <= len);
	memcpy(buf, q->q_buf, q->q_len);

	queue_del(q);
	queue_add(&pn->pn_queue_free, q);

	return q->q_len;
}

static int net_read(struct wif *wi, unsigned char *h80211, int len,
		    struct rx_info *ri)
{
	struct priv_net *pn = wi_priv(wi);
	unsigned char buf[2048];
	int cmd;
	int sz = sizeof(*ri);
	int l;

	/* try queue */
	l = queue_get(pn, buf, sizeof(buf));
	if (!l) {
		/* try reading form net */
		l = sizeof(buf);
		cmd = net_get(pn->pn_s, buf, &l);

		if (cmd == -1)
			return -1;
		if (cmd == NET_RC)
			return ntohl(*((uint32_t*)buf));
		assert(cmd == NET_PACKET);
	}

	/* XXX */
	if (ri)
		memcpy(ri, buf, sz);
	l -= sz;
	assert(l > 0);
	if (l > len)
		l = len;
	memcpy(h80211, &buf[sz], l);

	return l;
}

static int net_get_mac(struct wif *wi, unsigned char *mac)
{
	struct priv_net *pn = wi_priv(wi);
	unsigned char buf[6];
	int cmd;
	int sz = sizeof(buf);

	if (net_send(pn->pn_s, NET_GET_MAC, NULL, 0) == -1)
		return -1;

	cmd = net_get_nopacket(pn, buf, &sz);
	if (cmd == -1)
		return -1;
	if (cmd == NET_RC)
		return ntohl(*((uint32_t*)buf));
	assert(cmd == NET_MAC);
	assert(sz == sizeof(buf));

	memcpy(mac, buf, 6);

	return 0;
}

static int net_write(struct wif *wi, unsigned char *h80211, int len,
		     struct tx_info *ti)
{
	struct priv_net *pn = wi_priv(wi);
	int sz = sizeof(*ti);
	unsigned char buf[2048];
	unsigned char *ptr = buf;

	/* XXX */
	if (ti)
		memcpy(ptr, ti, sz);
	else
		memset(ptr, 0, sizeof(*ti));

	ptr += sz;
	memcpy(ptr, h80211, len);
	sz += len;

	return net_cmd(pn, NET_WRITE, buf, sz);
}

static int net_set_channel(struct wif *wi, int chan)
{
	uint32_t c = htonl(chan);

	return net_cmd(wi_priv(wi), NET_SET_CHAN, &c, sizeof(c));
}

static int net_get_channel(struct wif *wi)
{
	struct priv_net *pn = wi_priv(wi);

	return net_cmd(pn, NET_GET_CHAN, NULL, 0);
}

static int net_set_rate(struct wif *wi, int rate)
{
	uint32_t c = htonl(rate);

	return net_cmd(wi_priv(wi), NET_SET_RATE, &c, sizeof(c));
}

static int net_get_rate(struct wif *wi)
{
	struct priv_net *pn = wi_priv(wi);

	return net_cmd(pn, NET_GET_RATE, NULL, 0);
}

static int net_get_monitor(struct wif *wi)
{
	return net_cmd(wi_priv(wi), NET_GET_MONITOR, NULL, 0);
}

static void do_net_free(struct wif *wi)
{
	assert(wi->wi_priv);
	free(wi->wi_priv);
	wi->wi_priv = 0;
	free(wi);
}

static void net_close(struct wif *wi)
{
	struct priv_net *pn = wi_priv(wi);

	close(pn->pn_s);
	do_net_free(wi);
}

static int get_ip_port(char *iface, char *ip)
{
	char *host;
	char *ptr;
	int port = -1;
	struct in_addr addr;

	host = strdup(iface);
	if (!host)
		return -1;

	ptr = strchr(host, ':');
	if (!ptr)
		goto out;

	*ptr++ = 0;

	if (!inet_aton(host, &addr))
		goto out; /* XXX resolve hostname */

	assert(strlen(host) <= 15);
	strcpy(ip, host);
	port = atoi(ptr);

out:
	free(host);
	return port;
}

static int handshake(int s)
{
	if (s) {} /* XXX unused */
	/* XXX do a handshake */
	return 0;
}

static int do_net_open(char *iface)
{
	int s, port;
	char ip[16];
	struct sockaddr_in s_in;

	port = get_ip_port(iface, ip);
	if (port == -1)
		return -1;

	s_in.sin_family = PF_INET;
	s_in.sin_port = htons(port);
	if (!inet_aton(ip, &s_in.sin_addr))
		return -1;

	if ((s = socket(s_in.sin_family, SOCK_STREAM, IPPROTO_TCP)) == -1)
		return -1;

	printf("Connecting to %s port %d...\n", ip, port);

	if (connect(s, (struct sockaddr*) &s_in, sizeof(s_in)) == -1) {
		close(s);

		printf("Failed to connect\n");

		return -1;
	}

	if (handshake(s) == -1) {
		close(s);

		printf("Failed to connect - handshake failed\n");

		return -1;
	}

	printf("Connection successful\n");

	return s;
}

static int net_fd(struct wif *wi)
{
	struct priv_net *pn = wi_priv(wi);

	return pn->pn_s;
}

struct wif *net_open(char *iface)
{
	struct wif *wi;
	struct priv_net *pn;
	int s;

	/* setup wi struct */
	wi = wi_alloc(sizeof(*pn));
	if (!wi)
		return NULL;
	wi->wi_read		= net_read;
	wi->wi_write		= net_write;
	wi->wi_set_channel	= net_set_channel;
	wi->wi_get_channel	= net_get_channel;
        wi->wi_set_rate    	= net_set_rate;
	wi->wi_get_rate    	= net_get_rate;
	wi->wi_close		= net_close;
	wi->wi_fd		= net_fd;
	wi->wi_get_mac		= net_get_mac;
	wi->wi_get_monitor	= net_get_monitor;

	/* setup iface */
	s = do_net_open(iface);
	if (s == -1) {
		do_net_free(wi);
		return NULL;
	}

	/* setup private state */
	pn = wi_priv(wi);
	pn->pn_s = s;
	pn->pn_queue.q_next = pn->pn_queue.q_prev = &pn->pn_queue;
	pn->pn_queue_free.q_next = pn->pn_queue_free.q_prev
					= &pn->pn_queue_free;

	return wi;
}