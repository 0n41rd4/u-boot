// SPDX-License-Identifier: GPL-2.0+
/*
 *
 * Copyright (c) 2016 Alexander Graf
 *
 */

#define LOG_CATEGORY LOGC_EFI

#include <efi_device_path.h>
#include <efi_loader.h>
#include <dm.h>
#include <linux/sizes.h>
#include <malloc.h>
#include <vsprintf.h>
#include <net.h>

#define MAX_EFI_NET_OBJS 10
#define MAX_NUM_DHCP_ENTRIES 10
#define MAX_NUM_DP_ENTRIES 10

static const efi_guid_t efi_pxe_base_code_protocol_guid =
					EFI_PXE_BASE_CODE_PROTOCOL_GUID;
#if IS_ENABLED(CONFIG_EFI_HTTP_PROTOCOL)
static const efi_guid_t efi_http_service_binding_guid =
					EFI_HTTP_SERVICE_BINDING_PROTOCOL_GUID;
#endif

struct dp_entry {
	struct efi_device_path *net_dp;
	struct udevice *dev;
	bool is_valid;
};

/*
 * The network device path cache. An entry is added when a new bootfile
 * is downloaded from the network. If the bootfile is then loaded as an
 * efi image, the most recent entry corresponding to the device is passed
 * as the device path of the loaded image.
 */
static struct dp_entry dp_cache[MAX_NUM_DP_ENTRIES];
static int next_dp_entry;

#if IS_ENABLED(CONFIG_EFI_HTTP_PROTOCOL)
static struct wget_http_info efi_wget_info = {
	.set_bootdev = false,
	.check_buffer_size = true,
	.silent = true,
};
#endif

struct dhcp_entry {
	struct efi_pxe_packet *dhcp_ack;
	struct udevice *dev;
	bool is_valid;
};

static struct dhcp_entry dhcp_cache[MAX_NUM_DHCP_ENTRIES];
static int next_dhcp_entry;

static efi_handle_t net_objs[MAX_EFI_NET_OBJS];

/**
 * efi_net_set_dhcp_ack() - take note of a selected DHCP IP address
 *
 * This function is called by dhcp_handler().
 *
 * @pkt:	packet received by dhcp_handler()
 * @len:	length of the packet received
 */
void efi_net_set_dhcp_ack(void *pkt, int len)
{
	efi_status_t r = EFI_SUCCESS;
	struct efi_pxe_packet **dhcp_ack;
	struct efi_handler *phandler;
	struct efi_pxe_base_code_protocol *pxe;
	struct udevice *dev;
	int i;

	dhcp_ack = &dhcp_cache[next_dhcp_entry].dhcp_ack;

	/* For now this function gets called only by the current device */
	dev = eth_get_dev();

	int maxsize = sizeof(**dhcp_ack);

	if (!*dhcp_ack) {
		*dhcp_ack = malloc(maxsize);
		if (!*dhcp_ack)
			return;
	}
	memset(*dhcp_ack, 0, maxsize);
	memcpy(*dhcp_ack, pkt, min(len, maxsize));

	dhcp_cache[next_dhcp_entry].is_valid = true;
	dhcp_cache[next_dhcp_entry].dev = dev;
	next_dhcp_entry++;
	next_dhcp_entry %= MAX_NUM_DHCP_ENTRIES;

	for (i = 0; i < MAX_EFI_NET_OBJS; i++) {
		if (net_objs[i] && net_objs[i]->dev == dev) {
			phandler = NULL;
			r = efi_search_protocol(net_objs[i],
						&efi_pxe_base_code_protocol_guid,
						&phandler);
			if (r == EFI_SUCCESS && phandler) {
				pxe = phandler->protocol_interface;
				pxe->mode->dhcp_ack = **dhcp_ack;
			}
			break;
		}
	}
}

/**
 * efi_netobj_set_dp() - set device path of a netobj
 *
 * @netobj:	handle of an EFI net device
 * @dp:		device path to set, allocated by caller
 * Return:	status code
 */
