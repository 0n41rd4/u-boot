// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Simple network protocol
 *
 * Copyright (c) 2016 Alexander Graf
 *
 * The simple network protocol has the following statuses and services
 * to move between them:
 *
 * Start():	 EfiSimpleNetworkStopped     -> EfiSimpleNetworkStarted
 * Initialize(): EfiSimpleNetworkStarted     -> EfiSimpleNetworkInitialized
 * Shutdown():	 EfiSimpleNetworkInitialized -> EfiSimpleNetworkStarted
 * Stop():	 EfiSimpleNetworkStarted     -> EfiSimpleNetworkStopped
 * Reset():	 EfiSimpleNetworkInitialized -> EfiSimpleNetworkInitialized
 */

#define LOG_CATEGORY LOGC_EFI

#include <efi_loader.h>
#include <dm.h>
#include <linux/sizes.h>
#include <malloc.h>
#include <vsprintf.h>
#include <net.h>

static const efi_guid_t efi_net_guid = EFI_SIMPLE_NETWORK_PROTOCOL_GUID;

/**
 * struct efi_simple_network_extended_protocol - EFI object representing an
 *						 EFI_SIMPLE_NETWORK_PROTOCOL
 *						 interface
 *
 * @net:			simple network protocol interface
 * @net_mode:			status of the network interface
 * @dev:			net udevice
 * @new_tx_packet:		new transmit packet
 * @transmit_buffer:	transmit buffer
 * @receive_buffer:		array of receive buffers
 * @receive_lengths:	array of lengths for received packets
 * @rx_packet_idx:		index of the current receive packet
 * @rx_packet_num:		number of received packets
 * @wait_for_packet:	signaled when a packet has been received
 * @network_timer_event:	event to check for new network packets.
 */
struct efi_simple_network_extended_protocol {
	struct efi_simple_network_protocol net;
	struct efi_simple_network_mode net_mode;
	struct udevice *dev;
	void *new_tx_packet;
	void *transmit_buffer;
	uchar **receive_buffer;
	size_t *receive_lengths;
	int rx_packet_idx;
	int rx_packet_num;
	struct efi_event *wait_for_packet;
	struct efi_event *network_timer_event;
};

static struct efi_simple_network_extended_protocol *current_network;

/*
 * efi_net_start() - start the network interface
 *
 * This function implements the Start service of the
 * EFI_SIMPLE_NETWORK_PROTOCOL. See the Unified Extensible Firmware Interface
 * (UEFI) specification for details.
 *
 * @this:	pointer to the protocol instance
 * Return:	status code
 */
static efi_status_t EFIAPI efi_net_start(struct efi_simple_network_protocol *this)
{
	efi_status_t ret = EFI_SUCCESS;
	struct efi_simple_network_extended_protocol *nt;

	EFI_ENTRY("%p", this);
	/* Check parameters */
	if (!this) {
		ret = EFI_INVALID_PARAMETER;
		goto out;
	}

	nt = (struct efi_simple_network_extended_protocol *)this;

	if (this->mode->state != EFI_NETWORK_STOPPED) {
		ret = EFI_ALREADY_STARTED;
	} else {
		this->int_status = 0;
		nt->wait_for_packet->is_signaled = false;
		this->mode->state = EFI_NETWORK_STARTED;
	}
out:
	return EFI_EXIT(ret);
}

/*
 * efi_net_stop() - stop the network interface
 *
 * This function implements the Stop service of the
 * EFI_SIMPLE_NETWORK_PROTOCOL. See the Unified Extensible Firmware Interface
 * (UEFI) specification for details.
 *
 * @this:	pointer to the protocol instance
 * Return:	status code
 */
static efi_status_t EFIAPI efi_net_stop(struct efi_simple_network_protocol *this)
{
	efi_status_t ret = EFI_SUCCESS;
	struct efi_simple_network_extended_protocol *nt;

	EFI_ENTRY("%p", this);

	/* Check parameters */
	if (!this) {
		ret = EFI_INVALID_PARAMETER;
		goto out;
	}

	nt = (struct efi_simple_network_extended_protocol *)this;

	if (this->mode->state == EFI_NETWORK_STOPPED) {
		ret = EFI_NOT_STARTED;
	} else {
		/* Disable hardware and put it into the reset state */
		eth_set_dev(nt->dev);
		env_set("ethact", eth_get_name());
		eth_halt();
		/* Clear cache of packets */
		nt->rx_packet_num = 0;
		this->mode->state = EFI_NETWORK_STOPPED;
	}
out:
	return EFI_EXIT(ret);
}

