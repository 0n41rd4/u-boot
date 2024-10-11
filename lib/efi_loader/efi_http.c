// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * An HTTP driver
 *
 * HTTP_PROTOCOL
 * HTTP_SERVICE_BINDING_PROTOCOL
 * IP4_CONFIG2_PROTOCOL
 */
#include <charset.h>
#include <efi_loader.h>
#include <image.h>
#include <linux/sizes.h>
#include <malloc.h>
#include <mapmem.h>
#include <net.h>
#include <net/wget.h>
#include <lmb.h>


static const efi_guid_t efi_ip4_config2_guid = EFI_IP4_CONFIG2_PROTOCOL_GUID;
static const efi_guid_t efi_http_service_binding_guid = EFI_HTTP_SERVICE_BINDING_PROTOCOL_GUID;
static const efi_guid_t efi_http_guid = EFI_HTTP_PROTOCOL_GUID;

/*
 * struct efi_http_driver - EFI object implementing HTTP protocol
 *
 * @header:	EFI object header
 * @http:	http protocol interface
 */
struct efi_http_instance {
	efi_handle_t handle;		// Pointer to efi object
	struct efi_object header;	// Efi object, in case caller does not provide
	struct efi_http_protocol http;
	bool configured;
	ulong current_offset;
	ulong http_size;
	ulong http_load_addr;
};

// This is what the HTTP service binding protocol returns.
static struct efi_http_instance http_instance;

struct efi_ip4_config2_manual_address current_http_ip;
static enum efi_ip4_config2_policy current_policy;
static ulong http_tcp_port;
static char http_current_mac_addr[32];
static int num_instances = 0;

enum efi_http_status_code efi_ul_to_httpstatus(ulong status);

static efi_status_t efi_http_set_buffer(struct efi_http_instance *instance)
{
	efi_status_t ret = EFI_SUCCESS;

	efi_free_pool((void *)instance->http_load_addr);

	instance->http_load_addr = (ulong)efi_alloc(instance->http_size);

	if (instance->http_load_addr == 0) {
		ret = EFI_OUT_OF_RESOURCES;
	}

	return ret;
}

static efi_status_t efi_http_send_headers(struct efi_http_header **client_headers, efi_uintn_t *client_header_count)
{
	efi_status_t ret = EFI_SUCCESS;
	size_t headers_size;

	headers_size = (current_http_info.num_headers)*sizeof(struct efi_http_header);

	ret = efi_allocate_pool(EFI_BOOT_SERVICES_DATA, headers_size,
				(void **)client_headers); // This is deallocated by the client.

	if (ret != EFI_SUCCESS)
		goto out;

	// Send headers
	*client_header_count = current_http_info.num_headers;
	for (int i = 0; i < current_http_info.num_headers; i++) {
		(*client_headers)[i].field_name = current_http_info.headers[i].name;
		(*client_headers)[i].field_value = current_http_info.headers[i].value;
	}

	// Send only once
	current_http_info.num_headers = 0;

out:
	return ret;
}

static efi_status_t efi_http_send_data(struct efi_http_instance *instance, void *client_buffer, efi_uintn_t *client_buffer_size)
{
	efi_status_t ret = EFI_SUCCESS;
	ulong transfer_size;
	uchar *ptr;

	// Amount of data left;
	transfer_size = current_http_info.content_length - instance->current_offset;

	// Amount of data the client is willing to receive
	if (transfer_size > *client_buffer_size) {
		transfer_size = *client_buffer_size;
	} else {
		*client_buffer_size = transfer_size;
	}

	if (!transfer_size) // Ok, only headers
		goto out;

	if (!client_buffer) {
		ret = EFI_INVALID_PARAMETER;
		goto out;
	}

	// Send data
	ptr = map_sysmem(image_load_addr + instance->current_offset, transfer_size);
	memcpy(client_buffer, ptr, transfer_size);
	unmap_sysmem(ptr);

	instance->current_offset += transfer_size;

	// Whole file served, clean the buffer:
	if (instance->current_offset == current_http_info.content_length){
		efi_free_pool((void *)http_instance.http_load_addr);
		http_instance.current_offset = 0;
		current_http_info.content_length = 0;
	}

out:
	return ret;
}

/* EFI_HTTP_PROTOCOL */

