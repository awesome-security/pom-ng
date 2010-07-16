/*
 *  This file is part of pom-ng.
 *  Copyright (C) 2010 Guy Martin <gmsoft@tuxicoman.be>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */


#include "common.h"

#include <getopt.h>
#include <pwd.h>
#include <signal.h>
#include <sys/msg.h>
#include <sys/wait.h>

#include "main.h"
#include "input.h"
#include "input_ipc.h"
#include "xmlrpcsrv.h"
#include "httpd.h"

#include "mod.h" // TOREMOVE
#include "pomlog.h"

static char* shutdown_reason = NULL;
static pid_t input_process_pid = 0;
static int running = 1;

void signal_handler(int signal) {

	switch (signal) {
		case SIGCHLD:
			if (running)
				halt("Input process died :-(");
			break;
		case SIGINT:
		case SIGTERM:
		default:
			halt("Received signal");
			break;

	}
}


int main(int argc, char *argv[]) {

	// Parse options

	int c;
	
	uid_t uid = 0;
	gid_t gid = 0;

	while (1) {

		static struct option long_options[] = {
			{ "user", 1, 0, 'u' },
		};

		
		char *args = "u:";

		c = getopt_long(argc, argv, args, long_options, NULL);

		if (c == -1)
			break;

		switch (c) {
			case 'u': {
				char *user = optarg;
				struct passwd pwd, *res;

				size_t buffsize = sysconf(_SC_GETPW_R_SIZE_MAX);
				if (buffsize < 0) {
					pomlog(POMLOG_ERR "Could not find out buffer size for getpwnam_r()");
					return -1;
				}

				char *buff = malloc(buffsize);

				getpwnam_r(user, &pwd, buff, buffsize, &res);
				if (!res) {
					pomlog(POMLOG_ERR "Could not get user info, does user %s exists ?", user);
					return -1;
				}
				free(buff);


				uid = pwd.pw_uid;
				gid = pwd.pw_gid;

				break;
			}
		}


	}

	pomlog("Starting " PACKAGE_NAME " ...");

	// Create IPC key and queue
	
	key_t input_ipc_key = 0;
	
	int i;
	for (i = 0; i < strlen(PACKAGE_NAME); i++)
		input_ipc_key += PACKAGE_NAME[i];
	input_ipc_key += getpid();

	int input_ipc_queue = input_ipc_create_queue(input_ipc_key);
	if (input_ipc_queue == POM_ERR) {
		pomlog(POMLOG_ERR "Unable to create IPC message queue");
		return -1;
	}
	
	// Change the permissions of the queue to the low privilege user
	if (uid || gid) {
		if (input_ipc_set_uid_gid(input_ipc_queue, uid, gid) != POM_OK) {
			pomlog(POMLOG_ERR "Could not set right permissions on the IPC queue");
			return -1;
		}
	}

	// Fork the input process while we have root privileges
	input_process_pid = fork();

	if (input_process_pid == -1) {
		pomlog(POMLOG_ERR "Error while forking()");
		return -1;
	}

	if (!input_process_pid) { // Child
		return input_main(input_ipc_key, uid, gid);
	}

	// Drop privileges if provided

	if (gid && setegid(gid)) {
		pomlog(POMLOG_ERR "Failed to drop group privileges : %s", strerror(errno));
		return -1;
	}
	if (uid && seteuid(uid)) {
		pomlog(POMLOG_ERR "Failed to drop user privileges : %s", strerror(errno));
		return -1;
	}

	if (uid || gid)
		pomlog(POMLOG_ERR "Main process dropped privileges to uid/gid %u/%u", geteuid(), getegid());

	// Install signal handler

	struct sigaction mysigaction;
	sigemptyset(&mysigaction.sa_mask);
	mysigaction.sa_flags = 0;
	mysigaction.sa_handler = signal_handler;
	sigaction(SIGINT, &mysigaction, NULL);
	sigaction(SIGCHLD, &mysigaction, NULL);
	sigaction(SIGTERM, &mysigaction, NULL);

	// Initialize components
	
	int res = 0;

	if (xmlrpcsrv_init() != POM_OK) {
		pomlog(POMLOG_ERR "Error while starting XML-RPC server");
		res = -1;
		goto err_xmlrpcsrv;
	}

	if (httpd_init(8080) != POM_OK) {
		pomlog(POMLOG_ERR "Error while starting HTTP server");
		res = -1;
		goto err_httpd;
	}

	// Wait for the IPC queue to be created
	int input_queue_id = input_ipc_get_queue(input_ipc_key);
	if (input_queue_id == -1)
		goto err;

	// Init the input IPC log thread
	if (pomlog_ipc_thread_init(&input_queue_id) != POM_OK)
		goto err;

	// Main loop
	
	pomlog(PACKAGE_NAME " started !");

	struct mod_reg *mod_ptye_uint32 = mod_load("ptype_uint32");

	while (running) {

		if (input_ipc_process_reply(input_queue_id) != POM_OK) {
			pomlog("Error while processing input reply. Aborting");
			break;
		}

		sleep(3);
	}

	pomlog(POMLOG_INFO "Shutting down : %s", shutdown_reason);
	free(shutdown_reason);
	shutdown_reason = NULL;

	mod_unload(mod_ptye_uint32);

err:

	if (kill(input_process_pid, SIGINT) == -1) {
		pomlog(POMLOG_ERR "Error while sending SIGINT to input process");
	} else {
		pomlog("Waiting for input process to terminate ...");
		waitpid(input_process_pid, NULL, 0);
	}

	// Delete the IPC queue
	if (msgctl(input_ipc_queue, IPC_RMID, 0)) {
		pomlog(POMLOG_WARN "Unable to remove the IPC msg queue while terminating");
	}


	// Cleanup components

	input_ipc_cleanup();

	httpd_cleanup();
err_httpd:
	xmlrpcsrv_cleanup();
err_xmlrpcsrv:

	pomlog_cleanup();

	printf(POMLOG_INFO PACKAGE_NAME " shutted down\n");
	return res;
}


int halt(char *reason) {
	// Can be called from a signal handler, don't use pomlog()
	shutdown_reason = strdup(reason);
	running = 0;
	return POM_OK;
}