/*
 * efi_net_initialize() - initialize the network interface
 *
 * This function implements the Initialize service of the
 * EFI_SIMPLE_NETWORK_PROTOCOL. See the Unified Extensible Firmware Interface
 * (UEFI) specification for details.
 *
 * @this:	pointer to the protocol instance
 * @extra_rx:	extra receive buffer to be allocated
 * @extra_tx:	extra transmit buffer to be allocated
 * Return:	status code
 */
static efi_status_t EFIAPI efi_net_initialize(struct efi_simple_network_protocol *this,
					      ulong extra_rx, ulong extra_tx)
{
	int ret;
	efi_status_t r = EFI_SUCCESS;
	struct efi_simple_network_extended_protocol *nt;

	EFI_ENTRY("%p, %lx, %lx", this, extra_rx, extra_tx);

	/* Check parameters */
	if (!this) {
		r = EFI_INVALID_PARAMETER;
		goto out;
	}
	nt = (struct efi_simple_network_extended_protocol *)this;

	switch (this->mode->state) {
	case EFI_NETWORK_INITIALIZED:
	case EFI_NETWORK_STARTED:
		break;
	default:
		r = EFI_NOT_STARTED;
		goto out;
	}

	/* Setup packet buffers */
	net_init();
	/* Clear cache of packets */
	nt->rx_packet_num = 0;
	/* Set the net device corresponding to the efi net object */
	eth_set_dev(nt->dev);
	env_set("ethact", eth_get_name());
	/* Get hardware ready for send and receive operations */
	ret = eth_start_udev(nt->dev);
	if (ret < 0) {
		eth_halt();
		this->mode->state = EFI_NETWORK_STOPPED;
		r = EFI_DEVICE_ERROR;
		goto out;
	} else {
		this->int_status = 0;
		nt->wait_for_packet->is_signaled = false;
		this->mode->state = EFI_NETWORK_INITIALIZED;
	}
out:
	return EFI_EXIT(r);
}

/*
 * efi_net_reset() - reinitialize the network interface
 *
 * This function implements the Reset service of the
 * EFI_SIMPLE_NETWORK_PROTOCOL. See the Unified Extensible Firmware Interface
 * (UEFI) specification for details.
 *
 * @this:			pointer to the protocol instance
 * @extended_verification:	execute exhaustive verification
 * Return:			status code
 */
static efi_status_t EFIAPI efi_net_reset(struct efi_simple_network_protocol *this,
					 int extended_verification)
{
	efi_status_t ret;

	EFI_ENTRY("%p, %x", this, extended_verification);

	/* Check parameters */
	if (!this) {
		ret = EFI_INVALID_PARAMETER;
		goto out;
	}

	switch (this->mode->state) {
	case EFI_NETWORK_INITIALIZED:
		break;
	case EFI_NETWORK_STOPPED:
		ret = EFI_NOT_STARTED;
		goto out;
	default:
		ret = EFI_DEVICE_ERROR;
		goto out;
	}

	this->mode->state = EFI_NETWORK_STARTED;
	ret = EFI_CALL(efi_net_initialize(this, 0, 0));
out:
	return EFI_EXIT(ret);
}

/*
 * efi_net_shutdown() - shut down the network interface
 *
 * This function implements the Shutdown service of the
 * EFI_SIMPLE_NETWORK_PROTOCOL. See the Unified Extensible Firmware Interface
 * (UEFI) specification for details.
 *
 * @this:	pointer to the protocol instance
 * Return:	status code
 */
static efi_status_t EFIAPI efi_net_shutdown(struct efi_simple_network_protocol *this)
{
	efi_status_t ret = EFI_SUCCESS;
	struct efi_simple_network_extended_protocol *nt;

	EFI_ENTRY("%p", this);

	/* Check parameters */
	if (!this) {
		ret = EFI_INVALID_PARAMETER;
		goto out;
	}
	nt = (struct efi_simple_network_extended_protocol *)this;

	switch (this->mode->state) {
	case EFI_NETWORK_INITIALIZED:
		break;
	case EFI_NETWORK_STOPPED:
		ret = EFI_NOT_STARTED;
		goto out;
	default:
		ret = EFI_DEVICE_ERROR;
		goto out;
	}

	eth_set_dev(nt->dev);
	env_set("ethact", eth_get_name());
	eth_halt();

	this->int_status = 0;
	nt->wait_for_packet->is_signaled = false;
	this->mode->state = EFI_NETWORK_STARTED;

out:
	return EFI_EXIT(ret);
}

