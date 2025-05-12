// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * netdump.efi tests EFI network protocols
 *
 * Specifying 'nocolor' as load option data suppresses colored output and
 * clearing of the screen.
 */

#include <efi_api.h>
#include <charset.h>
#include <net.h>

#define BUFFER_SIZE 64
#define ESC 0x17

#define efi_size_in_pages(size) (((size) + EFI_PAGE_MASK) >> EFI_PAGE_SHIFT)

static struct efi_system_table *systable;
static struct efi_boot_services *bs;
static struct efi_simple_text_output_protocol *cerr;
static struct efi_simple_text_output_protocol *cout;
static struct efi_simple_text_input_protocol *cin;

static efi_handle_t http_protocol_handle;
static int callback_done;

static const efi_guid_t loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static const efi_guid_t efi_http_guid = EFI_HTTP_PROTOCOL_GUID;
static const efi_guid_t efi_http_service_binding_guid = EFI_HTTP_SERVICE_BINDING_PROTOCOL_GUID;
static const efi_guid_t efi_ip4_config2_guid = EFI_IP4_CONFIG2_PROTOCOL_GUID;

static efi_handle_t handle;
static efi_handle_t net_handle;

static bool nocolor;

/**
 * color() - set foreground color
 *
 * @color:	foreground color
 */
static void color(u8 color)
{
	if (!nocolor)
		cout->set_attribute(cout, color | EFI_BACKGROUND_BLACK);
}

/**
 * print() - print string
 *
 * @string:	text
 */
static void print(u16 *string)
{
	cout->output_string(cout, string);
}

/**
 * cls() - clear screen
 */
static void cls(void)
{
	if (nocolor)
		print(u"\r\n");
	else
		cout->clear_screen(cout);
}

/**
 * error() - print error string
 *
 * @string:	error text
 */
static void error(u16 *string)
{
	color(EFI_LIGHTRED);
	print(string);
	color(EFI_LIGHTBLUE);
}

/*
 * printx() - print hexadecimal number
 *
 * @val:	value to print;
 * @prec:	minimum number of digits to print
 */
static void printx(u64 val, u32 prec)
{
	int i;
	u16 c;
	u16 buf[16];
	u16 *pos = buf;

	for (i = 2 * sizeof(val) - 1; i >= 0; --i) {
		c = (val >> (4 * i)) & 0x0f;
		if (c || pos != buf || !i || i < prec) {
			c += '0';
			if (c > '9')
				c += 'a' - '9' - 1;
			*pos++ = c;
		}
	}
	*pos = 0;
	print(buf);
}

/*
 * printd() - print decimal byte
 *
 * @val:	value to print;
 */
static void printd(u64 val)
{
	u16 buf[21];
	u16 tmp;
	int i = 0;

	do {
		buf[i++] = '0' + (val % 10);
		val /= 10;
	} while (val > 0);

	for (int j = 0; j < i / 2; ++j) {
		tmp = buf[j];
		buf[j] = buf[i - 1 - j];
		buf[i - 1 - j] = tmp;
	}

	buf[i] = 0;
	print(buf);
}

/**
 * efi_drain_input() - drain console input
 */
static void efi_drain_input(void)
{
	cin->reset(cin, true);
}

/**
 * efi_input() - read string from console
 *
 * @buffer:		input buffer
 * @buffer_size:	buffer size
 * Return:		status code
 */
