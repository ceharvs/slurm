/*****************************************************************************\
 *  salloc.c - Request a SLURM job allocation and
 *             launch a user-specified command.
 *****************************************************************************
 *  Copyright (C) 2006-2007 The Regents of the University of California.
 *  Copyright (C) 2008-2010 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Christopher J. Morrone <morrone2@llnl.gov>
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <https://computing.llnl.gov/linux/slurm/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#if HAVE_CONFIG_H
#  include "config.h"
#endif

#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <slurm/slurm.h>

#include "src/common/basil_resv_conf.h"
#include "src/common/env.h"
#include "src/common/read_config.h"
#include "src/common/slurm_rlimits_info.h"
#include "src/common/uid.h"
#include "src/common/xmalloc.h"
#include "src/common/xsignal.h"
#include "src/common/xstring.h"
#include "src/common/plugstack.h"

#include "src/salloc/salloc.h"
#include "src/salloc/opt.h"

#ifdef HAVE_BG
#include "src/common/node_select.h"
#include "src/plugins/select/bluegene/plugin/bg_boot_time.h"
#include "src/plugins/select/bluegene/wrap_rm_api.h"
#endif

#ifdef HAVE_CRAY_XT
#include "src/common/node_select.h"
#endif

#define MAX_RETRIES	10
#define POLL_SLEEP	3	/* retry interval in seconds  */

char **command_argv;
int command_argc;
pid_t command_pid = -1;
char *work_dir = NULL;

enum possible_allocation_states allocation_state = NOT_GRANTED;
pthread_mutex_t allocation_state_lock = PTHREAD_MUTEX_INITIALIZER;

static bool exit_flag = false;
static bool allocation_interrupted = false;
static uint32_t pending_job_id = 0;
static time_t last_timeout = 0;

static int  _fill_job_desc_from_opts(job_desc_msg_t *desc);
static void _ring_terminal_bell(void);
static pid_t  _fork_command(char **command);
static void _forward_signal(int signo);
static void _pending_callback(uint32_t job_id);
static void _ignore_signal(int signo);
static void _exit_on_signal(int signo);
static void _signal_while_allocating(int signo);
static void _job_complete_handler(srun_job_complete_msg_t *msg);
static void _set_exit_code(void);
static void _set_rlimits(char **env);
static void _set_spank_env(void);
static void _set_submit_dir_env(void);
static void _timeout_handler(srun_timeout_msg_t *msg);
static void _user_msg_handler(srun_user_msg_t *msg);
static void _ping_handler(srun_ping_msg_t *msg);
static void _node_fail_handler(srun_node_fail_msg_t *msg);

#ifdef HAVE_BG
static int _wait_bluegene_block_ready(
			resource_allocation_response_msg_t *alloc);
static int _blocks_dealloc(void);
#else
static int _wait_nodes_ready(resource_allocation_response_msg_t *alloc);
#endif

#ifdef HAVE_CRAY_XT
static int  _claim_reservation(resource_allocation_response_msg_t *alloc);
#endif