/*
 * efi_net_receive_filters() - mange multicast receive filters
 *
 * This function implements the ReceiveFilters service of the
 * EFI_SIMPLE_NETWORK_PROTOCOL. See the Unified Extensible Firmware Interface
 * (UEFI) specification for details.
 *
 * @this:		pointer to the protocol instance
 * @enable:		bit mask of receive filters to enable
 * @disable:		bit mask of receive filters to disable
 * @reset_mcast_filter:	true resets contents of the filters
 * @mcast_filter_count:	number of hardware MAC addresses in the new filters list
 * @mcast_filter:	list of new filters
 * Return:		status code
 */
static efi_status_t EFIAPI efi_net_receive_filters
		(struct efi_simple_network_protocol *this, u32 enable, u32 disable,
		 int reset_mcast_filter, ulong mcast_filter_count,
		 struct efi_mac_address *mcast_filter)
{
	EFI_ENTRY("%p, %x, %x, %x, %lx, %p", this, enable, disable,
		  reset_mcast_filter, mcast_filter_count, mcast_filter);

	return EFI_EXIT(EFI_UNSUPPORTED);
}

/*
 * efi_net_station_address() - set the hardware MAC address
 *
 * This function implements the StationAddress service of the
 * EFI_SIMPLE_NETWORK_PROTOCOL. See the Unified Extensible Firmware Interface
 * (UEFI) specification for details.
 *
 * @this:	pointer to the protocol instance
 * @reset:	if true reset the address to default
 * @new_mac:	new MAC address
 * Return:	status code
 */
static efi_status_t EFIAPI efi_net_station_address
		(struct efi_simple_network_protocol *this, int reset,
		 struct efi_mac_address *new_mac)
{
	EFI_ENTRY("%p, %x, %p", this, reset, new_mac);

	return EFI_EXIT(EFI_UNSUPPORTED);
}

/*
 * efi_net_statistics() - reset or collect statistics of the network interface
 *
 * This function implements the Statistics service of the
 * EFI_SIMPLE_NETWORK_PROTOCOL. See the Unified Extensible Firmware Interface
 * (UEFI) specification for details.
 *
 * @this:	pointer to the protocol instance
 * @reset:	if true, the statistics are reset
 * @stat_size:	size of the statistics table
 * @stat_table:	table to receive the statistics
 * Return:	status code
 */
static efi_status_t EFIAPI efi_net_statistics(struct efi_simple_network_protocol *this,
					      int reset, ulong *stat_size,
					      void *stat_table)
{
	EFI_ENTRY("%p, %x, %p, %p", this, reset, stat_size, stat_table);

	return EFI_EXIT(EFI_UNSUPPORTED);
}

/*
 * efi_net_mcastiptomac() - translate multicast IP address to MAC address
 *
 * This function implements the MCastIPtoMAC service of the
 * EFI_SIMPLE_NETWORK_PROTOCOL. See the Unified Extensible Firmware Interface
 * (UEFI) specification for details.
 *
 * @this:	pointer to the protocol instance
 * @ipv6:	true if the IP address is an IPv6 address
 * @ip:		IP address
 * @mac:	MAC address
 * Return:	status code
 */
static efi_status_t EFIAPI efi_net_mcastiptomac(struct efi_simple_network_protocol *this,
						int ipv6,
						struct efi_ip_address *ip,
						struct efi_mac_address *mac)
{
	efi_status_t ret = EFI_SUCCESS;

	EFI_ENTRY("%p, %x, %p, %p", this, ipv6, ip, mac);

	if (!this || !ip || !mac) {
		ret = EFI_INVALID_PARAMETER;
		goto out;
	}

	if (ipv6) {
		ret = EFI_UNSUPPORTED;
		goto out;
	}

	/* Multi-cast addresses are in the range 224.0.0.0 - 239.255.255.255 */
	if ((ip->ip_addr[0] & 0xf0) != 0xe0) {
		ret = EFI_INVALID_PARAMETER;
		goto out;
	};