static efi_status_t efi_input(u16 *buffer, efi_uintn_t buffer_size)
{
	struct efi_input_key key = {0};
	efi_uintn_t index;
	efi_uintn_t pos = 0;
	u16 outbuf[2] = u" ";
	efi_status_t ret;

	*buffer = 0;
	for (;;) {
		ret = bs->wait_for_event(1, &cin->wait_for_key, &index);
		if (ret != EFI_SUCCESS)
			continue;
		ret = cin->read_key_stroke(cin, &key);
		if (ret != EFI_SUCCESS)
			continue;
		switch (key.scan_code) {
		case 0x17: /* Escape */
			print(u"\r\nAborted\r\n");
			return EFI_ABORTED;
		default:
			break;
		}
		switch (key.unicode_char) {
		case 0x08: /* Backspace */
			if (pos) {
				buffer[pos--] = 0;
				print(u"\b \b");
			}
			break;
		case 0x0a: /* Linefeed */
		case 0x0d: /* Carriage return */
			print(u"\r\n");
			return EFI_SUCCESS;
		default:
			break;
		}
		/* Ignore surrogate codes */
		if (key.unicode_char >= 0xD800 && key.unicode_char <= 0xDBFF)
			continue;
		if (key.unicode_char >= 0x20 &&
		    pos < buffer_size - 1) {
			*outbuf = key.unicode_char;
			buffer[pos++] = key.unicode_char;
			buffer[pos] = 0;
			print(outbuf);
		}
	}
}

/**
 * skip_whitespace() - skip over leading whitespace
 *
 * @pos:	UTF-16 string
 * Return:	pointer to first non-whitespace
 */
static u16 *skip_whitespace(u16 *pos)
{
	for (; *pos && *pos <= 0x20; ++pos)
		;
	return pos;
}

/**
 * starts_with() - check if @string starts with @keyword
 *
 * @string:	string to search for keyword
 * @keyword:	keyword to be searched
 * Return:	true if @string starts with the keyword
 */
static bool starts_with(u16 *string, u16 *keyword)
{
	if (!string || !keyword)
		return false;

	for (; *keyword; ++string, ++keyword) {
		if (*string != *keyword)
			return false;
	}
	return true;
}

/*
 * efi_dectoul() - decimal string to efi_uintn_t
 *
 * @s:		string
 * Return:	string as efi_uintn_t
 */
static efi_uintn_t efi_dectoul(char *s)
{
	efi_uintn_t result = 0;

	while (*s >= '0' && *s <= '9') {
		result = result * 10 + (*s - '0');
		s++;
	}
	return result;
}

/*
 * parse_address() - parse string IP address into ip_addr
 *
 * @input:	string IP address
 * @address:	pointer to u8[4] buffer
 */
static void parse_address(u8 **input, u8 *address)
{
	int i = 0;
	efi_uintn_t part;

	if (**input < '0' || **input > '9')
		return;

	while (i <= 3) {
		part = efi_dectoul(*input);
		if (part > 255)
			return;
		address[i] = (u8)part;
		while (**input >= '0' && **input <= '9')
			(*input)++;
		if (**input == '.')
			(*input)++;
		else
			break;
		i++;
	}
}

/*
 * utf16le_ascii_to_utf8() - simple utf16to utf8 converter
 *
 * @out:	output utf8 buffer
 * @in:		input utf16 buffer
 * @max_out:	maximum characters to convert
 */
static int utf16le_ascii_to_utf8(char *out, const u16 *in, size_t max_out)
{
	size_t i = 0;

	while (in[i] && i < max_out - 1) {
		if (in[i] > 0x7F)
			return -1;  // non-ASCII detected, not supported
		out[i] = (char)(in[i] & 0xFF);
		i++;
	}
	out[i] = '\0';
	return 0;
}

/**
 * do_help() - print help
 */
static void do_help(void)
{
	error(u"set address <ip> <netmask> - set IP and netmask\r\n");
	error(u"get address                - get IP and netmask\r\n");
	error(u"set policy static          - set policy to static\r\n");
	error(u"wget <uri>                 - fetch a file via EFI_HTTP_PROTOCOL\r\n");
	error(u"exit                       - exit the shell\r\n");
}

/**
 * do_set_address() - sets IP and netmask using EFI_IP4_CONFIG2_PROTOCOL
 *
 * @address:		ip and netmask
 * Return:		status code
 */