int main(int argc, char *argv[])
{
	log_options_t logopt = LOG_OPTS_STDERR_ONLY;
	job_desc_msg_t desc;
	resource_allocation_response_msg_t *alloc;
	time_t before, after;
	allocation_msg_thread_t *msg_thr;
	char **env = NULL;
	int status = 0;
	int errnum = 0;
	int retries = 0;
	pid_t pid;
	pid_t rc_pid = 0;
	int rc = 0;
	static char *msg = "Slurm job queue full, sleeping and retrying.";
	slurm_allocation_callbacks_t callbacks;

	log_init(xbasename(argv[0]), logopt, 0, NULL);

	_set_exit_code();
	if (spank_init_allocator() < 0) {
		error("Failed to initialize plugin stack");
		exit(error_exit);
	}

	/* Be sure to call spank_fini when salloc exits
	 */
	if (atexit((void (*) (void)) spank_fini) < 0)
		error("Failed to register atexit handler for plugins: %m");


	if (initialize_and_process_args(argc, argv) < 0) {
		error("salloc parameter parsing");
		exit(error_exit);
	}
	/* reinit log with new verbosity (if changed by command line) */
	if (opt.verbose || opt.quiet) {
		logopt.stderr_level += opt.verbose;
		logopt.stderr_level -= opt.quiet;
		logopt.prefix_level = 1;
		log_alter(logopt, 0, NULL);
	}

	if (spank_init_post_opt() < 0) {
		error("Plugin stack post-option processing failed");
		exit(error_exit);
	}

	_set_spank_env();
	_set_submit_dir_env();
	if (opt.cwd && chdir(opt.cwd)) {
		error("chdir(%s): %m", opt.cwd);
		exit(error_exit);
	}

	if (opt.get_user_env_time >= 0) {
		char *user = uid_to_string(opt.uid);
		if (strcmp(user, "nobody") == 0) {
			error("Invalid user id %u: %m", (uint32_t)opt.uid);
			exit(error_exit);
		}
		env = env_array_user_default(user,
					     opt.get_user_env_time,
					     opt.get_user_env_mode);
		xfree(user);
		if (env == NULL)
			exit(error_exit);    /* error already logged */
		_set_rlimits(env);
	}

	/*
	 * Request a job allocation
	 */
	slurm_init_job_desc_msg(&desc);
	if (_fill_job_desc_from_opts(&desc) == -1) {
		exit(error_exit);
	}
	if (opt.gid != (gid_t) -1) {
		if (setgid(opt.gid) < 0) {
			error("setgid: %m");
			exit(error_exit);
		}
	}

	callbacks.ping = _ping_handler;
	callbacks.timeout = _timeout_handler;
	callbacks.job_complete = _job_complete_handler;
	callbacks.user_msg = _user_msg_handler;
	callbacks.node_fail = _node_fail_handler;
	/* create message thread to handle pings and such from slurmctld */
	msg_thr = slurm_allocation_msg_thr_create(&desc.other_port,
						  &callbacks);

	xsignal(SIGHUP, _signal_while_allocating);
	xsignal(SIGINT, _signal_while_allocating);
	xsignal(SIGQUIT, _signal_while_allocating);
	xsignal(SIGPIPE, _signal_while_allocating);
	xsignal(SIGTERM, _signal_while_allocating);
	xsignal(SIGUSR1, _signal_while_allocating);
	xsignal(SIGUSR2, _signal_while_allocating);

	before = time(NULL);
	while ((alloc = slurm_allocate_resources_blocking(&desc, opt.immediate,
					_pending_callback)) == NULL) {
		if ((errno != ESLURM_ERROR_ON_DESC_TO_RECORD_COPY) ||
		    (retries >= MAX_RETRIES))
			break;
		if (retries == 0)
			error(msg);
		else
			debug(msg);
		sleep (++retries);
	}

	/* become the user after the allocation has been requested. */
	if (opt.uid != (uid_t) -1) {
		if (setuid(opt.uid) < 0) {
			error("setuid: %m");
			exit(error_exit);
		}
	}
	if (alloc == NULL) {
		if (allocation_interrupted) {
			/* cancelled by signal */
		} else if (errno == EINTR) {
			error("Interrupted by signal."
			      "  Allocation request rescinded.");
		} else if (opt.immediate &&
			   ((errno == ETIMEDOUT) ||
			    (errno == ESLURM_NOT_TOP_PRIORITY) ||
			    (errno == ESLURM_NODES_BUSY))) {
			error("Unable to allocate resources: %m");
			error_exit = immediate_exit;
		} else {
			error("Failed to allocate resources: %m");
		}
		slurm_allocation_msg_thr_destroy(msg_thr);
		exit(error_exit);
	} else if (!allocation_interrupted) {
		/*
		 * Allocation granted!
		 */
		info("Granted job allocation %u", alloc->job_id);
		pending_job_id = alloc->job_id;
#ifdef HAVE_BG
		if (!_wait_bluegene_block_ready(alloc)) {
			if(!allocation_interrupted)
				error("Something is wrong with the "
				      "boot of the block.");
			goto relinquish;
		}
#else
		if (!_wait_nodes_ready(alloc)) {
			if(!allocation_interrupted)
				error("Something is wrong with the "
				      "boot of the nodes.");
			goto relinquish;
		}	
#endif
#ifdef HAVE_CRAY_XT
		if (!_claim_reservation(alloc)) {
			if(!allocation_interrupted)
				error("Something is wrong with the ALPS "
				      "resource reservation.");
			goto relinquish;
		}
#endif
	}

	after = time(NULL);

	xsignal(SIGHUP,  _exit_on_signal);
	xsignal(SIGINT,  _ignore_signal);
	xsignal(SIGQUIT, _ignore_signal);
	xsignal(SIGPIPE, _ignore_signal);
	xsignal(SIGTERM, _forward_signal);
	xsignal(SIGUSR1, _ignore_signal);
	xsignal(SIGUSR2, _ignore_signal);

	if (opt.bell == BELL_ALWAYS
	    || (opt.bell == BELL_AFTER_DELAY
		&& ((after - before) > DEFAULT_BELL_DELAY))) {
		_ring_terminal_bell();
	}
	if (opt.no_shell)
		exit(0);
	if (allocation_interrupted) {
		/* salloc process received a signal after
		 * slurm_allocate_resources_blocking returned with the
		 * allocation, but before the new signal handlers were
		 * registered.
		 */
		goto relinquish;
	}

	/*
	 * Run the user's command.
	 */
	if (env_array_for_job(&env, alloc, &desc) != SLURM_SUCCESS)
		goto relinquish;

	/* Add default task count for srun, if not already set */
	if (opt.ntasks_set) {
		env_array_append_fmt(&env, "SLURM_NTASKS", "%d", opt.ntasks);
		/* keep around for old scripts */
		env_array_append_fmt(&env, "SLURM_NPROCS", "%d", opt.ntasks);
	}
	if (opt.cpus_per_task > 1) {
		env_array_append_fmt(&env, "SLURM_CPUS_PER_TASK", "%d",
				     opt.cpus_per_task);
	}
	if (opt.overcommit) {
		env_array_append_fmt(&env, "SLURM_OVERCOMMIT", "%d",
			opt.overcommit);
	}
	if (opt.acctg_freq >= 0) {
		env_array_append_fmt(&env, "SLURM_ACCTG_FREQ", "%d",
			opt.acctg_freq);
	}
	if (opt.network)
		env_array_append_fmt(&env, "SLURM_NETWORK", "%s", opt.network);

	env_array_set_environment(env);
	env_array_free(env);
	pthread_mutex_lock(&allocation_state_lock);
	if (allocation_state == REVOKED) {
		error("Allocation was revoked for job %u before command could "
		      "be run", alloc->job_id);
		pthread_mutex_unlock(&allocation_state_lock);
		if (slurm_complete_job(alloc->job_id, status) != 0) {
			error("Unable to clean up allocation for job %u: %m",
			      alloc->job_id);
		}
		return 1;
	} else {
		allocation_state = GRANTED;
		pthread_mutex_unlock(&allocation_state_lock);
		command_pid = pid = _fork_command(command_argv);
	}

	/*
	 * Wait for command to exit, OR for waitpid to be interrupted by a
	 * signal.  Either way, we are going to release the allocation next.
	 */
	if (pid > 0) {
		while ((rc_pid = waitpid(pid, &status, 0)) == -1) {
			if (exit_flag)
				break;
			if (errno == EINTR)
				continue;
		}
		errnum = errno;
		if ((rc_pid == -1) && (errnum != EINTR))
			error("waitpid for %s failed: %m", command_argv[0]);
	}

	/*
	 * Relinquish the job allocation (if not already revoked).
	 */
relinquish:
	pthread_mutex_lock(&allocation_state_lock);
	if (allocation_state != REVOKED) {
		info("Relinquishing job allocation %d", alloc->job_id);
		if (slurm_complete_job(alloc->job_id, status)
		    != 0)
			error("Unable to clean up job allocation %d: %m",
			      alloc->job_id);
		else
			allocation_state = REVOKED;
	}
	pthread_mutex_unlock(&allocation_state_lock);

	slurm_free_resource_allocation_response_msg(alloc);
	slurm_allocation_msg_thr_destroy(msg_thr);

	/*
	 * Figure out what return code we should use.  If the user's command
	 * exited normally, return the user's return code.
	 */
	rc = 1;
	if (rc_pid != -1) {

		if (WIFEXITED(status)) {
			rc = WEXITSTATUS(status);
		} else if (WIFSIGNALED(status)) {
			verbose("Command \"%s\" was terminated by signal %d",
				command_argv[0], WTERMSIG(status));
			/* if we get these signals we return a normal
			 * exit since this was most likely sent from the
			 * user */
			switch(WTERMSIG(status)) {
			case SIGHUP:
			case SIGINT:
			case SIGQUIT:
			case SIGKILL:
				rc = 0;
				break;
			default:
				break;
			}
		}
	}
	return rc;
}