	switch (this->mode->state) {
	case EFI_NETWORK_INITIALIZED:
	case EFI_NETWORK_STARTED:
		break;
	default:
		ret = EFI_NOT_STARTED;
		goto out;
	}

	memset(mac, 0, sizeof(struct efi_mac_address));

	/*
	 * Copy lower 23 bits of IPv4 multi-cast address
	 * RFC 1112, RFC 7042 2.1.1.
	 */
	mac->mac_addr[0] = 0x01;
	mac->mac_addr[1] = 0x00;
	mac->mac_addr[2] = 0x5E;
	mac->mac_addr[3] = ip->ip_addr[1] & 0x7F;
	mac->mac_addr[4] = ip->ip_addr[2];
	mac->mac_addr[5] = ip->ip_addr[3];
out:
	return EFI_EXIT(ret);
}

/**
 * efi_net_nvdata() - read or write NVRAM
 *
 * This function implements the GetStatus service of the Simple Network
 * Protocol. See the UEFI spec for details.
 *
 * @this:		the instance of the Simple Network Protocol
 * @read_write:		true for read, false for write
 * @offset:		offset in NVRAM
 * @buffer_size:	size of buffer
 * @buffer:		buffer
 * Return:		status code
 */
static efi_status_t EFIAPI efi_net_nvdata(struct efi_simple_network_protocol *this,
					  int read_write, ulong offset,
					  ulong buffer_size, char *buffer)
{
	EFI_ENTRY("%p, %x, %lx, %lx, %p", this, read_write, offset, buffer_size,
		  buffer);

	return EFI_EXIT(EFI_UNSUPPORTED);
}

/**
 * efi_net_get_status() - get interrupt status
 *
 * This function implements the GetStatus service of the Simple Network
 * Protocol. See the UEFI spec for details.
 *
 * @this:		the instance of the Simple Network Protocol
 * @int_status:		interface status
 * @txbuf:		transmission buffer
 */
static efi_status_t EFIAPI efi_net_get_status(struct efi_simple_network_protocol *this,
					      u32 *int_status, void **txbuf)
{
	efi_status_t ret = EFI_SUCCESS;
	struct efi_simple_network_extended_protocol *nt;

	EFI_ENTRY("%p, %p, %p", this, int_status, txbuf);

	efi_timer_check();

	/* Check parameters */
	if (!this) {
		ret = EFI_INVALID_PARAMETER;
		goto out;
	}

	nt = (struct efi_simple_network_extended_protocol *)this;

	switch (this->mode->state) {
	case EFI_NETWORK_STOPPED:
		ret = EFI_NOT_STARTED;
		goto out;
	case EFI_NETWORK_STARTED:
		ret = EFI_DEVICE_ERROR;
		goto out;
	default:
		break;
	}

	if (int_status) {
		*int_status = this->int_status;
		this->int_status = 0;
	}
	if (txbuf)
		*txbuf = nt->new_tx_packet;

	nt->new_tx_packet = NULL;
out:
	return EFI_EXIT(ret);
}

/**
 * efi_net_transmit() - transmit a packet
 *
 * This function implements the Transmit service of the Simple Network Protocol.
 * See the UEFI spec for details.
 *
 * @this:		the instance of the Simple Network Protocol
 * @header_size:	size of the media header
 * @buffer_size:	size of the buffer to receive the packet
 * @buffer:		buffer to receive the packet
 * @src_addr:		source hardware MAC address
 * @dest_addr:		destination hardware MAC address
 * @protocol:		type of header to build
 * Return:		status code
 */
static efi_status_t EFIAPI efi_net_transmit
		(struct efi_simple_network_protocol *this, size_t header_size,
		 size_t buffer_size, void *buffer,
		 struct efi_mac_address *src_addr,
		 struct efi_mac_address *dest_addr, u16 *protocol)
{
	efi_status_t ret = EFI_SUCCESS;
	struct efi_simple_network_extended_protocol *nt;

	EFI_ENTRY("%p, %lu, %lu, %p, %p, %p, %p", this,
		  (unsigned long)header_size, (unsigned long)buffer_size,
		  buffer, src_addr, dest_addr, protocol);

	efi_timer_check();

	/* Check parameters */
	if (!this || !buffer) {
		ret = EFI_INVALID_PARAMETER;
		goto out;
	}

	nt = (struct efi_simple_network_extended_protocol *)this;