static efi_status_t do_set_address(u16 *address)
{
	efi_uintn_t ret;
	struct efi_ip4_config2_protocol *ip4_config2;
	struct efi_ip4_config2_manual_address manual_address;
	u8 netmask[4];
	u8 ip[4];
	u8 utf8[128];
	u8 *tmp;

	ret = bs->open_protocol(net_handle, &efi_ip4_config2_guid,
					(void **)&ip4_config2, 0, 0,
					EFI_OPEN_PROTOCOL_GET_PROTOCOL);

	if (ret != EFI_SUCCESS) {
		error(u"set address: failed to open ipv4 config2 protocol\r\n");
		return ret;
	}

	// Convert the UTF-16 string to UTF-8
	if (utf16le_ascii_to_utf8(utf8, address, sizeof(utf8)) != 0) {
		error(u"set address: failed to convert address to utf8\r\n");
		return EFI_DEVICE_ERROR;
	}

	tmp = utf8;
	parse_address(&tmp, ip);
	tmp++;
	parse_address(&tmp, netmask);

	/* Set static ip and netmask */
	memcpy(&manual_address.address, ip,
	       sizeof(struct efi_ipv4_address));
	memcpy(&manual_address.subnet_mask, netmask,
	       sizeof(struct efi_ipv4_address));
	ret = ip4_config2->set_data(ip4_config2, EFI_IP4_CONFIG2_DATA_TYPE_MANUAL_ADDRESS,
			      sizeof(manual_address), &manual_address);
	if (ret != EFI_SUCCESS) {
		error(u"set address: failed to set ip address and netmask\r\n");
		return ret;
	}

	return EFI_SUCCESS;
}

/**
 * do_get_address() - get IP and netmask using EFI_IP4_CONFIG2_PROTOCOL
 *
 * Return:		status code
 */
static efi_status_t do_get_address(void)
{
	efi_uintn_t ret;
	struct efi_ip4_config2_protocol *ip4_config2;
	struct efi_ip4_config2_manual_address manual_address;
	efi_uintn_t data_size;

	ret = bs->open_protocol(net_handle, &efi_ip4_config2_guid,
					(void **)&ip4_config2, 0, 0,
					EFI_OPEN_PROTOCOL_GET_PROTOCOL);

	if (ret != EFI_SUCCESS) {
		error(u"get address: failed to open ipv4 config2 protocol\r\n");
		return ret;
	}

	/* Get IP address and netmask */
	data_size = sizeof(manual_address);
	ret = ip4_config2->get_data(ip4_config2, EFI_IP4_CONFIG2_DATA_TYPE_MANUAL_ADDRESS,
			      &data_size, &manual_address);
	if (ret != EFI_SUCCESS) {
		error(u"get address: failed to get IP address and netmask\r\n");
		return ret;
	}

	print(u"CURRENT IP: ");
	printd(manual_address.address.ip_addr[0]);
	print(u".");
	printd(manual_address.address.ip_addr[1]);
	print(u".");
	printd(manual_address.address.ip_addr[2]);
	print(u".");
	printd(manual_address.address.ip_addr[3]);
	print(u"\r\n");
	print(u"CURRENT NETMASK: ");
	printd(manual_address.subnet_mask.ip_addr[0]);
	print(u".");
	printd(manual_address.subnet_mask.ip_addr[1]);
	print(u".");
	printd(manual_address.subnet_mask.ip_addr[2]);
	print(u".");
	printd(manual_address.subnet_mask.ip_addr[3]);
	print(u"\r\n");

	return EFI_SUCCESS;
}

/**
 * do_set_policy_static() - sets policy to static
 *
 * Return:		status code
 */
static efi_status_t do_set_policy_static(void)
{
	efi_uintn_t ret;
	struct efi_ip4_config2_protocol *ip4_config2;
	enum efi_ip4_config2_policy policy;

	ret = bs->open_protocol(net_handle, &efi_ip4_config2_guid,
					(void **)&ip4_config2, 0, 0,
					EFI_OPEN_PROTOCOL_GET_PROTOCOL);
	if (ret != EFI_SUCCESS) {
		error(u"set polict: failed to open ipv4 config2 protocol\r\n");
		return ret;
	}

	/* Set policy to static */
	policy = EFI_IP4_CONFIG2_POLICY_STATIC;
	ret = ip4_config2->set_data(ip4_config2, EFI_IP4_CONFIG2_DATA_TYPE_POLICY,
			      sizeof(policy), (void *)&policy);
	if (ret != EFI_SUCCESS) {
		error(u"Failed to set policy\r\n");
		return ret;
	}

	return EFI_SUCCESS;
}

