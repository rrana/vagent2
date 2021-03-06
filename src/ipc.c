/*
 * Copyright (c) 2012-2013 Varnish Software AS
 * All rights reserved.
 *
 * Author: Kristian Lyngstøl <kristian@bohemians.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * General IPC mechanisms for use between various plugins.
 *
 * Usage:
 * Step 1: Provider has a single ipc_t *handle structure. This structure
 *         must be readable from the consumers/users. Issue ipc_init.
 * Step 2: While plugins/modules load, they issue ipc_register(handle) and
 *         store the returned value.
 * Step 3: (or 2... whichever). Provider sets handle->cb to a command
 *         handler and handle->priv respectively.
 * Step 4: Provider issues ipc_start, this returns a thread structure and
 *         the provider is open for business.
 * Step 5: A consumer/user issues ipc_run(sock, command, return), where the
 *         sock is what ipc_register() returned earlier, the command is the
 *         message to send and the return is a ipc_ret_t structure that
 *         will be used to return the status.
 * Step 6: The provider sees the message and the callback is run with the
 *         private data, command and a /different/ ipc_ret_t structure.
 *
 */


#define _GNU_SOURCE
#include "config.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>


#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <vcli.h>

#include "common.h"
#include "ipc.h"
#include "plugins.h"

/*
 * Client
 */

/*
 * Write text to a socket. Close it if we fail.
 * Returns true on success.
 */
static int ipc_write(int sock, const char *s)
{
	int i, l;

	i = strlen(s);
	l = write (sock, s, i);
	if (i == l)
		return 1;
	perror("Write error CLI socket");
	assert(close(sock));
	return 0;
}

/*
 * Write the command, read the result.
 * XXX: VCLI_ReadResult will allocate ret->answer. Caller MUST free it.
 * XXX: Note the \n is added. Do not add your own, or you wont get a reply.
 */
static void ipc_run_real(int handle, char *cmd, struct ipc_ret_t *ret)
{
	assert(cmd);
	assert (*cmd);
	ipc_write(handle, cmd);
	ipc_write(handle, "\n");

	VCLI_ReadResult(handle, &ret->status, &ret->answer, 2.0);
}

/*
 * Parse a command of arbitrary length, execute it, place result in *ret.
 */
void ipc_run(int handle, struct ipc_ret_t *ret, const char *fmt, ...)
{
	va_list ap;
	char *buffer;
	int iret;
	assert(fmt);
	va_start(ap, fmt);
	iret = vasprintf(&buffer, fmt, ap);
	assert(iret>0);
	va_end(ap);
	assert(buffer);
	ipc_run_real(handle, buffer, ret);
	free(buffer);
}

/*
 * Grab a IPC handle for a named plugin.
 * Return value is later used for ipc_run().
 * XXX: Must execute prior to plugins starting, otherwise we might use the
 * ipc structure before it's fully populated.
 */
int ipc_register(struct agent_core_t *core, const char *name)
{
	struct agent_plugin_t *v;
	int sv[2];
	int ret;
	v  = plugin_find(core, name);

	ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
	assert(ret == 0);
	assert(v->ipc->nlisteners < MAX_LISTENERS);
	v->ipc->listeners[v->ipc->nlisteners++] = sv[0];
	return sv[1];
}

/*
 * Server
 */

/*
 * Read everything up to the first new line.
 * FIXME: The 1024-limit here is a bit weird. Should probably use VSB
 * instead.
 * FIXME: read() has no timeout. We will block forever. FOREVER. Causes
 * havoc if a plugin is bugged, as it blocks other plugins.
 */
static int ipc_read_line(int fd, char **line)
{
	char *buf = *line;
	int i;
	int iret;
	char c;

	for (i=0; i<1024; i++) {
		iret = read(fd, &c, 1);
		assert (iret == 1);
		buf[i] = c;
		if (c == '\n')
			break;
	}
	assert (i < 1024);
	buf[i] = '\0';
	return i;
}

/*
 * A command was apparently issued. Read the data, including any here-doc
 * stuff, then execute the command.
 * Note that &ret must be populated with something we can free().
 */
static int ipc_cmd(int fd, struct ipc_t *ipc)
{
	char *buffer = malloc(1024);
	struct ipc_ret_t ret;
	assert(buffer);
	int length = 0;
	char *here = NULL;

	length = ipc_read_line(fd, &buffer);
	if (strstr(buffer,"<< ")) {
		char *line;
		here = strdup(strstr(buffer, "<< ") + 3);
		buffer[length] = '\n';
		while (1) {
			buffer = realloc(buffer,length + 1024);
			assert(buffer);
			line = buffer+length+1;
			length += ipc_read_line(fd, &line);
			length++;
			if (!strcmp(line,here))
				break;
			buffer[length] = '\n';
		}
		buffer[length] = '\0';
	}
	if (here)
		free(here);
	/*
	 * XXX: Typically hit if you throw in an empty newline at ipc_run,
	 * e.g: ipc_run(...,"param.set foo bar\n");
	 * Tends to cause havoc since varnish doesn't resond at all and we
	 * block.
	 */
	assert(*buffer);
	ipc->cb(ipc->priv, buffer, &ret);
	VCLI_WriteResult(fd, ret.status, ret.answer);
	free(buffer);
	if (ret.answer)
		free(ret.answer);
	return 1;
}

/*
 * IPC main loop.
 * Just wait for data on the fds provided, then trigger ipc_cmd().
 */
static void *ipc_loop(void *data)
{
	struct ipc_t *ipc = (struct ipc_t *)data;
	struct pollfd fds[MAX_LISTENERS+1];
	int i;
	int ret;

	for (i=0; i < ipc->nlisteners; i ++) {
		fds[i].fd = ipc->listeners[i];
		fds[i].events = POLLIN;
	}
	while (1) {
		ret = poll(fds, ipc->nlisteners, -1);
		assert(ret > 0);
		for (i=0; i < ipc->nlisteners; i++) {
			if (fds[i].revents & POLLIN) {
				ipc_cmd(fds[i].fd, ipc);
			}
		}
	}
	return NULL;
}

/*
 * Should probably be redundant...
 */
void ipc_init(struct ipc_t *ipc)
{
	ipc->nlisteners = 0;
}

/*
 * Does the actual threading and returns the thread.
 */
pthread_t *ipc_start(struct agent_core_t *core, const char *name)
{
	struct agent_plugin_t *plug;
	pthread_t *thread = malloc(sizeof (pthread_t));
	plug = plugin_find(core, name);
	
	pthread_create(thread,NULL,(ipc_loop),plug->ipc);
	plug->thread = thread;
	return thread;
}