	/* We do not support jumbo packets */
	if (buffer_size > PKTSIZE_ALIGN) {
		ret = EFI_INVALID_PARAMETER;
		goto out;
	}

	/* At least the IP header has to fit into the buffer */
	if (buffer_size < this->mode->media_header_size) {
		ret = EFI_BUFFER_TOO_SMALL;
		goto out;
	}

	/*
	 * TODO:
	 * Support VLANs. Use net_set_ether() for copying the header. Use a
	 * U_BOOT_ENV_CALLBACK to update the media header size.
	 */
	if (header_size) {
		struct ethernet_hdr *header = buffer;

		if (!dest_addr || !protocol ||
		    header_size != this->mode->media_header_size) {
			ret = EFI_INVALID_PARAMETER;
			goto out;
		}
		if (!src_addr)
			src_addr = &this->mode->current_address;

		memcpy(header->et_dest, dest_addr, ARP_HLEN);
		memcpy(header->et_src, src_addr, ARP_HLEN);
		header->et_protlen = htons(*protocol);
	}

	switch (this->mode->state) {
	case EFI_NETWORK_STOPPED:
		ret = EFI_NOT_STARTED;
		goto out;
	case EFI_NETWORK_STARTED:
		ret = EFI_DEVICE_ERROR;
		goto out;
	default:
		break;
	}

	eth_set_dev(nt->dev);
	env_set("ethact", eth_get_name());

	/* Ethernet packets always fit, just bounce */
	memcpy(nt->transmit_buffer, buffer, buffer_size);
	net_send_packet(nt->transmit_buffer, buffer_size);

	nt->new_tx_packet = buffer;
	this->int_status |= EFI_SIMPLE_NETWORK_TRANSMIT_INTERRUPT;
out:
	return EFI_EXIT(ret);
}

/**
 * efi_net_receive() - receive a packet from a network interface
 *
 * This function implements the Receive service of the Simple Network Protocol.
 * See the UEFI spec for details.
 *
 * @this:		the instance of the Simple Network Protocol
 * @header_size:	size of the media header
 * @buffer_size:	size of the buffer to receive the packet
 * @buffer:		buffer to receive the packet
 * @src_addr:		source MAC address
 * @dest_addr:		destination MAC address
 * @protocol:		protocol
 * Return:		status code
 */
static efi_status_t EFIAPI efi_net_receive
		(struct efi_simple_network_protocol *this, size_t *header_size,
		 size_t *buffer_size, void *buffer,
		 struct efi_mac_address *src_addr,
		 struct efi_mac_address *dest_addr, u16 *protocol)
{
	efi_status_t ret = EFI_SUCCESS;
	struct ethernet_hdr *eth_hdr;
	size_t hdr_size = sizeof(struct ethernet_hdr);
	u16 protlen;
	struct efi_simple_network_extended_protocol *nt;

	EFI_ENTRY("%p, %p, %p, %p, %p, %p, %p", this, header_size,
		  buffer_size, buffer, src_addr, dest_addr, protocol);

	/* Execute events */
	efi_timer_check();

	/* Check parameters */
	if (!this || !buffer || !buffer_size) {
		ret = EFI_INVALID_PARAMETER;
		goto out;
	}

	nt = (struct efi_simple_network_extended_protocol *)this;

	switch (this->mode->state) {
	case EFI_NETWORK_STOPPED:
		ret = EFI_NOT_STARTED;
		goto out;
	case EFI_NETWORK_STARTED:
		ret = EFI_DEVICE_ERROR;
		goto out;
	default:
		break;
	}