/**
 * select_net() - get a handle for networking
 *
 * Return:		status code
 */
static efi_status_t select_net(void)
{
	efi_uintn_t ret;
	efi_uintn_t num_handles;
	efi_handle_t *current_handle;
	efi_handle_t *handles;
	struct efi_ip4_config2_protocol *ip4_config2;

	num_handles = 0;
	ret = bs->locate_handle_buffer(BY_PROTOCOL, &efi_ip4_config2_guid,
				       NULL, &num_handles, &handles);

	if (ret != EFI_SUCCESS) {
		error(u"Failed to locate ipv4 config2 protocol\r\n");
		return ret;
	}

	for (current_handle = handles; num_handles--; current_handle++) {
		ret = bs->open_protocol(*current_handle, &efi_ip4_config2_guid,
					      (void **)&ip4_config2, 0, 0,
					      EFI_OPEN_PROTOCOL_GET_PROTOCOL);
		if (ret != EFI_SUCCESS)
			continue;
		break; // Get first handle that supports
	}
	if (ret != EFI_SUCCESS) {
		error(u"Failed to open ipv4 config2 protocol\r\n");
		return ret;
	}

	net_handle = *current_handle;

	return EFI_SUCCESS;
}

/**
 * setup_http() - get handles and open protocols
 *
 * Return:		status code
 */