static void _set_exit_code(void)
{
	int i;
	char *val;

	if ((val = getenv("SLURM_EXIT_ERROR"))) {
		i = atoi(val);
		if (i == 0)
			error("SLURM_EXIT_ERROR has zero value");
		else
			error_exit = i;
	}

	if ((val = getenv("SLURM_EXIT_IMMEDIATE"))) {
		i = atoi(val);
		if (i == 0)
			error("SLURM_EXIT_IMMEDIATE has zero value");
		else
			immediate_exit = i;
	}
}

/* Propagate SPANK environment via SLURM_SPANK_ environment variables */
static void _set_spank_env(void)
{
	int i;

	for (i=0; i<opt.spank_job_env_size; i++) {
		if (setenvfs("SLURM_SPANK_%s", opt.spank_job_env[i]) < 0) {
			error("unable to set %s in environment",
			      opt.spank_job_env[i]);
		}
	}
}

/* Set SLURM_SUBMIT_DIR environment variable with current state */
static void _set_submit_dir_env(void)
{
	work_dir = xmalloc(MAXPATHLEN + 1);
	if ((getcwd(work_dir, MAXPATHLEN)) == NULL) {
		error("getcwd failed: %m");
		exit(error_exit);
	}

	if (setenvf(NULL, "SLURM_SUBMIT_DIR", "%s", work_dir) < 0) {
		error("unable to set SLURM_SUBMIT_DIR in environment");
		return;
	}
}