	if (!nt->rx_packet_num) {
		ret = EFI_NOT_READY;
		goto out;
	}
	/* Fill export parameters */
	eth_hdr = (struct ethernet_hdr *)nt->receive_buffer[nt->rx_packet_idx];
	protlen = ntohs(eth_hdr->et_protlen);
	if (protlen == 0x8100) {
		hdr_size += 4;
		protlen = ntohs(*(u16 *)&nt->receive_buffer[nt->rx_packet_idx][hdr_size - 2]);
	}
	if (header_size)
		*header_size = hdr_size;
	if (dest_addr)
		memcpy(dest_addr, eth_hdr->et_dest, ARP_HLEN);
	if (src_addr)
		memcpy(src_addr, eth_hdr->et_src, ARP_HLEN);
	if (protocol)
		*protocol = protlen;
	if (*buffer_size < nt->receive_lengths[nt->rx_packet_idx]) {
		/* Packet doesn't fit, try again with bigger buffer */
		*buffer_size = nt->receive_lengths[nt->rx_packet_idx];
		ret = EFI_BUFFER_TOO_SMALL;
		goto out;
	}
	/* Copy packet */
	memcpy(buffer, nt->receive_buffer[nt->rx_packet_idx],
	       nt->receive_lengths[nt->rx_packet_idx]);
	*buffer_size = nt->receive_lengths[nt->rx_packet_idx];
	nt->rx_packet_idx = (nt->rx_packet_idx + 1) % ETH_PACKETS_BATCH_RECV;
	nt->rx_packet_num--;
	if (nt->rx_packet_num)
		nt->wait_for_packet->is_signaled = true;
	else
		this->int_status &= ~EFI_SIMPLE_NETWORK_RECEIVE_INTERRUPT;
out:
	return EFI_EXIT(ret);
}

/**
 * efi_net_push() - callback for received network packet
 *
 * This function is called when a network packet is received by eth_rx().
 *
 * @pkt:	network packet
 * @len:	length
 */
static void efi_net_push(void *pkt, int len)
{
	int rx_packet_next;
	struct efi_simple_network_extended_protocol *nt;

	nt = current_network;
	if (!nt)
		return;

	/* Check that we at least received an Ethernet header */
	if (len < sizeof(struct ethernet_hdr))
		return;

	/* Check that the buffer won't overflow */
	if (len > PKTSIZE_ALIGN)
		return;

	/* Can't store more than pre-alloced buffer */
	if (nt->rx_packet_num >= ETH_PACKETS_BATCH_RECV)
		return;

	rx_packet_next = (nt->rx_packet_idx + nt->rx_packet_num) %
	    ETH_PACKETS_BATCH_RECV;
	memcpy(nt->receive_buffer[rx_packet_next], pkt, len);
	nt->receive_lengths[rx_packet_next] = len;

	nt->rx_packet_num++;
}

/**
 * efi_network_timer_notify() - check if a new network packet has been received
 *
 * This notification function is called in every timer cycle.
 *
 * @event:	the event for which this notification function is registered
 * @context:	event context - not used in this function
 */
static void EFIAPI efi_network_timer_notify(struct efi_event *event,
					    void *context)
{
	struct efi_simple_network_extended_protocol *nt;

	EFI_ENTRY("%p, %p", event, context);

	nt = (struct efi_simple_network_extended_protocol *)context;

	/*
	 * Some network drivers do not support calling eth_rx() before
	 * initialization.
	 */
	if (!nt || nt->net.mode->state != EFI_NETWORK_INITIALIZED)
		goto out;

	current_network = nt;

	if (!nt->rx_packet_num) {
		eth_set_dev(nt->dev);
		env_set("ethact", eth_get_name());
		push_packet = efi_net_push;
		eth_rx();
		push_packet = NULL;
		if (nt->rx_packet_num) {
			nt->net.int_status |=
				EFI_SIMPLE_NETWORK_RECEIVE_INTERRUPT;
			nt->wait_for_packet->is_signaled = true;
		}
	}
out:
	EFI_EXIT(EFI_SUCCESS);
}

/**
 * efi_simple_network_install() - install the EFI_SIMPLE_NETWORK_PROTOCOL
 *
 * @handle:	handle to install the protocol
 * @dev:	net udevice
 */
