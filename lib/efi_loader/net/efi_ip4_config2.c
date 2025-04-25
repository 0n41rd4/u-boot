// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Implementation of EFI_IP4_CONFIG2_PROTOCOL
 *
 */

#define LOG_CATEGORY LOGC_EFI

#include <efi_loader.h>
#include <image.h>
#include <malloc.h>
#include <mapmem.h>
#include <net.h>

static const efi_guid_t efi_ip4_config2_guid = EFI_IP4_CONFIG2_PROTOCOL_GUID;

/* EFI_IP4_CONFIG2_PROTOCOL */

/**
 * struct efi_ip4_config2_extended_protocol - EFI object extending the
 *					      EFI_IP4_CONFIG2_PROTOCOL
 *					      interface
 *
 * @ip4_config2:		EFI_IP4_CONFIG2_PROTOCOL interface
 * @dev:			net udevice
 * @current_address		current address
 * @current_policy		current policy
 */
struct efi_ip4_config2_extended_protocol {
	struct efi_ip4_config2_protocol ip4_config2;
	struct udevice *dev;
	struct efi_ip4_config2_manual_address current_address;
	enum efi_ip4_config2_policy current_policy;
};

/*
 * efi_ip4_config2_set_data() -  Set the configuration for the EFI IPv4 network
 * stack running on the communication device
 *
 * This function implements EFI_IP4_CONFIG2_PROTOCOL.SetData()
 * See the Unified Extensible Firmware Interface
 * (UEFI) specification for details.
 *
 * @this:		pointer to the protocol instance
 * @data_type:		the type of data to set
 * @data_size:		size of the buffer pointed to by data in bytes
 * @data:		the data buffer to set
 * Return:		status code
 */
static efi_status_t EFIAPI efi_ip4_config2_set_data(struct efi_ip4_config2_protocol *this,
						    enum efi_ip4_config2_data_type data_type,
						    efi_uintn_t data_size,
						    void *data)
{
	EFI_ENTRY("%p, %d, %zu, %p", this, data_type, data_size, data);
	efi_status_t ret = EFI_SUCCESS;
	struct efi_ip4_config2_extended_protocol *ipconfig;

	if (!this || (data && !data_size) || (!data && data_size))
		return EFI_EXIT(EFI_INVALID_PARAMETER);

	ipconfig = (struct efi_ip4_config2_extended_protocol *)this;
	if (!ipconfig->dev)
		return EFI_EXIT(EFI_NOT_FOUND);

	switch (data_type) {
	case EFI_IP4_CONFIG2_DATA_TYPE_INTERFACEINFO:
		return EFI_EXIT(EFI_WRITE_PROTECTED);
	case EFI_IP4_CONFIG2_DATA_TYPE_MANUAL_ADDRESS:
		if (ipconfig->current_policy != EFI_IP4_CONFIG2_POLICY_STATIC)
			return EFI_EXIT(EFI_WRITE_PROTECTED);
		if (!data_size && !data) {
			memset((void *)&ipconfig->current_address, 0,
			       sizeof(ipconfig->current_address));
			return EFI_EXIT(EFI_SUCCESS);
		}
		if (data && data_size == sizeof(struct efi_ip4_config2_manual_address)) {
			memcpy((void *)&ipconfig->current_address, data,
			       sizeof(struct efi_ip4_config2_manual_address));
			efi_net_set_addr(&ipconfig->current_address.address,
					 &ipconfig->current_address.subnet_mask,
					 NULL, ipconfig->dev);
			return EFI_EXIT(EFI_SUCCESS);
		}
		return EFI_EXIT(EFI_BAD_BUFFER_SIZE);
	case EFI_IP4_CONFIG2_DATA_TYPE_POLICY:
		if (data && data_size == sizeof(enum efi_ip4_config2_policy)) {
			ipconfig->current_policy = *(enum efi_ip4_config2_policy *)data;
			return EFI_EXIT(EFI_SUCCESS);
		}
		return EFI_EXIT(EFI_BAD_BUFFER_SIZE);

	default:
		return EFI_EXIT(EFI_UNSUPPORTED);
	}

	return EFI_EXIT(ret);
}

/*
 * efi_ip4_config2_get_data() -  Get the configuration for the EFI IPv4 network
 * stack running on the communication device
 *
 * This function implements EFI_IP4_CONFIG2_PROTOCOL.GetData()
 * See the Unified Extensible Firmware Interface
 * (UEFI) specification for details.
 *
 * @this:		pointer to the protocol instance
 * @data_type:		the type of data to get
 * @data_size:		size
 * @data:		the data buffer
 * Return:		status code
 */