efi_status_t efi_netobj_set_dp(efi_handle_t netobj, struct efi_device_path *dp)
{
	efi_status_t ret;
	struct efi_handler *phandler;
	struct efi_device_path *new_net_dp;

	if (!efi_search_obj(netobj))
		return EFI_SUCCESS;

	// Create a device path for the netobj
	new_net_dp = dp;
	if (!new_net_dp)
		return EFI_OUT_OF_RESOURCES;

	phandler = NULL;
	efi_search_protocol(netobj, &efi_guid_device_path, &phandler);

	// If the device path protocol is not yet installed, install it
	if (!phandler)
		goto add;

	// If it is already installed, try to update it
	ret = efi_reinstall_protocol_interface(netobj, &efi_guid_device_path,
					       phandler->protocol_interface, new_net_dp);
	if (ret != EFI_SUCCESS)
		return ret;

	return EFI_SUCCESS;
add:
	ret = efi_add_protocol(netobj, &efi_guid_device_path,
			       new_net_dp);
	if (ret != EFI_SUCCESS)
		return ret;

	return EFI_SUCCESS;
}

/**
 * efi_netobj_get_dp() - get device path of a netobj
 *
 * @netobj:	handle to EFI net object
 * Return:	device path, NULL on error
 */
static struct efi_device_path *efi_netobj_get_dp(efi_handle_t netobj)
{
	struct efi_handler *phandler;

	if (!efi_search_obj(netobj))
		return NULL;

	phandler = NULL;
	efi_search_protocol(netobj, &efi_guid_device_path, &phandler);

	if (phandler && phandler->protocol_interface)
		return efi_dp_dup(phandler->protocol_interface);

	return NULL;
}

/**
 * efi_net_do_start() - start the efi network stack
 *
 * This gets called from do_bootefi_exec() each time a payload gets executed.
 *
 * @dev:	net udevice
 * Return:	status code
 */
efi_status_t efi_net_do_start(struct udevice *dev)
{
	efi_status_t r = EFI_SUCCESS;
	efi_handle_t netobj;
	struct efi_device_path *net_dp;
#if IS_ENABLED(CONFIG_EFI_HTTP_PROTOCOL)
	struct efi_handler *phandler;
	struct efi_pxe_base_code_protocol *pxe;
#endif
	int i;

	netobj = NULL;
	for (i = 0; i < MAX_EFI_NET_OBJS; i++) {
		if (net_objs[i] && net_objs[i]->dev == dev) {
			netobj = net_objs[i];
			break;
		}
	}

	if (!efi_search_obj(netobj))
		return r;

	efi_net_dp_from_dev(&net_dp, netobj->dev, true);
	// If no dp cache entry applies and there already
	// is a device path installed, continue
	if (!net_dp) {
		if (efi_netobj_get_dp(netobj))
			goto set_addr;
		else
			net_dp = efi_dp_from_eth(netobj->dev);

	}

	if (!net_dp)
		return EFI_OUT_OF_RESOURCES;

	r = efi_netobj_set_dp(netobj, net_dp);
	if (r != EFI_SUCCESS)
		return r;
set_addr:
#if IS_ENABLED(CONFIG_EFI_HTTP_PROTOCOL)
	/*
	 * No harm on doing the following. If the PXE handle is present, the client could
	 * find it and try to get its IP address from it. In here the PXE handle is present
	 * but the PXE protocol is not yet implmenented, so we add this in the meantime.
	 */
	pxe = NULL;
	r = efi_search_protocol(netobj,
				&efi_pxe_base_code_protocol_guid,
				&phandler);
	if (r == EFI_SUCCESS && phandler) {
		pxe = phandler->protocol_interface;
		efi_net_get_addr((struct efi_ipv4_address *)&pxe->mode->station_ip,
				 (struct efi_ipv4_address *)&pxe->mode->subnet_mask, NULL, dev);
	}
#endif

	return EFI_SUCCESS;
}