/*
 * efi_http_get_mode_data() - Gets the current operational status.
 *
 * This function implements EFI_HTTP_PROTOCOL.GetModeData()
 * See the Unified Extensible Firmware Interface
 * (UEFI) specification for details.
 *
 * @this:	pointer to the protocol instance
 * @data:	pointer to the buffer for operational parameters
 * 		of this HTTP instance
 * Return:	status code
 */
static efi_status_t EFIAPI efi_http_get_mode_data(struct efi_http_protocol *this, struct efi_http_config_data *data)
{
	EFI_ENTRY("%p, %p", this, data);

	efi_status_t ret = EFI_UNSUPPORTED;

	/* Only support ipv4 */
	if (!this || !data || !data->access_point.ipv4_node) {
		ret = EFI_INVALID_PARAMETER;
		goto out;
	}

out:
	return EFI_EXIT(ret);
}

/*
 * efi_http_configure() - Initializes operational status for this
 * EFI HTTP instance.
 *
 * This function implements EFI_HTTP_PROTOCOL.Configure()
 * See the Unified Extensible Firmware Interface
 * (UEFI) specification for details.
 *
 * @this:	pointer to the protocol instance
 * @data:	pointer to the buffer for operational parameters of
 * 		this HTTP instance
 * Return:	status code
 */
static efi_status_t EFIAPI efi_http_configure(struct efi_http_protocol *this, struct efi_http_config_data *data)
{
	EFI_ENTRY("%p, %p", this, data);

	efi_status_t ret = EFI_SUCCESS;
	enum efi_http_version http_version;
	struct efi_httpv4_access_point *ipv4_node;

	if (!this) {
		ret = EFI_INVALID_PARAMETER;
		goto out;
	}

	if (!data) {
		efi_free_pool((void *)http_instance.http_load_addr);
		http_instance.current_offset = 0;
		http_instance.configured = false;

		goto out;
	}

	if (http_instance.configured) {
		ret = EFI_ALREADY_STARTED;
		goto out;
	}

	http_version = data->http_version;
	ipv4_node = data->access_point.ipv4_node;

	if ((http_version != HTTPVERSION10	&&
		http_version != HTTPVERSION11)	||
		data->is_ipv6 || !ipv4_node		) { /* Only support ipv4 */
		ret = EFI_UNSUPPORTED;
		goto out;
	}

	// normally client uses default address, and the following is not taken
	if (!ipv4_node->use_default_address) {
		memcpy(&net_ip, &ipv4_node->local_address, sizeof(__be32));
		memcpy(&net_netmask, &ipv4_node->local_subnet, sizeof(__be32));
		/* our_port = ipv4_node->local_port; this is currently
		not accessible and overriden in wget_start */
	}

	memset(&current_http_info, 0, sizeof(struct wget_http_info));

	http_instance.current_offset = 0;
	http_instance.http_size = 0;
	http_instance.configured = true;

out:
	return EFI_EXIT(ret);
}

/*
 * efi_http_request() - Queues an HTTP request to this HTTP instance
 *
 * This function implements EFI_HTTP_PROTOCOL.Request()
 * See the Unified Extensible Firmware Interface
 * (UEFI) specification for details.
 *
 * @this:	pointer to the protocol instance
 * @token:	pointer to storage containing HTTP request token
 * Return:	status code
 */
