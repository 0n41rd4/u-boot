// SPDX-License-Identifier: GPL-2.0+
/*
 * PXE base code protocol
 *
 * Copyright (c) 2016 Alexander Graf
 *
 */

#define LOG_CATEGORY LOGC_EFI

#include <efi_loader.h>
#include <dm.h>
#include <linux/sizes.h>
#include <malloc.h>
#include <vsprintf.h>
#include <net.h>

static const efi_guid_t efi_pxe_base_code_protocol_guid =
					EFI_PXE_BASE_CODE_PROTOCOL_GUID;

/**
 * struct efi_pxe_base_code_extended_protocol - EFI object representing a
 *												EFI_PXE_BASE_CODE_PROTOCOL
 *												interface
 *
 * @pxe:			PXE base code protocol interface
 * @pxe_mode:			status of the PXE base code protocol

 */
struct efi_pxe_base_code_extended_protocol {
	struct efi_pxe_base_code_protocol pxe;
	struct efi_pxe_mode pxe_mode;
};

static efi_status_t EFIAPI efi_pxe_base_code_start(
				struct efi_pxe_base_code_protocol *this,
				u8 use_ipv6)
{
	return EFI_UNSUPPORTED;
}

static efi_status_t EFIAPI efi_pxe_base_code_stop(
				struct efi_pxe_base_code_protocol *this)
{
	return EFI_UNSUPPORTED;
}

static efi_status_t EFIAPI efi_pxe_base_code_dhcp(
				struct efi_pxe_base_code_protocol *this,
				u8 sort_offers)
{
	return EFI_UNSUPPORTED;
}

static efi_status_t EFIAPI efi_pxe_base_code_discover(
				struct efi_pxe_base_code_protocol *this,
				u16 type, u16 *layer, u8 bis,
				struct efi_pxe_base_code_discover_info *info)
{
	return EFI_UNSUPPORTED;
}

static efi_status_t EFIAPI efi_pxe_base_code_mtftp(
				struct efi_pxe_base_code_protocol *this,
				u32 operation, void *buffer_ptr,
				u8 overwrite, efi_uintn_t *buffer_size,
				struct efi_ip_address server_ip, char *filename,
				struct efi_pxe_base_code_mtftp_info *info,
				u8 dont_use_buffer)
{
	return EFI_UNSUPPORTED;
}

static efi_status_t EFIAPI efi_pxe_base_code_udp_write(
				struct efi_pxe_base_code_protocol *this,
				u16 op_flags, struct efi_ip_address *dest_ip,
				u16 *dest_port,
				struct efi_ip_address *gateway_ip,
				struct efi_ip_address *src_ip, u16 *src_port,
				efi_uintn_t *header_size, void *header_ptr,
				efi_uintn_t *buffer_size, void *buffer_ptr)
{
	return EFI_UNSUPPORTED;
}

static efi_status_t EFIAPI efi_pxe_base_code_udp_read(
				struct efi_pxe_base_code_protocol *this,
				u16 op_flags, struct efi_ip_address *dest_ip,
				u16 *dest_port, struct efi_ip_address *src_ip,
				u16 *src_port, efi_uintn_t *header_size,
				void *header_ptr, efi_uintn_t *buffer_size,
				void *buffer_ptr)
{
	return EFI_UNSUPPORTED;
}

static efi_status_t EFIAPI efi_pxe_base_code_set_ip_filter(
				struct efi_pxe_base_code_protocol *this,
				struct efi_pxe_base_code_filter *new_filter)
{
	return EFI_UNSUPPORTED;
}

static efi_status_t EFIAPI efi_pxe_base_code_arp(
				struct efi_pxe_base_code_protocol *this,
				struct efi_ip_address *ip_addr,
				struct efi_mac_address *mac_addr)
{
	return EFI_UNSUPPORTED;
}

static efi_status_t EFIAPI efi_pxe_base_code_set_parameters(
				struct efi_pxe_base_code_protocol *this,
				u8 *new_auto_arp, u8 *new_send_guid,
				u8 *new_ttl, u8 *new_tos,
				u8 *new_make_callback)
{
	return EFI_UNSUPPORTED;
}

static efi_status_t EFIAPI efi_pxe_base_code_set_station_ip(
				struct efi_pxe_base_code_protocol *this,
				struct efi_ip_address *new_station_ip,
				struct efi_ip_address *new_subnet_mask)
{
	return EFI_UNSUPPORTED;
}

static efi_status_t EFIAPI efi_pxe_base_code_set_packets(
				struct efi_pxe_base_code_protocol *this,
				u8 *new_dhcp_discover_valid,
				u8 *new_dhcp_ack_received,
				u8 *new_proxy_offer_received,
				u8 *new_pxe_discover_valid,
				u8 *new_pxe_reply_received,
				u8 *new_pxe_bis_reply_received,
				EFI_PXE_BASE_CODE_PACKET *new_dchp_discover,
				EFI_PXE_BASE_CODE_PACKET *new_dhcp_acc,
				EFI_PXE_BASE_CODE_PACKET *new_proxy_offer,
				EFI_PXE_BASE_CODE_PACKET *new_pxe_discover,
				EFI_PXE_BASE_CODE_PACKET *new_pxe_reply,
				EFI_PXE_BASE_CODE_PACKET *new_pxe_bis_reply)
{
	return EFI_UNSUPPORTED;
}


/**
 * efi_pxe_install() - install the EFI_PXE_BASE_CODE_PROTOCOL
 *
 * @handle:	handle to install the protocol
 */
efi_status_t efi_pxe_install(const efi_handle_t handle,
			     struct efi_pxe_packet *dhcp_ack)
{
	efi_status_t r;
	struct efi_pxe_base_code_extended_protocol *pxe;

	/* We only expose the "active" network device, so one is enough */
	pxe = calloc(1, sizeof(*pxe));
	if (!pxe) {
		printf("ERROR: Out of memory\n");
		return EFI_OUT_OF_RESOURCES;
	}

	pxe->pxe.revision = EFI_PXE_BASE_CODE_PROTOCOL_REVISION;
	pxe->pxe.start = efi_pxe_base_code_start;
	pxe->pxe.stop = efi_pxe_base_code_stop;
	pxe->pxe.dhcp = efi_pxe_base_code_dhcp;
	pxe->pxe.discover = efi_pxe_base_code_discover;
	pxe->pxe.mtftp = efi_pxe_base_code_mtftp;
	pxe->pxe.udp_write = efi_pxe_base_code_udp_write;
	pxe->pxe.udp_read = efi_pxe_base_code_udp_read;
	pxe->pxe.set_ip_filter = efi_pxe_base_code_set_ip_filter;
	pxe->pxe.arp = efi_pxe_base_code_arp;
	pxe->pxe.set_parameters = efi_pxe_base_code_set_parameters;
	pxe->pxe.set_station_ip = efi_pxe_base_code_set_station_ip;
	pxe->pxe.set_packets = efi_pxe_base_code_set_packets;
	pxe->pxe.mode = &pxe->pxe_mode;

	if (dhcp_ack)
		pxe->pxe_mode.dhcp_ack = *dhcp_ack;

	r = efi_add_protocol(handle, &efi_pxe_base_code_protocol_guid,
			     &pxe->pxe);
	if (r != EFI_SUCCESS)
		efi_free_pool(pxe);

	return r;
}