/* Returns 0 on success, -1 on failure */
static int _fill_job_desc_from_opts(job_desc_msg_t *desc)
{
	desc->contiguous = opt.contiguous ? 1 : 0;
	desc->features = opt.constraints;
	desc->gres = opt.gres;
	if (opt.immediate == 1)
		desc->immediate = 1;
	desc->name = xstrdup(opt.job_name);
	desc->reservation = xstrdup(opt.reservation);
	desc->wckey  = xstrdup(opt.wckey);

	desc->req_nodes = opt.nodelist;
	desc->exc_nodes = opt.exc_nodes;
	desc->partition = opt.partition;
	desc->min_nodes = opt.min_nodes;
	if (opt.max_nodes)
		desc->max_nodes = opt.max_nodes;
	desc->user_id = opt.uid;
	desc->group_id = opt.gid;
	if (opt.dependency)
		desc->dependency = xstrdup(opt.dependency);

	if (opt.cpu_bind)
		desc->cpu_bind       = opt.cpu_bind;
	if (opt.cpu_bind_type)
		desc->cpu_bind_type  = opt.cpu_bind_type;
	if (opt.mem_bind)
		desc->mem_bind       = opt.mem_bind;
	if (opt.mem_bind_type)
		desc->mem_bind_type  = opt.mem_bind_type;
	if (opt.plane_size != NO_VAL)
		desc->plane_size     = opt.plane_size;
	desc->task_dist  = opt.distribution;
	if (opt.plane_size != NO_VAL)
		desc->plane_size = opt.plane_size;

	if (opt.licenses)
		desc->licenses = xstrdup(opt.licenses);
	desc->network = opt.network;
	if (opt.nice)
		desc->nice = NICE_OFFSET + opt.nice;
	desc->mail_type = opt.mail_type;
	if (opt.mail_user)
		desc->mail_user = xstrdup(opt.mail_user);
	if (opt.begin)
		desc->begin_time = opt.begin;
	if (opt.account)
		desc->account = xstrdup(opt.account);
	if (opt.acctg_freq >= 0)
		desc->acctg_freq = opt.acctg_freq;
	if (opt.comment)
		desc->comment = xstrdup(opt.comment);
	if (opt.qos)
		desc->qos = xstrdup(opt.qos);

	if (opt.cwd)
		desc->work_dir = xstrdup(opt.cwd);
	else if (work_dir)
		desc->work_dir = xstrdup(work_dir);

	if (opt.hold)
		desc->priority     = 0;
#ifdef HAVE_BG
	if (opt.geometry[0] > 0) {
		int i;
		for (i=0; i<SYSTEM_DIMENSIONS; i++)
			desc->geometry[i] = opt.geometry[i];
	}
#endif
	if (opt.conn_type != (uint16_t)NO_VAL)
		desc->conn_type = opt.conn_type;
	if (opt.reboot)
		desc->reboot = 1;
	if (opt.no_rotate)
		desc->rotate = 0;
	if (opt.blrtsimage)
		desc->blrtsimage = xstrdup(opt.blrtsimage);
	if (opt.linuximage)
		desc->linuximage = xstrdup(opt.linuximage);
	if (opt.mloaderimage)
		desc->mloaderimage = xstrdup(opt.mloaderimage);
	if (opt.ramdiskimage)
		desc->ramdiskimage = xstrdup(opt.ramdiskimage);

	/* job constraints */
	if (opt.mincpus > -1)
		desc->pn_min_cpus = opt.mincpus;
	if (opt.realmem > -1)
		desc->pn_min_memory = opt.realmem;
	else if (opt.mem_per_cpu > -1)
		desc->pn_min_memory = opt.mem_per_cpu | MEM_PER_CPU;
	if (opt.tmpdisk > -1)
		desc->pn_min_tmp_disk = opt.tmpdisk;
	if (opt.overcommit) {
		desc->min_cpus = opt.min_nodes;
		desc->overcommit = opt.overcommit;
	} else
		desc->min_cpus = opt.ntasks * opt.cpus_per_task;
	if (opt.ntasks_set)
		desc->num_tasks = opt.ntasks;
	if (opt.cpus_set)
		desc->cpus_per_task = opt.cpus_per_task;
	if (opt.ntasks_per_node > -1)
		desc->ntasks_per_node = opt.ntasks_per_node;
	if (opt.ntasks_per_socket > -1)
		desc->ntasks_per_socket = opt.ntasks_per_socket;
	if (opt.ntasks_per_core > -1)
		desc->ntasks_per_core = opt.ntasks_per_core;

	/* node constraints */
	if (opt.sockets_per_node != NO_VAL)
		desc->sockets_per_node = opt.sockets_per_node;
	if (opt.cores_per_socket != NO_VAL)
		desc->cores_per_socket = opt.cores_per_socket;
	if (opt.threads_per_core != NO_VAL)
		desc->threads_per_core = opt.threads_per_core;

	if (opt.no_kill)
		desc->kill_on_node_fail = 0;
	if (opt.time_limit  != NO_VAL)
		desc->time_limit = opt.time_limit;
	if (opt.time_min  != NO_VAL)
		desc->time_min = opt.time_min;
	desc->shared = opt.shared;
	desc->job_id = opt.jobid;

	desc->wait_all_nodes = opt.wait_all_nodes;
	if (opt.warn_signal)
		desc->warn_signal = opt.warn_signal;
	if (opt.warn_time)
		desc->warn_time = opt.warn_time;

	if (opt.spank_job_env_size) {
		desc->spank_job_env      = opt.spank_job_env;
		desc->spank_job_env_size = opt.spank_job_env_size;
	}

	return 0;
}