static efi_status_t EFIAPI efi_http_request(struct efi_http_protocol *this, struct efi_http_token *token)
{
	EFI_ENTRY("%p, %p", this, token);

	efi_status_t ret = EFI_SUCCESS;
	u8 *tmp;
	u8 *url_8;
	u16 *url_16;
	const char *prefix = "http://";
	enum efi_http_method current_method;

	if (!token) {
		ret = EFI_INVALID_PARAMETER;
		goto out;
	}

	if (!http_instance.configured) {
		ret = EFI_NOT_STARTED;
		goto out;
	}

	if (!this || !token->message || !token->message->data.request) {
		goto out_invalid;
	}

	current_method = token->message->data.request->method;
	current_http_info.method = (enum wget_http_method)current_method;
	url_16 = token->message->data.request->url;

	/* Parse URL. It comes in UCS-2 encoding and follows RFC3986 */
	url_8 = calloc(1, 1024);
	tmp = url_8;
	utf16_utf8_strcpy((char **)&tmp, url_16);
	if (strncmp(url_8, prefix, strlen(prefix))) {
		goto out_invalid;
	}

	strcpy(net_boot_file_name, (const char *)(url_8 + strlen(prefix)));

	char *port_num = strchr(net_boot_file_name, ':');
	if (port_num > 0) {
		++port_num;
		http_tcp_port = dectoul(port_num, NULL);
		if (http_tcp_port)
			env_set_ulong("httpdstp", http_tcp_port);
	}

	switch (current_method) {
	case HTTP_METHOD_GET:
	case HTTP_METHOD_HEAD:
		if (current_http_info.content_length < SZ_64K)
			current_http_info.content_length = SZ_64K;

		http_instance.http_size = current_http_info.content_length;

		ret = efi_http_set_buffer(&http_instance);
		if (ret != EFI_SUCCESS)
			goto out;

		// Call wget. This should be abstracted
		image_load_addr = http_instance.http_load_addr;
		current_http_info.set_bootdev = false;
		if (net_loop(WGET) < 0) {
			efi_free_pool((void *)http_instance.http_load_addr);
			goto out_error;
		}

		http_instance.current_offset = 0; // We have a new file
		token->status = EFI_SUCCESS;
		goto out_signal;

		break;
	default:
		ret = EFI_UNSUPPORTED;
		goto out;
		break;
	}

out_invalid:
	ret = EFI_INVALID_PARAMETER;
	token->status = EFI_ABORTED;
	goto out_signal;
out_error:
	ret = EFI_DEVICE_ERROR;
	token->status = EFI_DEVICE_ERROR;
out_signal:
	efi_signal_event(token->event);
out:
	return EFI_EXIT(ret);
}

/*
 * efi_http_cancel() - Abort an asynchronous HTTP request or response token
 *
 * This function implements EFI_HTTP_PROTOCOL.Cancel()
 * See the Unified Extensible Firmware Interface
 * (UEFI) specification for details.
 *
 * @this:	pointer to the protocol instance
 * @token:	pointer to storage containing HTTP request token
 * Return:	status code
 */
static efi_status_t EFIAPI efi_http_cancel(struct efi_http_protocol *this, struct efi_http_token *token)
{
	EFI_ENTRY("%p, %p", this, token);

	efi_status_t ret = EFI_UNSUPPORTED;

	return EFI_EXIT(ret);
}

/*
 * efi_http_response() -  Queues an HTTP response to this HTTP instance
 *
 * This function implements EFI_HTTP_PROTOCOL.Response()
 * See the Unified Extensible Firmware Interface
 * (UEFI) specification for details.
 *
 * @this:	pointer to the protocol instance
 * @token:	pointer to storage containing HTTP request token
 * Return:	status code
 */
static efi_status_t EFIAPI efi_http_response(struct efi_http_protocol *this, struct efi_http_token *token)
{
	EFI_ENTRY("%p, %p", this, token);

	efi_status_t ret = EFI_SUCCESS;

	if (!token) {
		ret = EFI_INVALID_PARAMETER;
		goto out;
	}

	if (!this || !token->message) {
		goto out_invalid;
	}

	// Set HTTP status code
	if (token->message->data.response) // TODO extra check, see spec.
		token->message->data.response->status_code = efi_ul_to_httpstatus(current_http_info.status_code);

	ret = efi_http_send_headers(&token->message->headers, &token->message->header_count);
	if (ret != EFI_SUCCESS)
		goto out;

	ret = efi_http_send_data(&http_instance, token->message->body, &token->message->body_length);
	if (ret != EFI_SUCCESS)
		goto out;

	token->status = EFI_SUCCESS;
	goto out_signal;

out_invalid:
	ret = EFI_INVALID_PARAMETER;
	token->status = EFI_ABORTED;
	goto out_signal;
out_signal:
	efi_signal_event(token->event);
out:
	return EFI_EXIT(ret);
}

/*
 * efi_http_poll() -  Polls for incoming data packets and processes outgoing data packets
 *
 * This function implements EFI_HTTP_PROTOCOL.Poll()
 * See the Unified Extensible Firmware Interface
 * (UEFI) specification for details.
 *
 * @this:	pointer to the protocol instance
 * @token:	pointer to storage containing HTTP request token
 * Return:	status code
 */
static efi_status_t EFIAPI efi_http_poll(struct efi_http_protocol *this)
{
	EFI_ENTRY("%p", this);

	efi_status_t ret = EFI_UNSUPPORTED;

	return EFI_EXIT(ret);
}

/* EFI_HTTP_SERVICE_BINDING_PROTOCOL */