/**
 * efi_net_register() - register the simple network protocol
 *
 * This gets called from do_bootefi_exec().
 * @dev:	net udevice
 */
efi_status_t efi_net_register(struct udevice *dev)
{
	efi_status_t r;
	int seq_num;
	efi_handle_t netobj;
	struct efi_pxe_packet *dhcp_ack;
	int i, j;

	if (!dev) {
		/* No network device active, don't expose any */
		return EFI_SUCCESS;
	}

	for (i = 0; i < MAX_EFI_NET_OBJS; i++) {
		if (net_objs[i] && net_objs[i]->dev == dev) {
			// Do not register duplicate devices
			return EFI_SUCCESS;
		}
	}

	seq_num = -1;
	for (i = 0; i < MAX_EFI_NET_OBJS; i++) {
		if (!net_objs[i]) {
			seq_num = i;
			break;
		}
	}
	if (seq_num < 0)
		return EFI_OUT_OF_RESOURCES;

	/* We only expose the "active" network device, so one is enough */
	netobj = calloc(1, sizeof(*netobj));
	if (!netobj) {
		printf("ERROR: Out of memory\n");
		return EFI_OUT_OF_RESOURCES;
	}

	/* Hook net up to the device list */
	efi_add_handle(netobj);

	if (efi_link_dev(netobj, dev) < 0)
		return EFI_DEVICE_ERROR;

	/* Install EFI_SIMPLE_NETWORK_PROTOCOL */
	r = efi_simple_network_install(netobj);
	if (r != EFI_SUCCESS)
		goto failure_to_add_protocol;

	/*
	 * Scan dhcp entries for one corresponding
	 * to this udevice, from newest to oldest
	 */
	dhcp_ack = NULL;
	i = (next_dhcp_entry + MAX_NUM_DHCP_ENTRIES - 1) % MAX_NUM_DHCP_ENTRIES;
	for (j = 0; dhcp_cache[i].is_valid && j < MAX_NUM_DHCP_ENTRIES;
	     i = (i + MAX_NUM_DHCP_ENTRIES - 1) % MAX_NUM_DHCP_ENTRIES, j++) {
		if (dev == dhcp_cache[i].dev) {
			dhcp_ack = dhcp_cache[i].dhcp_ack;
			break;
		}
	}

	/* Install EFI_PXE_BASE_CODE_PROTOCOL */
	r = efi_pxe_install(netobj, dhcp_ack);
	if (r != EFI_SUCCESS)
		goto failure_to_add_protocol;

#if IS_ENABLED(CONFIG_EFI_IP4_CONFIG2_PROTOCOL)
	/* Install EFI_IP4_CONFIG2_PROTOCOL */
	r = efi_ip4_config2_install(netobj);
	if (r != EFI_SUCCESS)
		goto failure_to_add_protocol;
#endif
#ifdef CONFIG_EFI_HTTP_PROTOCOL
	/* Install EFI_HTTP_PROTOCOL */
	r = efi_http_install(netobj);
	if (r != EFI_SUCCESS)
		goto failure_to_add_protocol;
#endif

	net_objs[seq_num] = netobj;
	return EFI_SUCCESS;
failure_to_add_protocol:
	printf("ERROR: Failure to add protocol\n");
	return r;
}

/**
 * efi_net_new_dp() - update device path associated to a net udevice
 *
 * This gets called to update the device path when a new boot
 * file is downloaded
 *
 * @dev:	dev to set the device path from
 * @server:	remote server address
 * @udev:	net udevice
 * Return:	status code
 */