static void _ring_terminal_bell(void)
{
        if (isatty(STDOUT_FILENO)) {
                fprintf(stdout, "\a");
                fflush(stdout);
        }
}

/* returns the pid of the forked command, or <0 on error */
static pid_t _fork_command(char **command)
{
	pid_t pid;

	pid = fork();
	if (pid < 0) {
		error("fork failed: %m");
	} else if (pid == 0) {
		/* child */
		execvp(command[0], command);

		/* should only get here if execvp failed */
		error("Unable to exec command \"%s\"", command[0]);
		exit(error_exit);
	}
	/* parent returns */
	return pid;
}

static void _pending_callback(uint32_t job_id)
{
	info("Pending job allocation %u", job_id);
	pending_job_id = job_id;
}

static void _signal_while_allocating(int signo)
{
	allocation_interrupted = true;
	if (pending_job_id != 0) {
		slurm_complete_job(pending_job_id, 0);
	}
}

static void _ignore_signal(int signo)
{
	/* do nothing */
}

static void _exit_on_signal(int signo)
{
	exit_flag = true;
}

static void _forward_signal(int signo)
{
	if (command_pid > 0)
		kill(command_pid, signo);
}

/* This typically signifies the job was cancelled by scancel */
static void _job_complete_handler(srun_job_complete_msg_t *comp)
{
	if (pending_job_id && (pending_job_id != comp->job_id)) {
		error("Ignoring bogus job_complete call: job %u is not "
		      "job %u", pending_job_id, comp->job_id);
		return;
	}

	if (comp->step_id == NO_VAL) {
		pthread_mutex_lock(&allocation_state_lock);
		if (allocation_state != REVOKED) {
			/* If the allocation_state is already REVOKED, then
			 * no need to print this message.  We probably
			 * relinquished the allocation ourself.
			 */
			if (last_timeout && (last_timeout < time(NULL))) {
				info("Job %u has exceeded its time limit and "
				     "its allocation has been revoked.",
				     comp->job_id);
			} else {
				info("Job allocation %u has been revoked.",
				     comp->job_id);
			}
		}
		if (allocation_state == GRANTED
		    && command_pid > -1
		    && opt.kill_command_signal_set) {
			verbose("Sending signal %d to command \"%s\", pid %d",
				opt.kill_command_signal,
				command_argv[0], command_pid);
			kill(command_pid, opt.kill_command_signal);
		}
		allocation_state = REVOKED;
		pthread_mutex_unlock(&allocation_state_lock);
	} else {
		verbose("Job step %u.%u is finished.",
			comp->job_id, comp->step_id);
	}
}