/*
 * efi_http_service_binding_create_child() -  Creates a child handle
 * and installs a protocol
 *
 * This function implements EFI_HTTP_SERVICE_BINDING.CreateChild()
 * See the Unified Extensible Firmware Interface
 * (UEFI) specification for details.
 *
 * @this:		pointer to the protocol instance
 * @child_handle:	pointer to child handle
 * Return:		status code
 */
static efi_status_t EFIAPI efi_http_service_binding_create_child(
			struct efi_service_binding_protocol *this,
			efi_handle_t *child_handle)
{
	EFI_ENTRY("%p, %p", this, child_handle);

	efi_status_t ret = EFI_SUCCESS;

	if (!child_handle)
		return EFI_EXIT(EFI_INVALID_PARAMETER);

	if (num_instances) {
		ret = EFI_OUT_OF_RESOURCES;
		goto failure_to_add_protocol;
	}

	memset(&http_instance, 0, sizeof(struct efi_http_instance));

	http_instance.handle = NULL;

	if (*child_handle) {
			http_instance.handle = *child_handle;
			goto install;
	}

	http_instance.handle = &http_instance.header;

	efi_add_handle(http_instance.handle);
	*child_handle = http_instance.handle;

install:
	ret = efi_add_protocol(http_instance.handle, &efi_http_guid,
				 &http_instance.http);
	if (ret != EFI_SUCCESS)
		goto failure_to_add_protocol;

	http_instance.http.get_mode_data = efi_http_get_mode_data;
	http_instance.http.configure = efi_http_configure;
	http_instance.http.request = efi_http_request;
	http_instance.http.cancel = efi_http_cancel;
	http_instance.http.response = efi_http_response;
	http_instance.http.poll = efi_http_poll;
	++num_instances;

	return EFI_EXIT(EFI_SUCCESS);
failure_to_add_protocol:
	printf("ERROR: Failure to add protocol\n");
	return EFI_EXIT(ret);
}

/*
 * efi_http_service_binding_destroy_child() -  Destroys a child handle with
 * a protocol installed on it
 *
 * This function implements EFI_HTTP_SERVICE_BINDING.DestroyChild()
 * See the Unified Extensible Firmware Interface
 * (UEFI) specification for details.
 *
 * @this:		pointer to the protocol instance
 * @child_handle:	child handle
 * Return:		status code
 */
static efi_status_t EFIAPI efi_http_service_binding_destroy_child(
			struct efi_service_binding_protocol *this,
			efi_handle_t child_handle)
{
	EFI_ENTRY("%p, %p", this, child_handle);

	efi_status_t ret;

	if (num_instances == 0)
		return EFI_EXIT(EFI_NOT_FOUND);

	if (!child_handle)
		return EFI_EXIT(EFI_INVALID_PARAMETER);

	ret = efi_delete_handle(child_handle);
	if (ret != EFI_SUCCESS) {
		printf("ERROR: Failure to remove protocol\n");
		return EFI_EXIT(ret);
	}

	num_instances--;
	return EFI_EXIT(EFI_SUCCESS);
}

