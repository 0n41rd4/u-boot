/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Duncan Hare Copyright 2017
 */

/**
 * wget_start() - begin wget
 */
void wget_start(void);

enum wget_state {
	WGET_CLOSED,
	WGET_CONNECTING,
	WGET_CONNECTED,
	WGET_TRANSFERRING,
	WGET_TRANSFERRED
};

enum wget_http_method {
	WGET_HTTP_METHOD_GET,
	WGET_HTTP_METHOD_POST,
	WGET_HTTP_METHOD_PATCH,
	WGET_HTTP_METHOD_OPTIONS,
	WGET_HTTP_METHOD_CONNECT,
	WGET_HTTP_METHOD_HEAD,
	WGET_HTTP_METHOD_PUT,
	WGET_HTTP_METHOD_DELETE,
	WGET_HTTP_METHOD_TRACE,
	WGET_HTTP_METHOD_MAX
};

#define DEBUG_WGET		0	/* Set to 1 for debug messages */
#define WGET_RETRY_COUNT	30
#define WGET_TIMEOUT		2000UL
#define MAX_HTTP_HEADERS 100
#define MAX_HTTP_HEADER_NAME 256
#define MAX_HTTP_HEADER_VALUE 512

struct wget_http_header {
	uchar name[MAX_HTTP_HEADER_NAME];
	uchar value[MAX_HTTP_HEADER_VALUE];
};

struct wget_http_info {
	enum wget_http_method method;
	ulong status_code;
	ulong content_length;
	bool set_bootdev;
	ulong num_headers;
	struct wget_http_header headers[MAX_HTTP_HEADERS];
};

extern struct wget_http_info current_http_info;