/*
 * Job has been notified of it's approaching time limit.
 * Job will be killed shortly after timeout.
 * This RPC can arrive multiple times with the same or updated timeouts.
 * FIXME: We may want to signal the job or perform other action for this.
 * FIXME: How much lead time do we want for this message? Some jobs may
 *	require tens of minutes to gracefully terminate.
 */
static void _timeout_handler(srun_timeout_msg_t *msg)
{
	if (msg->timeout != last_timeout) {
		last_timeout = msg->timeout;
		verbose("Job allocation time limit to be reached at %s",
			ctime(&msg->timeout));
	}
}

static void _user_msg_handler(srun_user_msg_t *msg)
{
	info("%s", msg->msg);
}

static void _ping_handler(srun_ping_msg_t *msg)
{
	/* the api will respond so there really isn't anything to do
	   here */
}

static void _node_fail_handler(srun_node_fail_msg_t *msg)
{
	error("Node failure on %s", msg->nodelist);
}

static void _set_rlimits(char **env)
{
	slurm_rlimits_info_t *rli;
	char env_name[25] = "SLURM_RLIMIT_";
	char *env_value, *p;
	struct rlimit r;
	//unsigned long env_num;
	rlim_t env_num;

	for (rli=get_slurm_rlimits_info(); rli->name; rli++) {
		if (rli->propagate_flag != PROPAGATE_RLIMITS)
			continue;
		strcpy(&env_name[sizeof("SLURM_RLIMIT_")-1], rli->name);
		env_value = getenvp(env, env_name);
		if (env_value == NULL)
			continue;
		unsetenvp(env, env_name);
		if (getrlimit(rli->resource, &r) < 0) {
			error("getrlimit(%s): %m", env_name+6);
			continue;
		}
		env_num = strtol(env_value, &p, 10);
		if (p && (p[0] != '\0')) {
			error("Invalid environment %s value %s",
			      env_name, env_value);
			continue;
		}
		if (r.rlim_cur == env_num)
			continue;
		r.rlim_cur = (rlim_t) env_num;
		if (setrlimit(rli->resource, &r) < 0) {
			error("setrlimit(%s): %m", env_name+6);
			continue;
		}
	}
}