efi_status_t efi_net_new_dp(const char *dev, const char *server, struct udevice *udev)
{
	efi_status_t ret;
	efi_handle_t netobj;
	struct efi_device_path *old_net_dp, *new_net_dp;
	struct efi_device_path **dp;
	int i;

	dp = &dp_cache[next_dp_entry].net_dp;

	dp_cache[next_dp_entry].dev = udev;
	dp_cache[next_dp_entry].is_valid = true;
	next_dp_entry++;
	next_dp_entry %= MAX_NUM_DP_ENTRIES;

	old_net_dp = *dp;
	new_net_dp = NULL;
	if (!strcmp(dev, "Net"))
		new_net_dp = efi_dp_from_eth(udev);
	else if (!strcmp(dev, "Http"))
		new_net_dp = efi_dp_from_http(server, udev);
	if (!new_net_dp)
		return EFI_OUT_OF_RESOURCES;

	*dp = new_net_dp;
	// Free the old cache entry
	efi_free_pool(old_net_dp);

	netobj = NULL;
	for (i = 0; i < MAX_EFI_NET_OBJS; i++) {
		if (net_objs[i] && net_objs[i]->dev == udev) {
			netobj = net_objs[i];
			break;
		}
	}
	if (!netobj)
		return EFI_SUCCESS;

	new_net_dp = efi_dp_dup(*dp);
	if (!new_net_dp)
		return EFI_OUT_OF_RESOURCES;
	ret = efi_netobj_set_dp(netobj, new_net_dp);
	if (ret != EFI_SUCCESS)
		efi_free_pool(new_net_dp);

	return ret;
}

/**
 * efi_net_dp_from_dev() - get device path associated to a net udevice
 *
 * Produce a copy of the current device path
 *
 * @dp:		copy of the current device path
 * @udev:	net udevice
 * @cache_only:	get device path from cache only
 */
void efi_net_dp_from_dev(struct efi_device_path **dp, struct udevice *udev, bool cache_only)
{
	int i, j;

	if (!dp)
		return;

	*dp = NULL;

	if (cache_only)
		goto cache;

	// If a netobj matches:
	for (i = 0; i < MAX_EFI_NET_OBJS; i++) {
		if (net_objs[i] && net_objs[i]->dev == udev) {
			*dp = efi_netobj_get_dp(net_objs[i]);
			if (*dp)
				return;
		}
	}
cache:
	// Search in the cache
	i = (next_dp_entry + MAX_NUM_DP_ENTRIES - 1) % MAX_NUM_DP_ENTRIES;
	for (j = 0; dp_cache[i].is_valid && j < MAX_NUM_DP_ENTRIES;
		i = (i + MAX_NUM_DP_ENTRIES - 1) % MAX_NUM_DP_ENTRIES, j++) {
		if (dp_cache[i].dev == udev) {
			*dp = efi_dp_dup(dp_cache[i].net_dp);
			return;
		}
	}
}

/**
 * efi_net_get_addr() - get IP address information
 *
 * Copy the current IP address, mask, and gateway into the
 * efi_ipv4_address structs pointed to by ip, mask and gw,
 * respectively.
 *
 * @ip:		pointer to an efi_ipv4_address struct to
 *		be filled with the current IP address
 * @mask:	pointer to an efi_ipv4_address struct to
 *		be filled with the current network mask
 * @gw:		pointer to an efi_ipv4_address struct to be
 *		filled with the current network gateway
 * @dev:	udevice
 */
void efi_net_get_addr(struct efi_ipv4_address *ip,
		      struct efi_ipv4_address *mask,
		      struct efi_ipv4_address *gw,
		      struct udevice *dev)
{
	if (!dev)
		dev = eth_get_dev();
#ifdef CONFIG_NET_LWIP
	char ipstr[] = "ipaddr\0\0";
	char maskstr[] = "netmask\0\0";
	char gwstr[] = "gatewayip\0\0";
	int idx;
	struct in_addr tmp;
	char *env;

	idx = dev_seq(dev);

	if (idx < 0 || idx > 99) {
		log_err("unexpected idx %d\n", idx);
		return;
	}

	if (idx) {
		sprintf(ipstr, "ipaddr%d", idx);
		sprintf(maskstr, "netmask%d", idx);
		sprintf(gwstr, "gatewayip%d", idx);
	}

	env = env_get(ipstr);
	if (env && ip) {
		tmp = string_to_ip(env);
		memcpy(ip, &tmp, sizeof(tmp));
	}

