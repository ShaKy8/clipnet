#pragma once

/* Client mode: send one JSON request (an "id" is added when missing), print
 * the result as JSON on stdout, exit 0; on an error reply print the message
 * on stderr and exit 1; exit 2 when the daemon cannot be reached. */
int ctl_main(const char *socket_path, const char *request_json);