#ifdef HAVE_BG
/* returns 1 if job and nodes are ready for job to begin, 0 otherwise */
static int _wait_bluegene_block_ready(resource_allocation_response_msg_t *alloc)
{
	int is_ready = 0, i, rc;
	char *block_id = NULL;
	int cur_delay = 0;
	int max_delay = BG_FREE_PREVIOUS_BLOCK + BG_MIN_BLOCK_BOOT +
			(BG_INCR_BLOCK_BOOT * alloc->node_cnt);

	pending_job_id = alloc->job_id;
	select_g_select_jobinfo_get(alloc->select_jobinfo,
				    SELECT_JOBDATA_BLOCK_ID,
				    &block_id);

	for (i=0; (cur_delay < max_delay); i++) {
		if (i == 1)
			info("Waiting for block %s to become ready for job",
			     block_id);
		if (i) {
			sleep(POLL_SLEEP);
			rc = _blocks_dealloc();
			if ((rc == 0) || (rc == -1))
				cur_delay += POLL_SLEEP;
			debug("still waiting");
		}

		rc = slurm_job_node_ready(alloc->job_id);

		if (rc == READY_JOB_FATAL)
			break;				/* fatal error */
		if ((rc == READY_JOB_ERROR) || (rc == EAGAIN))
			continue;			/* retry */
		if ((rc & READY_JOB_STATE) == 0)	/* job killed */
			break;
		if (rc & READY_NODE_STATE) {		/* job and node ready */
			is_ready = 1;
			break;
		}
		if (allocation_interrupted)
			break;
	}
	if (is_ready)
     		info("Block %s is ready for job", block_id);
	else if (!allocation_interrupted)
		error("Block %s still not ready", block_id);
	else	/* allocation_interrupted and slurmctld not responing */
		is_ready = 0;

	xfree(block_id);
	pending_job_id = 0;

	return is_ready;
}

/*
 * Test if any BG blocks are in deallocating state since they are
 * probably related to this job we will want to sleep longer
 * RET	1:  deallocate in progress
 *	0:  no deallocate in progress
 *     -1: error occurred
 */