	env = env_get(maskstr);
	if (env && mask) {
		tmp = string_to_ip(env);
		memcpy(mask, &tmp, sizeof(tmp));
	}
	env = env_get(gwstr);
	if (env && gw) {
		tmp = string_to_ip(env);
		memcpy(gw, &tmp, sizeof(tmp));
	}
#else
	if (ip)
		memcpy(ip, &net_ip, sizeof(net_ip));
	if (mask)
		memcpy(mask, &net_netmask, sizeof(net_netmask));
#endif
}

/**
 * efi_net_set_addr() - set IP address information
 *
 * Set the current IP address, mask, and gateway to the
 * efi_ipv4_address structs pointed to by ip, mask and gw,
 * respectively.
 *
 * @ip:		pointer to new IP address
 * @mask:	pointer to new network mask to set
 * @gw:		pointer to new network gateway
 * @dev:	udevice
 */
void efi_net_set_addr(struct efi_ipv4_address *ip,
		      struct efi_ipv4_address *mask,
		      struct efi_ipv4_address *gw,
		      struct udevice *dev)
{
	if (!dev)
		dev = eth_get_dev();
#ifdef CONFIG_NET_LWIP
	char ipstr[] = "ipaddr\0\0";
	char maskstr[] = "netmask\0\0";
	char gwstr[] = "gatewayip\0\0";
	int idx;
	struct in_addr *addr;
	char tmp[46];

	idx = dev_seq(dev);

	if (idx < 0 || idx > 99) {
		log_err("unexpected idx %d\n", idx);
		return;
	}

	if (idx) {
		sprintf(ipstr, "ipaddr%d", idx);
		sprintf(maskstr, "netmask%d", idx);
		sprintf(gwstr, "gatewayip%d", idx);
	}

	if (ip) {
		addr = (struct in_addr *)ip;
		ip_to_string(*addr, tmp);
		env_set(ipstr, tmp);
	}

	if (mask) {
		addr = (struct in_addr *)mask;
		ip_to_string(*addr, tmp);
		env_set(maskstr, tmp);
	}

	if (gw) {
		addr = (struct in_addr *)gw;
		ip_to_string(*addr, tmp);
		env_set(gwstr, tmp);
	}
#else
	if (ip)
		memcpy(&net_ip, ip, sizeof(*ip));
	if (mask)
		memcpy(&net_netmask, mask, sizeof(*mask));
#endif
}

#if IS_ENABLED(CONFIG_EFI_HTTP_PROTOCOL)
/**
 * efi_net_set_buffer() - allocate a buffer of min 64K
 *
 * @buffer:	allocated buffer
 * @size:	desired buffer size
 * Return:	status code
 */
static efi_status_t efi_net_set_buffer(void **buffer, size_t size)
{
	efi_status_t ret = EFI_SUCCESS;

	if (size < SZ_64K)
		size = SZ_64K;

	*buffer = efi_alloc(size);
	if (!*buffer)
		ret = EFI_OUT_OF_RESOURCES;

	efi_wget_info.buffer_size = (ulong)size;

	return ret;
}

/**
 * efi_net_parse_headers() - parse HTTP headers
 *
 * Parses the raw buffer efi_wget_info.headers into an array headers
 * of efi structs http_headers. The array should be at least
 * MAX_HTTP_HEADERS long.
 *
 * @num_headers:	number of headers
 * @headers:		caller provided array of struct http_headers
 */