/* EFI_IP4_CONFIG2_PROTOCOL */

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
static efi_status_t EFIAPI efi_ip4_config2_set_data(
	struct efi_ip4_config2_protocol *this,
	enum efi_ip4_config2_data_type data_type,
	efi_uintn_t data_size,
	void *data)
{
	EFI_ENTRY("%p, %d, %lu, %p", this, data_type, data_size, data);
	efi_status_t ret = EFI_SUCCESS;

	if (!this)
		return EFI_EXIT(EFI_INVALID_PARAMETER);

	switch (data_type) {
	case EFI_IP4_CONFIG2_DATA_TYPE_INTERFACEINFO:
		return EFI_EXIT(EFI_WRITE_PROTECTED);
		break;
	case EFI_IP4_CONFIG2_DATA_TYPE_MANUAL_ADDRESS:
		if (current_policy != EFI_IP4_CONFIG2_POLICY_STATIC)
			return EFI_EXIT(EFI_WRITE_PROTECTED);
		if (data_size == 0 && data == NULL) {
			memset((void *)&current_http_ip, 0, sizeof(struct efi_ip4_config2_manual_address));
			return EFI_EXIT(EFI_SUCCESS);
		}
		if (data && data_size == sizeof(struct efi_ip4_config2_manual_address)) {
			memcpy((void *)&current_http_ip, data, sizeof(struct efi_ip4_config2_manual_address));
			memcpy((void *)&net_ip, (void *)current_http_ip.address.ip_addr, 4);
			memcpy((void *)&net_netmask, (void *)current_http_ip.subnet_mask.ip_addr, 4);

			return EFI_EXIT(EFI_SUCCESS);
		}
		return EFI_EXIT(EFI_INVALID_PARAMETER);
		break;
	case EFI_IP4_CONFIG2_DATA_TYPE_POLICY:
			if (data && data_size == sizeof(enum efi_ip4_config2_policy)) {
				current_policy = *(enum efi_ip4_config2_policy *)data;
				return EFI_EXIT(EFI_SUCCESS);
			}
		return EFI_EXIT(EFI_INVALID_PARAMETER);

	default:
		return EFI_EXIT(EFI_UNSUPPORTED);
	break;
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
static efi_status_t EFIAPI efi_ip4_config2_get_data(
	struct efi_ip4_config2_protocol *this,
	enum efi_ip4_config2_data_type data_type,
	efi_uintn_t *data_size,
	void *data)
{
	EFI_ENTRY("%p, %d, %p, %p", this, data_type, data_size, data);

	efi_status_t ret = EFI_SUCCESS;

	if (!this || !data_size)
		return EFI_EXIT(EFI_INVALID_PARAMETER);

	if (*data_size && !data)
		return EFI_EXIT(EFI_INVALID_PARAMETER);

	switch (data_type) {
	case EFI_IP4_CONFIG2_DATA_TYPE_INTERFACEINFO:
		struct efi_ip4_config2_interface_info *info;

		if (*data_size < (sizeof(struct efi_ip4_config2_interface_info) + sizeof(struct efi_ip4_route_table))) {
			*data_size = (sizeof(struct efi_ip4_config2_interface_info) + sizeof(struct efi_ip4_route_table));
			return EFI_EXIT(EFI_BUFFER_TOO_SMALL);
		}

		info = (struct efi_ip4_config2_interface_info *)data;
		memset(info, 0, sizeof(struct efi_ip4_config2_interface_info));

		info->hw_address_size = 6;
		memcpy(info->hw_address.mac_addr, http_current_mac_addr, 6);
		// Set the route table size

		info->route_table_size = 0;
		/* Mistake in grub causes segfault on this
		info->route_table = (struct efi_ip4_route_table *)((char *)info + sizeof(struct efi_ip4_config2_interface_info));

		memset(info->route_table, 0, sizeof(struct efi_ip4_route_table));

		memcpy(&info->route_table->subnet_address, &net_ip, sizeof(struct efi_ipv4_address));
		memcpy(&info->route_table->subnet_mask, &net_netmask, sizeof(struct efi_ipv4_address));
		*/
		break;
	case EFI_IP4_CONFIG2_DATA_TYPE_MANUAL_ADDRESS:
		if (*data_size < sizeof(struct efi_ip4_config2_manual_address)) {
			*data_size = sizeof(struct efi_ip4_config2_manual_address);
			return EFI_EXIT(EFI_BUFFER_TOO_SMALL);
		}

		memcpy((void *)current_http_ip.address.ip_addr,(void *)&net_ip, 4);
		memcpy((void *)current_http_ip.subnet_mask.ip_addr, (void *)&net_netmask, 4);
		memcpy(data, (void *)&current_http_ip, sizeof(struct efi_ip4_config2_manual_address));

		break;
	default:
		return EFI_EXIT(EFI_UNSUPPORTED);
	break;
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
static efi_status_t EFIAPI efi_ip4_config2_register_notify(
	struct efi_ip4_config2_protocol *this,
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
static efi_status_t EFIAPI efi_ip4_config2_unregister_notify(
	struct efi_ip4_config2_protocol *this,
	enum efi_ip4_config2_data_type data_type,
	struct efi_event *event)
{
	EFI_ENTRY("%p, %d, %p", this, data_type, event);

	return EFI_EXIT(EFI_UNSUPPORTED);
}

/**
 * efi_http_register() - register the http protocol
 *
 */
efi_status_t efi_http_register(const efi_handle_t handle,
			       struct efi_service_binding_protocol *http_service_binding,
			       struct efi_ip4_config2_protocol *ip4config)
{
	efi_status_t r = EFI_SUCCESS;

	r = efi_add_protocol(handle, &efi_ip4_config2_guid,
				 ip4config);
	if (r != EFI_SUCCESS)
		goto failure_to_add_protocol;

	memcpy(http_current_mac_addr, eth_get_ethaddr(), 6);

	ip4config->set_data = efi_ip4_config2_set_data;
	ip4config->get_data = efi_ip4_config2_get_data;
	ip4config->register_data_notify = efi_ip4_config2_register_notify;
	ip4config->unregister_data_notify = efi_ip4_config2_unregister_notify;

	r = efi_add_protocol(handle, &efi_http_service_binding_guid,
				 http_service_binding);
	if (r != EFI_SUCCESS)
		goto failure_to_add_protocol;

	http_service_binding->create_child = efi_http_service_binding_create_child;
	http_service_binding->destroy_child = efi_http_service_binding_destroy_child;

	return EFI_SUCCESS;
failure_to_add_protocol:
	printf("ERROR: Failure to add protocol\n");
	return r;
}


enum efi_http_status_code efi_ul_to_httpstatus(ulong status) {
	switch (status) {
		case 100: return HTTP_STATUS_100_CONTINUE;
		case 101: return HTTP_STATUS_101_SWITCHING_PROTOCOLS;
		case 200: return HTTP_STATUS_200_OK;
		case 201: return HTTP_STATUS_201_CREATED;
		case 202: return HTTP_STATUS_202_ACCEPTED;
		case 203: return HTTP_STATUS_203_NON_AUTHORITATIVE_INFORMATION;
		case 204: return HTTP_STATUS_204_NO_CONTENT;
		case 205: return HTTP_STATUS_205_RESET_CONTENT;
		case 206: return HTTP_STATUS_206_PARTIAL_CONTENT;
		case 300: return HTTP_STATUS_300_MULTIPLE_CHOICES;
		case 301: return HTTP_STATUS_301_MOVED_PERMANENTLY;
		case 302: return HTTP_STATUS_302_FOUND;
		case 303: return HTTP_STATUS_303_SEE_OTHER;
		case 304: return HTTP_STATUS_304_NOT_MODIFIED;
		case 305: return HTTP_STATUS_305_USE_PROXY;
		case 307: return HTTP_STATUS_307_TEMPORARY_REDIRECT;
		case 400: return HTTP_STATUS_400_BAD_REQUEST;
		case 401: return HTTP_STATUS_401_UNAUTHORIZED;
		case 402: return HTTP_STATUS_402_PAYMENT_REQUIRED;
		case 403: return HTTP_STATUS_403_FORBIDDEN;
		case 404: return HTTP_STATUS_404_NOT_FOUND;
		case 405: return HTTP_STATUS_405_METHOD_NOT_ALLOWED;
		case 406: return HTTP_STATUS_406_NOT_ACCEPTABLE;
		case 407: return HTTP_STATUS_407_PROXY_AUTHENTICATION_REQUIRED;
		case 408: return HTTP_STATUS_408_REQUEST_TIME_OUT;
		case 409: return HTTP_STATUS_409_CONFLICT;
		case 410: return HTTP_STATUS_410_GONE;
		case 411: return HTTP_STATUS_411_LENGTH_REQUIRED;
		case 412: return HTTP_STATUS_412_PRECONDITION_FAILED;
		case 413: return HTTP_STATUS_413_REQUEST_ENTITY_TOO_LARGE;
		case 414: return HTTP_STATUS_414_REQUEST_URI_TOO_LARGE;
		case 415: return HTTP_STATUS_415_UNSUPPORTED_MEDIA_TYPE;
		case 416: return HTTP_STATUS_416_REQUESTED_RANGE_NOT_SATISFIED;
		case 417: return HTTP_STATUS_417_EXPECTATION_FAILED;
		case 500: return HTTP_STATUS_500_INTERNAL_SERVER_ERROR;
		case 501: return HTTP_STATUS_501_NOT_IMPLEMENTED;
		case 502: return HTTP_STATUS_502_BAD_GATEWAY;
		case 503: return HTTP_STATUS_503_SERVICE_UNAVAILABLE;
		case 504: return HTTP_STATUS_504_GATEWAY_TIME_OUT;
		case 505: return HTTP_STATUS_505_HTTP_VERSION_NOT_SUPPORTED;
		case 308: return HTTP_STATUS_308_PERMANENT_REDIRECT;
		default: return HTTP_STATUS_UNSUPPORTED_STATUS;
	}
}