static int _blocks_dealloc(void)
{
	static block_info_msg_t *bg_info_ptr = NULL, *new_bg_ptr = NULL;
	int rc = 0, error_code = 0, i;

	if (bg_info_ptr) {
		error_code = slurm_load_block_info(bg_info_ptr->last_update,
						   &new_bg_ptr);
		if (error_code == SLURM_SUCCESS)
			slurm_free_block_info_msg(bg_info_ptr);
		else if (slurm_get_errno() == SLURM_NO_CHANGE_IN_DATA) {
			error_code = SLURM_SUCCESS;
			new_bg_ptr = bg_info_ptr;
		}
	} else {
		error_code = slurm_load_block_info((time_t) NULL,
						   &new_bg_ptr);
	}

	if (error_code) {
		error("slurm_load_partitions: %s",
		      slurm_strerror(slurm_get_errno()));
		return -1;
	}
	for (i=0; i<new_bg_ptr->record_count; i++) {
		if(new_bg_ptr->block_array[i].state
		   == RM_PARTITION_DEALLOCATING) {
			rc = 1;
			break;
		}
	}
	bg_info_ptr = new_bg_ptr;
	return rc;
}
#else
/* returns 1 if job and nodes are ready for job to begin, 0 otherwise */
static int _wait_nodes_ready(resource_allocation_response_msg_t *alloc)
{
	int is_ready = 0, i, rc;
	int cur_delay = 0;
	int suspend_time, resume_time, max_delay;

	suspend_time = slurm_get_suspend_timeout();
	resume_time  = slurm_get_resume_timeout();
	if ((suspend_time == 0) || (resume_time == 0))
		return 1;	/* Power save mode disabled */
	max_delay = suspend_time + resume_time;
	max_delay *= 5;		/* Allow for ResumeRate support */

	pending_job_id = alloc->job_id;

	if (opt.wait_all_nodes == (uint16_t) NO_VAL)
		opt.wait_all_nodes = DEFAULT_WAIT_ALL_NODES;

	for (i=0; (cur_delay < max_delay); i++) {
		if (i) {
			if (i == 1)
				info("Waiting for nodes to boot");
			else
				debug("still waiting");
			sleep(POLL_SLEEP);
			cur_delay += POLL_SLEEP;
		}

		if (opt.wait_all_nodes)
			rc = slurm_job_node_ready(alloc->job_id);
		else {
			is_ready = 1;
			break;
		}

		if (rc == READY_JOB_FATAL)
			break;				/* fatal error */
		if ((rc == READY_JOB_ERROR) || (rc == EAGAIN))
			continue;			/* retry */
		if ((rc & READY_JOB_STATE) == 0)	/* job killed */
			break;
		if (rc & READY_NODE_STATE) {		/* job and node ready */
			is_ready = 1;
			break;
		}
		if (allocation_interrupted)
			break;
	}
	if (is_ready) {
		if (i > 0)
     			info ("Nodes %s are ready for job", alloc->node_list);
	} else if (!allocation_interrupted)
		error("Nodes %s are still not ready", alloc->node_list);
	else	/* allocation_interrupted or slurmctld not responing */
		is_ready = 0;

	pending_job_id = 0;

	return is_ready;
}
#endif	/* HAVE_BG */

#ifdef HAVE_CRAY_XT
/* returns 1 if job and nodes are ready for job to begin, 0 otherwise */
static int _claim_reservation(resource_allocation_response_msg_t *alloc)
{
	int rc = 0;
	uint32_t resv_id = 0;

	select_g_select_jobinfo_get(alloc->select_jobinfo,
				    SELECT_JOBDATA_RESV_ID,
				    &resv_id);
	if (!resv_id)
		return rc;
	if (basil_resv_conf(resv_id, alloc->job_id) == SLURM_SUCCESS)
		rc = 1;
	xfree(resv_id);
	return rc;
}
#endif