void efi_net_parse_headers(ulong *num_headers, struct http_header *headers)
{
	if (!num_headers || !headers)
		return;

	// Populate info with http headers.
	*num_headers = 0;
	const uchar *line_start = efi_wget_info.headers;
	const uchar *line_end;
	ulong count;
	struct http_header *current_header;
	const uchar *separator;
	size_t name_length, value_length;

	// Skip the first line (request or status line)
	line_end = strstr(line_start, "\r\n");

	if (line_end)
		line_start = line_end + 2;

	while ((line_end = strstr(line_start, "\r\n")) != NULL) {
		count = *num_headers;
		if (line_start == line_end || count >= MAX_HTTP_HEADERS)
			break;
		current_header = headers + count;
		separator = strchr(line_start, ':');
		if (separator) {
			name_length = separator - line_start;
			++separator;
			while (*separator == ' ')
				++separator;
			value_length = line_end - separator;
			if (name_length < MAX_HTTP_HEADER_NAME &&
			    value_length < MAX_HTTP_HEADER_VALUE) {
				strncpy(current_header->name, line_start, name_length);
				current_header->name[name_length] = '\0';
				strncpy(current_header->value, separator, value_length);
				current_header->value[value_length] = '\0';
				(*num_headers)++;
			}
		}
		line_start = line_end + 2;
	}
}

/**
 * efi_net_do_request() - issue an HTTP request using wget
 *
 * @url:		url
 * @method:		HTTP method
 * @buffer:		data buffer
 * @status_code:	HTTP status code
 * @file_size:		file size in bytes
 * @headers_buffer:	headers buffer
 * @parent:		service binding protocol
 * Return:		status code
 */
efi_status_t efi_net_do_request(u8 *url, enum efi_http_method method, void **buffer,
				u32 *status_code, ulong *file_size, char *headers_buffer,
				struct efi_service_binding_protocol *parent)
{
	efi_status_t ret = EFI_SUCCESS;
	int wget_ret;
	static bool last_head;
	struct udevice *dev;
	struct efi_handler *phandler;
	int i;

	if (!buffer || !file_size || !parent)
		return EFI_ABORTED;

	efi_wget_info.method = (enum wget_http_method)method;
	efi_wget_info.headers = headers_buffer;

	// Set corresponding udevice
	dev = NULL;
	for (i = 0; i < MAX_EFI_NET_OBJS; i++) {
		if (!net_objs[i])
			continue;

		ret = efi_search_protocol(net_objs[i],
					  &efi_http_service_binding_guid,
					  &phandler);
		if (ret == EFI_SUCCESS && phandler &&
		    phandler->protocol_interface == (void *)parent) {
			dev = net_objs[i]->dev;
			break;
		}
	}
	if (!dev)
		return EFI_ABORTED;

	switch (method) {
	case HTTP_METHOD_GET:
		ret = efi_net_set_buffer(buffer, last_head ? (size_t)efi_wget_info.hdr_cont_len : 0);
		if (ret != EFI_SUCCESS)
			goto out;
		eth_set_dev(dev);
		env_set("ethact", eth_get_name());
		wget_ret = wget_request((ulong)*buffer, url, &efi_wget_info);
		if ((ulong)efi_wget_info.hdr_cont_len > efi_wget_info.buffer_size) {
			// Try again with updated buffer size
			efi_free_pool(*buffer);
			ret = efi_net_set_buffer(buffer, (size_t)efi_wget_info.hdr_cont_len);
			if (ret != EFI_SUCCESS)
				goto out;
			eth_set_dev(dev);
			env_set("ethact", eth_get_name());
			if (wget_request((ulong)*buffer, url, &efi_wget_info)) {
				efi_free_pool(*buffer);
				ret = EFI_DEVICE_ERROR;
				goto out;
			}
		} else if (wget_ret) {
			efi_free_pool(*buffer);
			ret = EFI_DEVICE_ERROR;
			goto out;
		}
		// Pass the actual number of received bytes to the application
		*file_size = efi_wget_info.file_size;
		*status_code = efi_wget_info.status_code;
		last_head = false;
		break;
	case HTTP_METHOD_HEAD:
		ret = efi_net_set_buffer(buffer, 0);
		if (ret != EFI_SUCCESS)
			goto out;
		eth_set_dev(dev);
		env_set("ethact", eth_get_name());
		wget_request((ulong)*buffer, url, &efi_wget_info);
		*file_size = 0;
		*status_code = efi_wget_info.status_code;
		last_head = true;
		break;
	default:
		ret = EFI_UNSUPPORTED;
		break;
	}

out:
	return ret;
}
#endif