static efi_status_t setup_http(void)
{
	efi_uintn_t ret;
	struct efi_http_config_data http_config;
	struct efi_httpv4_access_point ipv4_node;
	struct efi_http_protocol *http;
	struct efi_service_binding_protocol *http_service;

	ret = bs->open_protocol(net_handle, &efi_http_service_binding_guid,
					(void **)&http_service, 0, 0,
					EFI_OPEN_PROTOCOL_GET_PROTOCOL);
	if (ret != EFI_SUCCESS) {
		error(u"Failed to open http service binding protocol\r\n");
		return ret;
	}

	http_protocol_handle = NULL;
	ret = http_service->create_child(http_service, &http_protocol_handle);
	if (ret != EFI_SUCCESS) {
		error(u"Failed to create an http service instance\r\n");
		return ret;
	}

	ret = bs->open_protocol(http_protocol_handle, &efi_http_guid,
				      (void **)&http, 0, 0, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
	if (ret != EFI_SUCCESS) {
		error(u"Failed to open http protocol\r\n");
		return ret;
	}
	print(u"HTTP Service Binding: child created successfully\r\n");

	http_config.http_version = HTTPVERSION11;
	http_config.is_ipv6 = false;
	http_config.access_point.ipv4_node = &ipv4_node;
	ipv4_node.use_default_address = true;

	ret = http->configure(http, &http_config);
	if (ret != EFI_SUCCESS) {
		error(u"Failed to configure http instance\r\n");
		return ret;
	}

	return EFI_SUCCESS;
}

static void EFIAPI callback_http(struct efi_event *event, void *context)
{
	callback_done = 1;
}

/**
 * execute_http() - execute http request
 *
 * @filename:	file name
 * Return:	status code
 */
static efi_status_t execute_http(u16 *filename, void **buffer, efi_uintn_t *buffer_size)
{
	efi_status_t ret;
	struct efi_http_protocol *http;
	struct efi_http_request_data request_data;
	struct efi_http_message request_message;
	struct efi_http_token request_token;
	struct efi_http_response_data response_data;
	struct efi_http_message response_message;
	struct efi_http_token response_token;
	enum efi_http_status_code status_code;
	efi_uintn_t len, sum, content_len;
	int i;

	request_data.method = HTTP_METHOD_GET;
	request_data.url = filename;
	request_message.data.request = &request_data;
	request_message.header_count = 3;
	request_message.body_length = 0;
	request_message.body = NULL;

	/* request token */
	request_token.event = NULL;
	request_token.status = EFI_NOT_READY;
	request_token.message = &request_message;
	callback_done = 0;
	ret = bs->create_event(EVT_NOTIFY_SIGNAL,
			TPL_CALLBACK,
			callback_http,
			NULL,
			&request_token.event);

	if (ret != EFI_SUCCESS) {
		error(u"Failed to create request event\r\n");
		return ret;
	}

	ret = bs->open_protocol(http_protocol_handle, &efi_http_guid,
				      (void **)&http, 0, 0, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
	if (ret != EFI_SUCCESS) {
		error(u"Failed to open http protocol\r\n");
		return ret;
	}

	ret = http->request(http, &request_token);

	if (ret != EFI_SUCCESS) {
		bs->close_event(request_token.event);
		error(u"Failed to proceed with the http request\r\n");
		return ret;
	}

	while (!callback_done)
		http->poll(http);

	response_data.status_code = HTTP_STATUS_UNSUPPORTED_STATUS;
	response_message.data.response = &response_data;
	response_message.header_count = 0;
	response_message.headers = NULL;
	response_message.body_length = 0;
	response_message.body = NULL;
	response_token.event = NULL;

	ret = bs->create_event(EVT_NOTIFY_SIGNAL,
			TPL_CALLBACK,
			callback_http,
			NULL,
			&response_token.event);

	if (ret != EFI_SUCCESS) {
		bs->close_event(request_token.event);
		error(u"Failed to create response event\r\n");
		return ret;
	}

	response_token.status = EFI_SUCCESS;
	response_token.message = &response_message;

	callback_done = 0;
	ret = http->response(http, &response_token);

	if (ret != EFI_SUCCESS) {
		error(u"Failed http first response\r\n");
		goto fail;
	}

	while (!callback_done)
		http->poll(http);

	if (response_message.data.response->status_code != HTTP_STATUS_200_OK) {
		status_code = response_message.data.response->status_code;
		if (status_code == HTTP_STATUS_404_NOT_FOUND) {
			error(u"File not found\r\n");
		} else {
			error(u"Bad http status:\r\n");
			printx(response_message.data.response->status_code, 2);
			print(u"\r\n");
		}
		goto fail_free_hdr;
	}

	for (i = 0; i < response_message.header_count; i++) {
		if (!memcmp(response_message.headers[i].field_name,
			    "Content-Length", sizeof("Content-Length")))
			content_len = efi_dectoul(response_message.headers[i].field_value);
	}

	ret = bs->allocate_pool(EFI_LOADER_CODE, content_len,
				      buffer);
	if (ret != EFI_SUCCESS) {
		error(u"Failed allocating response buffer\r\n");
		goto fail_free_hdr;
	}

	*buffer_size = content_len;
	len = content_len;
	sum = 0;
	while (len) {
		response_message.data.response = NULL;
		response_message.header_count = 0;
		response_message.headers = NULL;
		response_message.body_length = len;
		response_message.body = *buffer + sum;

		response_token.message = &response_message;
		response_token.status = EFI_NOT_READY;

		callback_done = 0;
		ret = http->response(http, &response_token);
		if (ret != EFI_SUCCESS) {
			error(u"Failed http second response\r\n");
			goto fail_free_buf;
		}

		while (!callback_done)
			http->poll(http);

		if (!response_message.body_length)
			break;

		len -= response_message.body_length;
		sum += response_message.body_length;
	}

	if (len) {
		ret = EFI_DEVICE_ERROR;
		goto fail_free_buf;
	}

	if (response_message.headers)
		bs->free_pool(response_message.headers);
	bs->close_event(request_token.event);
	bs->close_event(response_token.event);
	print(u"Efi Http request executed successfully\r\n");
	return EFI_SUCCESS;

fail_free_buf:
	bs->free_pool(*buffer);
	*buffer = NULL;
fail_free_hdr:
	if (response_message.headers)
		bs->free_pool(response_message.headers);
fail:
	bs->close_event(request_token.event);
	bs->close_event(response_token.event);
	return ret;
}

/**
 * do_netdump_wget() - fetch a file via EFI_HTTP_PROTOCOL
 *
 * @filename:	file name
 * Return:	status code
 */
static efi_status_t do_netdump_wget(u16 *filename)
{
	void *buffer;
	efi_uintn_t buffer_size;
	u32 crc32;
	efi_uintn_t ret = EFI_SUCCESS;

	ret = do_set_policy_static();
	if (ret != EFI_SUCCESS) {
		error(u"Failed to set static network policy\r\n");
		return ret;
	}

	ret = setup_http();
	if (ret != EFI_SUCCESS) {
		error(u"Failed to set up http protocol\r\n");
		return ret;
	}

	buffer = NULL;
	ret = execute_http(filename, &buffer, &buffer_size);
	if (ret != EFI_SUCCESS) {
		error(u"Failed to execute http request\r\n");
		return ret;
	}

	ret = bs->calculate_crc32(buffer, buffer_size, &crc32);
	if (ret != EFI_SUCCESS) {
		error(u"Calculating CRC32 failed\r\n");
		goto out;
	}

	print(u"length: ");
	printd(buffer_size);
	print(u"\r\n");

	print(u"crc32: 0x");
	printx(crc32, 8);
	print(u"\r\n");
out:
	bs->free_pool(buffer);
	return ret;
}

/**
 * get_load_options() - get load options
 *
 * Return:	load options or NULL
 */
static u16 *get_load_options(void)
{
	efi_status_t ret;
	struct efi_loaded_image *loaded_image;

	ret = bs->open_protocol(handle, &loaded_image_guid,
				(void **)&loaded_image, NULL, NULL,
				EFI_OPEN_PROTOCOL_GET_PROTOCOL);
	if (ret != EFI_SUCCESS) {
		error(u"Loaded image protocol not found\r\n");
		return NULL;
	}

	if (!loaded_image->load_options_size || !loaded_image->load_options)
		return NULL;

	return loaded_image->load_options;
}

/**
 * efi_main() - entry point of the EFI application.
 *
 * @handle:	handle of the loaded image
 * @systab:	system table
 * Return:	status code
 */
efi_status_t EFIAPI efi_main(efi_handle_t image_handle,
			     struct efi_system_table *systab)
{
	u16 *load_options;
	efi_status_t ret;

	handle = image_handle;
	systable = systab;
	cerr = systable->std_err;
	cout = systable->con_out;
	cin = systable->con_in;
	bs = systable->boottime;
	load_options = get_load_options();

	if (starts_with(load_options, u"nocolor"))
		nocolor = true;

	ret = select_net();
	if (ret != EFI_SUCCESS) {
		error(u"Failed to set up networking\r\n");
		return ret;
	}

	color(EFI_WHITE);
	cls();
	print(u"NET Dump\r\n========\r\n\r\n");

	color(EFI_LIGHTBLUE);

	for (;;) {
		u16 command[BUFFER_SIZE];
		u16 *pos;
		efi_uintn_t ret;

		efi_drain_input();
		print(u"=> ");
		ret = efi_input(command, sizeof(command));
		if (ret == EFI_ABORTED)
			break;
		pos = skip_whitespace(command);
		if (starts_with(pos, u"exit"))
			break;
		else if (starts_with(pos, u"set address "))
			do_set_address(pos + 12);
		else if (starts_with(pos, u"get address"))
			do_get_address();
		else if (starts_with(pos, u"set policy static"))
			do_set_policy_static();
		else if (starts_with(pos, u"wget "))
			do_netdump_wget(pos + 5);
		else
			do_help();
	}

	color(EFI_LIGHTGRAY);
	cls();

	return EFI_SUCCESS;
}
