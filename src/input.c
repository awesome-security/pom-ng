/*
 *  This file is part of pom-ng.
 *  Copyright (C) 2010-2015 Guy Martin <gmsoft@tuxicoman.be>
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

#include "core.h"
#include "registry.h"
#include "input.h"
#include "mod.h"
#include "core.h"
#include "packet.h"
#include <pom-ng/ptype.h>
#include <pom-ng/ptype_bool.h>
#include <pom-ng/proto.h>

static struct registry_class *input_registry_class = NULL;

static struct input_reg *input_reg_head = NULL;
static struct input *input_head = NULL;
static unsigned int input_cur_running = 0;

int input_init() {
	
	input_registry_class = registry_add_class(INPUT_REGISTRY);
	if (!input_registry_class)
		return POM_ERR;

	input_registry_class->instance_add = input_instance_add;
	input_registry_class->instance_remove = input_instance_remove;
		
	return POM_OK;
}

int input_cleanup() {


	if (input_registry_class)
		registry_remove_class(input_registry_class);
	input_registry_class = NULL;

	while (input_reg_head) {

		struct input_reg *tmp = input_reg_head;
		input_reg_head = tmp->next;

		if (tmp->info->unregister_func && tmp->info->unregister_func() != POM_OK) {
			pomlog(POMLOG_WARN "Error while running the unregister function of the input %s", tmp->info->name);
		}

		mod_refcount_dec(tmp->info->mod);
		free(tmp);
	}



	return POM_OK;

}

int input_register(struct input_reg_info *reg_info) {

	pomlog(POMLOG_DEBUG "Registering input %s", reg_info->name);

	struct input_reg *tmp;
	for (tmp = input_reg_head; tmp && strcmp(tmp->info->name, reg_info->name); tmp = tmp->next);
	if (tmp) {
		pomlog(POMLOG_ERR "Input %s already registered", reg_info->name);
		return POM_ERR;
	}

	if (reg_info->register_func) {
		if (reg_info->register_func() != POM_OK) {
			pomlog(POMLOG_ERR "Error while calling the register function of the input %s", reg_info->name);
		}
	}

	struct input_reg *reg = malloc(sizeof(struct input_reg));
	if(!reg) {
		pom_oom(sizeof(struct input_reg));
		return POM_ERR;
	}

	memset(reg, 0, sizeof(struct input_reg));
	reg->info = reg_info;

	if (registry_add_instance_type(input_registry_class, reg_info->name, reg_info->description) != POM_OK) {
		free(reg);
		return POM_ERR;
	}

	mod_refcount_inc(reg_info->mod);

	reg->next = input_reg_head;
	input_reg_head = reg;
	if (reg->next)
		reg->next->prev = reg;

	return POM_OK;

}

int input_unregister(char *name) {

	struct input_reg *reg;

	for (reg = input_reg_head; reg && strcmp(reg->info->name, name); reg = reg->next);
	if (!reg) 
		return POM_OK;

	if (reg->info->unregister_func && reg->info->register_func() != POM_OK) {
		pomlog(POMLOG_ERR "Error while calling the register function of the input %s", reg->info->name);
	}

	registry_remove_instance_type(input_registry_class, name);

	if (reg->prev)
		reg->prev->next = reg->next;
	else
		input_reg_head = reg->next;
	
	if (reg->next)
		reg->next->prev = reg->prev;

	reg->next = NULL;
	reg->prev = NULL;

	mod_refcount_dec(reg->module);

	free(reg);

	return POM_OK;
}

int input_instance_add(char *type, char *name) {

	struct input_reg *reg;
	for (reg = input_reg_head; reg && strcmp(reg->info->name, type); reg = reg->next);

	if (!reg) {
		pomlog(POMLOG_ERR "Input type %s does not exists", type);
		return POM_ERR;
	}

	struct input *res = malloc(sizeof(struct input));
	if (!res) {
		pom_oom(sizeof(struct input));
		return POM_ERR;
	}
	memset(res, 0, sizeof(struct input));

	res->reg_instance = registry_add_instance(input_registry_class, name);
	if (!res->reg_instance)
		goto err;

	res->reg = reg;
	res->name = strdup(name);
	if (!res->name) {
		pom_oom(strlen(name) + 1);
		goto err;
	}


	res->reg_instance->priv = res;

	struct ptype *param_running_val = ptype_alloc("bool");
	if (!param_running_val)
		goto err;

	struct registry_param *param_running = registry_new_param("running", "no", param_running_val, "Running state of the input",  REGISTRY_PARAM_FLAG_CLEANUP_VAL);
	if (!param_running) {
		ptype_cleanup(param_running_val);
		goto err;
	}
	res->reg_param_running = param_running;

	if (registry_param_set_callbacks(param_running, res, NULL, input_instance_start_stop_handler) != POM_OK) {
		registry_cleanup_param(param_running);
		ptype_cleanup(param_running_val);
		goto err;
	}
	
	if (registry_instance_add_param(res->reg_instance, param_running) != POM_OK) {
		registry_cleanup_param(param_running);
		ptype_cleanup(param_running_val);
		goto err;
	}


	struct ptype *input_type = ptype_alloc("string");
	if (!input_type)
		goto err;

	struct registry_param *type_param = registry_new_param("type", type, input_type, "Type of the input", REGISTRY_PARAM_FLAG_CLEANUP_VAL | REGISTRY_PARAM_FLAG_IMMUTABLE);
	if (!type_param) {
		ptype_cleanup(input_type);
		goto err;
	}

	if (registry_instance_add_param(res->reg_instance, type_param) != POM_OK) {
		registry_cleanup_param(type_param);
		ptype_cleanup(input_type);
		goto err;
	}

	res->perf_pkts_in = registry_instance_add_perf(res->reg_instance, "pkts_in", registry_perf_type_counter, "Number of packets read", "pkts");
	res->perf_bytes_in = registry_instance_add_perf(res->reg_instance, "bytes_in", registry_perf_type_counter, "Number of bytes read", "bytes");
	res->perf_runtime = registry_instance_add_perf(res->reg_instance, "runtime", registry_perf_type_timeticks, "Runtime", NULL);

	if (!res->perf_pkts_in || !res->perf_bytes_in || !res->perf_runtime)
		goto err;

	if (registry_uid_create(res->reg_instance) != POM_OK)
		goto err;

	if (reg->info->init) {
		if (reg->info->init(res) != POM_OK) {
			pomlog(POMLOG_ERR "Error while initializing the input %s", name);
			goto err;
		}
	}

	res->next = input_head;
	if (res->next)
		res->next->prev = res;
	input_head = res;

	return POM_OK;

err:
	if (res->reg_instance)
		registry_remove_instance(res->reg_instance);

	free(res);

	return POM_ERR;
}

int input_instance_remove(struct registry_instance *ri) {

	struct input *i = ri->priv;
	
	int running = i->running & INPUT_RUN_RUNNING;
	if (running && registry_set_param(i->reg_instance, "running", "0") != POM_OK) {
		return POM_ERR;
	}

	if (i->priv && i->reg->info->cleanup) {
		if (i->reg->info->cleanup(i) != POM_OK) {
			pomlog(POMLOG_ERR "Error while cleaning up input");
			return POM_ERR;
		}
	}

	if (i->name)
		free(i->name);

	if (i->prev)
		i->prev->next = i->next;
	else
		input_head = i->next;

	if (i->next)
		i->next->prev = i->prev;

	free(i);

	return POM_OK;
}

int input_instance_start_stop_handler(void *priv, struct registry_param *p, struct ptype *run) {

	struct input *i = priv;

	char *new_state = PTYPE_BOOL_GETVAL(run);

	if (i->running & INPUT_RUN_BUSY) {
		pomlog(POMLOG_INFO "Input %s is busy starting/stopping and cannot be changed yet.", i->name);
		return POM_ERR;
	}

	char cur_state = i->running & INPUT_RUN_RUNNING;

	if (cur_state == *new_state) {
		pomlog(POMLOG_ERR "Error, input is already %s", (cur_state ? "running" : "stopped"));
		return POM_ERR;
	}

	__sync_fetch_and_or(&i->running, INPUT_RUN_BUSY);
	if (*new_state) {

		struct input *tmp;
		for (tmp = input_head; tmp; tmp = tmp->next) {
			if (tmp != i) {
			
				if (tmp->running & INPUT_RUN_RUNNING) {

					// Don't start any other input if a non-live input is running
					if (!(tmp->reg->info->flags & INPUT_REG_FLAG_LIVE)) {
						pomlog(POMLOG_ERR "When using non-live input, only one input can be started at once");
						goto err;
					}

					// Don't start a non live input if other inputs are running
					if (!(i->reg->info->flags & INPUT_REG_FLAG_LIVE)) {
						pomlog(POMLOG_ERR "Non-live input cannot be started while live inputs are running");
						goto err;
					}
				}
			}
		}


		core_pause_processing();

		if (i->reg->info->open && i->reg->info->open(i) != POM_OK) {
			core_resume_processing();
			pomlog(POMLOG_ERR "Error while starting input %s", i->name);
			goto err;
		}
		core_resume_processing();

		__sync_fetch_and_or(&i->running, INPUT_RUN_RUNNING);

		if (pthread_create(&i->thread, NULL, input_process_thread, (void*) i)) {
			pomlog(POMLOG_ERR "Unable to start a new thread for input %s : %s", i->name, pom_strerror(errno));
			goto err;
		}

		if (__sync_add_and_fetch(&input_cur_running, 1))
			core_set_state(core_state_running);
		
	} else {

		__sync_fetch_and_and(&i->running, ~INPUT_RUN_RUNNING);

		if (i->reg->info->interrupt && i->reg->info->interrupt(i) == POM_ERR) {
			pomlog(POMLOG_WARN "Warning : error while interrupting the read process of the input");
		}

		if (i->thread != pthread_self()) {
			if (pthread_join(i->thread, NULL))
				pomlog(POMLOG_WARN "Error while joining the input thread : %s", pom_strerror(errno));
		} else {
			if (pthread_detach(i->thread))
				pomlog(POMLOG_WARN "Error while detaching the input thread : %s", pom_strerror(errno));
		}

		if (!__sync_sub_and_fetch(&input_cur_running, 1))
			core_set_state(core_state_finishing);
	}

	__sync_fetch_and_and(&i->running, ~INPUT_RUN_BUSY);

	return POM_OK;

err:
	__sync_fetch_and_and(&i->running, ~INPUT_RUN_BUSY);
	return POM_ERR;

}

int input_stop(struct input *i) {

	// This is called by the inputs to terminate cleanly
	// The input is already locked
	
	if (i->running & INPUT_RUN_RUNNING)
		return registry_set_param(i->reg_instance, "running", "no");

	return POM_OK;
}

int input_stop_all() {

	registry_lock();
	struct input *tmp;
	for (tmp = input_head; tmp ; tmp = tmp->next) {
		if (tmp->running & INPUT_RUN_RUNNING)
			registry_set_param(tmp->reg_instance, "running", "no");
	}
	registry_unlock();

	return POM_OK;
}


void *input_process_thread(void *param) {

	struct input *i = param;

	pomlog("Input %s started", i->name);
	registry_perf_timeticks_restart(i->perf_runtime);

	while (i->running & INPUT_RUN_RUNNING) {
	
		if (i->reg->info->read(i) != POM_OK) {
			if (i->running & INPUT_RUN_RUNNING) {
				// This will update the value of i->running
				pomlog(POMLOG_ERR "Error while reading from input %s", i->name);
				registry_set_param(i->reg_instance, "running" , "0");
			}
		}

	}

	core_pause_processing();
	if (i->reg->info->close && i->reg->info->close(i) != POM_OK) {
		pomlog(POMLOG_WARN "Error while stopping input %s", i->name);
	}
	core_resume_processing();

	__sync_fetch_and_and(&i->running, ~INPUT_RUN_RUNNING);

	registry_perf_timeticks_stop(i->perf_runtime);
	pomlog("Input %s stopped", i->name);

	return NULL;

}


int input_add_param(struct input *i, struct registry_param *p) {

	if (!(p->flags & (REGISTRY_PARAM_FLAG_NOT_LOCKED_WHILE_RUNNING | REGISTRY_PARAM_FLAG_IMMUTABLE)))
		registry_param_set_callbacks(p, i, input_param_locked_while_running, NULL);

	return registry_instance_add_param(i->reg_instance, p);
}

int input_param_locked_while_running(void *input, struct registry_param *p, char *param) {

	struct input *i = input;

	if (i->running) {
		pomlog("Parameter '%s' cannot be changed while input %s is running", p->name, i->name);
		return POM_ERR;
	}

	return POM_OK;
}