efi_status_t efi_simple_network_install(const efi_handle_t handle)
{
	efi_status_t r;
	struct efi_simple_network_extended_protocol *simple_network;
	void *transmit_buffer = NULL;
	uchar **receive_buffer = NULL;
	size_t *receive_lengths;
	int i;

	if (!handle || !handle->dev) {
		/* No network device active, don't expose any */
		return EFI_SUCCESS;
	}

	/* We only expose the "active" network device, so one is enough */
	simple_network = calloc(1, sizeof(*simple_network));
	if (!simple_network)
		goto out_of_resources;

	simple_network->dev = handle->dev;

	/* Allocate an aligned transmit buffer */
	transmit_buffer = calloc(1, PKTSIZE_ALIGN + PKTALIGN);
	if (!transmit_buffer)
		goto out_of_resources;
	transmit_buffer = (void *)ALIGN((uintptr_t)transmit_buffer, PKTALIGN);
	simple_network->transmit_buffer = transmit_buffer;

	/* Allocate a number of receive buffers */
	receive_buffer = calloc(ETH_PACKETS_BATCH_RECV,
				sizeof(*receive_buffer));
	if (!receive_buffer)
		goto out_of_resources;
	for (i = 0; i < ETH_PACKETS_BATCH_RECV; i++) {
		receive_buffer[i] = malloc(PKTSIZE_ALIGN);
		if (!receive_buffer[i])
			goto out_of_resources;
	}
	simple_network->receive_buffer = receive_buffer;

	receive_lengths = calloc(ETH_PACKETS_BATCH_RECV,
				 sizeof(*receive_lengths));
	if (!receive_lengths)
		goto out_of_resources;
	simple_network->receive_lengths = receive_lengths;

	simple_network->net.revision = EFI_SIMPLE_NETWORK_PROTOCOL_REVISION;
	simple_network->net.start = efi_net_start;
	simple_network->net.stop = efi_net_stop;
	simple_network->net.initialize = efi_net_initialize;
	simple_network->net.reset = efi_net_reset;
	simple_network->net.shutdown = efi_net_shutdown;
	simple_network->net.receive_filters = efi_net_receive_filters;
	simple_network->net.station_address = efi_net_station_address;
	simple_network->net.statistics = efi_net_statistics;
	simple_network->net.mcastiptomac = efi_net_mcastiptomac;
	simple_network->net.nvdata = efi_net_nvdata;
	simple_network->net.get_status = efi_net_get_status;
	simple_network->net.transmit = efi_net_transmit;
	simple_network->net.receive = efi_net_receive;
	simple_network->net.mode = &simple_network->net_mode;
	simple_network->net_mode.state = EFI_NETWORK_STOPPED;
	if (dev_get_plat(handle->dev))
		memcpy(simple_network->net_mode.current_address.mac_addr,
		       ((struct eth_pdata *)dev_get_plat(handle->dev))->enetaddr, 6);
	simple_network->net_mode.hwaddr_size = ARP_HLEN;
	simple_network->net_mode.media_header_size = ETHER_HDR_SIZE;
	simple_network->net_mode.max_packet_size = PKTSIZE;
	simple_network->net_mode.if_type = ARP_ETHER;

	/*
	 * Create WaitForPacket event.
	 */
	r = efi_create_event(EVT_NOTIFY_WAIT, TPL_CALLBACK,
			     efi_network_timer_notify, NULL, NULL,
			     &simple_network->wait_for_packet);
	if (r != EFI_SUCCESS) {
		printf("ERROR: Failed to register network event\n");
		goto error;
	}
	simple_network->net.wait_for_packet = simple_network->wait_for_packet;
	/*
	 * Create a timer event.
	 *
	 * The notification function is used to check if a new network packet
	 * has been received.
	 *
	 * iPXE is running at TPL_CALLBACK most of the time. Use a higher TPL.
	 */
	r = efi_create_event(EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_NOTIFY,
			     efi_network_timer_notify, simple_network, NULL,
			     &simple_network->network_timer_event);
	if (r != EFI_SUCCESS) {
		efi_close_event(simple_network->wait_for_packet);
		printf("ERROR: Failed to register network event\n");
		goto error;
	}
	/* Network is time critical, create event in every timer cycle */
	r = efi_set_timer(simple_network->network_timer_event, EFI_TIMER_PERIODIC, 0);
	if (r != EFI_SUCCESS) {
		efi_close_event(simple_network->network_timer_event);
		efi_close_event(simple_network->wait_for_packet);
		printf("ERROR: Failed to set network timer\n");
		goto error;
	}

	r = efi_add_protocol(handle, &efi_net_guid,
			     &simple_network->net);
	if (r != EFI_SUCCESS){
		efi_close_event(simple_network->network_timer_event);
		efi_close_event(simple_network->wait_for_packet);
		printf("ERROR: Failed to install simple network protocol\n");
		goto error;
	}

	return EFI_SUCCESS;
out_of_resources:
	r = EFI_OUT_OF_RESOURCES;
	printf("ERROR: Out of memory\n");
error:
	free(simple_network);
	free(transmit_buffer);
	if (receive_buffer)
		for (i = 0; i < ETH_PACKETS_BATCH_RECV; i++)
			free(receive_buffer[i]);
	free(receive_buffer);
	free(receive_lengths);
	return r;
}