static efi_status_t EFIAPI efi_ip4_config2_get_data(struct efi_ip4_config2_protocol *this,
						    enum efi_ip4_config2_data_type data_type,
						    efi_uintn_t *data_size,
						    void *data)
{
	EFI_ENTRY("%p, %d, %p, %p", this, data_type, data_size, data);

	efi_status_t ret = EFI_SUCCESS;
	struct efi_ip4_config2_interface_info *info;
	struct efi_ip4_config2_extended_protocol *ipconfig;
	int tmp;

	if (!this || !data_size)
		return EFI_EXIT(EFI_INVALID_PARAMETER);

	if (*data_size && !data)
		return EFI_EXIT(EFI_INVALID_PARAMETER);

	ipconfig = (struct efi_ip4_config2_extended_protocol *)this;
	if (!ipconfig->dev)
		return EFI_EXIT(EFI_NOT_FOUND);

	tmp = sizeof(struct efi_ip4_config2_interface_info) + sizeof(struct efi_ip4_route_table);

	switch (data_type) {
	case EFI_IP4_CONFIG2_DATA_TYPE_INTERFACEINFO:
		if (*data_size < tmp) {
			*data_size = tmp;
			return EFI_EXIT(EFI_BUFFER_TOO_SMALL);
		}

		info = (struct efi_ip4_config2_interface_info *)data;
		memset(info, 0, sizeof(*info));

		info->hw_address_size = 6;
		memcpy(info->hw_address.mac_addr, eth_get_ethaddr_from_dev(ipconfig->dev), 6);
		// Set the route table size

		info->route_table_size = 0;
		break;
	case EFI_IP4_CONFIG2_DATA_TYPE_MANUAL_ADDRESS:
		if (*data_size < sizeof(struct efi_ip4_config2_manual_address)) {
			*data_size = sizeof(struct efi_ip4_config2_manual_address);
			return EFI_EXIT(EFI_BUFFER_TOO_SMALL);
		}

		efi_net_get_addr(&ipconfig->current_address.address,
				 &ipconfig->current_address.subnet_mask, NULL, ipconfig->dev);
		memcpy(data, (void *)&ipconfig->current_address,
		       sizeof(struct efi_ip4_config2_manual_address));

		break;
	default:
		return EFI_EXIT(EFI_NOT_FOUND);
	}
	return EFI_EXIT(ret);
}

/*
 * efi_ip4_config2_register_notify() -  Register an event that is to be signaled whenever
 * a configuration process on the specified configuration
 * data is done
 *
 * This function implements EFI_IP4_CONFIG2_PROTOCOL.RegisterDataNotify()
 * See the Unified Extensible Firmware Interface
 * (UEFI) specification for details.
 *
 * @this:		pointer to the protocol instance
 * @data_type:		the type of data to register the event for
 * @event:		the event to register
 * Return:		status code
 */
static efi_status_t EFIAPI efi_ip4_config2_register_notify(struct efi_ip4_config2_protocol *this,
							   enum efi_ip4_config2_data_type data_type,
							   struct efi_event *event)
{
	EFI_ENTRY("%p, %d, %p", this, data_type, event);

	return EFI_EXIT(EFI_UNSUPPORTED);
}

/*
 * efi_ip4_config2_unregister_notify() -  Remove a previously registered eventfor
 * the specified configuration data
 *
 * This function implements EFI_IP4_CONFIG2_PROTOCOL.UnregisterDataNotify()
 * See the Unified Extensible Firmware Interface
 * (UEFI) specification for details.
 *
 * @this:		pointer to the protocol instance
 * @data_type:		the type of data to remove the event for
 * @event:		the event to unregister
 * Return:		status code
 */
static efi_status_t EFIAPI efi_ip4_config2_unregister_notify(struct efi_ip4_config2_protocol *this,
							     enum efi_ip4_config2_data_type data_type,
							     struct efi_event *event)
{
	EFI_ENTRY("%p, %d, %p", this, data_type, event);

	return EFI_EXIT(EFI_UNSUPPORTED);
}

/**
 * efi_ip4_config2_install() - install the EFI_IP4_CONFIG2_PROTOCOL
 *
 * @handle	handle to install the protocol
 */
efi_status_t efi_ip4_config2_install(const efi_handle_t handle)
{
	efi_status_t r;
	struct efi_ip4_config2_extended_protocol *ipconfig;

	r = efi_allocate_pool(EFI_LOADER_DATA,
			      sizeof(*ipconfig),
			      (void **)&ipconfig);
	if (r != EFI_SUCCESS)
		return r;

	ipconfig->dev = handle->dev;
	ipconfig->ip4_config2.set_data = efi_ip4_config2_set_data;
	ipconfig->ip4_config2.get_data = efi_ip4_config2_get_data;
	ipconfig->ip4_config2.register_data_notify = efi_ip4_config2_register_notify;
	ipconfig->ip4_config2.unregister_data_notify = efi_ip4_config2_unregister_notify;
	ipconfig->current_policy = EFI_IP4_CONFIG2_POLICY_MAX;

	r = efi_add_protocol(handle, &efi_ip4_config2_guid,
			     &ipconfig->ip4_config2);
	if (r != EFI_SUCCESS)
		efi_free_pool(ipconfig);

	return r;
}
