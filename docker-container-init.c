#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>


#define LOG_ERROR(fmt, ...)   if (opt_verbosity >= 0) { fprintf(stderr, "[ERROR][docker-container-init] " fmt "\n", ## __VA_ARGS__); }
#define LOG_PERROR(fmt, ...)  if (opt_verbosity >= 0) { fprintf(stderr, "[ERROR][docker-container-init] " fmt " [errno=%d] %s\n", \
																## __VA_ARGS__, errno, strerror(errno)); }
#define LOG_INFO(fmt, ...)    if (opt_verbosity >= 1) { fprintf(stderr, "[INFO][docker-container-init] "  fmt "\n", ## __VA_ARGS__); }
#define LOG_DEBUG(fmt, ...)   if (opt_verbosity >= 2) { fprintf(stderr, "[DEBUG][docker-container-init] " fmt "\n", ## __VA_ARGS__); }

#define ARRAY_LEN(arr)        (sizeof(arr) / sizeof(arr[0]))

typedef enum { false, true } bool;

// Program state

sigset_t used_sigmask, orig_sigmask;
// If subproc_pid > 0 then a subprocess is running.
pid_t subproc_pid = 0;
// This exitcode that will be returned by this init program.
int exitcode = 0;
// true if we have received at least one SIGTERM or SIGINT.
bool exit_signal_received = false;
// NULL if the user hasn't specified a subprocess on the commandline
char** subproc_argv = NULL;

// Commandline args

bool opt_wait_for_children = true;
bool opt_broadcast_sigterm_before_wait = true;
bool opt_create_subproc_group = false;
bool opt_forward_realtime_signals = false;
bool opt_exit_on_sigint = true;
bool opt_check_pid_1 = true;
int opt_verbosity = 0;


const char OPTIONS[] =
	"Options:\n"
	"-W  Don't wait for all children (including inherited/orphaned ones) before\n"
	"    exit. This wait is performed after your command (if any) has exited.\n"
	"-B  Don't broadcast a sigterm before waiting for all children.\n"
	"    This option is ignored when -W is used.\n"
	"-I  Don't exit on SIGINT. Exit only on SIGTERM.\n"
	"    This option is ignored when you specify a command.\n"
	"-g  Run your command in its own process group and forward SIGTERM to the\n"
	"    group instead of the process created from your command.\n"
	"-r  Enable forwarding of realtime signals to the specified command.\n"
	"    Without this option we forward only some of the standard signals.\n"
	"-D  Don't check whether this process is running as pid 1.\n"
	"    Comes in handy for debugging.\n"
	"-v  Log a limited number of info messages to stderr.\n"
	"    Without -v we log only in case of errors.\n"
	"-vv Spammy debug log level.\n"
	"-h  Print this help message.\n"
	;

void print_help_exit(char* argv[]) {
	printf("docker-container-init built on %s %s\n", __DATE__, __TIME__);
#if defined(VERSION)
	printf("version %s\n", VERSION);
#endif
	printf("\nUsage: %s [options] [--] [command]\n\n", argv[0]);
	printf(OPTIONS);
	exit(1);
}

bool parse_args(int argc, char* argv[]) {
	opterr = 0;
	int opt;
	while ( (opt = getopt(argc, argv, "+hWBIgrDv")) != -1 ) {
		switch (opt) {
		case 'h':
			print_help_exit(argv);
			break;
		case 'W':
			opt_wait_for_children = false;
			break;
		case 'B':
			opt_broadcast_sigterm_before_wait = false;
			break;
		case 'I':
			opt_exit_on_sigint = false;
			break;
		case 'g':
			opt_create_subproc_group = true;
			break;
		case 'r':
			opt_forward_realtime_signals = true;
			break;
		case 'D':
			opt_check_pid_1 = false;
			break;
		case 'v':
			opt_verbosity++;
			break;
		case '?':
			opt = optopt;
			// no break
		default:
			// The switch jumps directly to default only if we specify an
			// option in the optstring without a case in our switch statement.
			fprintf(stderr, "Invalid option: -%c\n", opt);
			print_help_exit(argv);
			break;
		}
	}

	if (argv[optind])
		subproc_argv = &argv[optind];
	return true;
}

int used_std_signals[] = { SIGHUP, SIGINT, SIGQUIT, SIGUSR1, SIGUSR2, SIGTERM, SIGCHLD };

bool setup_sigmask() {
	sigemptyset(&used_sigmask);
	// adding standard signals
	for (size_t i=0; i<ARRAY_LEN(used_std_signals); i++)
		sigaddset(&used_sigmask, used_std_signals[i]);
	// adding realtime signals
	if (opt_forward_realtime_signals) {
		for (int signum=SIGRTMIN; signum<=SIGRTMAX; signum++)
			sigaddset(&used_sigmask, signum);
	}
	if (sigprocmask(SIG_BLOCK, &used_sigmask, &orig_sigmask)) {
		LOG_PERROR("sigprocmask failed.");
		return false;
	}

	// We want to be able to write to the terminal even if a child process group
	// gets into the foreground. Note that this mask is inherited by our subprocess
	// and restored only after the call to tcsetpgrp(). This way tcsetpgrp() can
	// succeed in the child process. For more info see the man page of tcsetpgrp.
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGTTOU);
	if (sigprocmask(SIG_BLOCK, &mask, NULL)) {
		LOG_PERROR("sigprocmask failed.");
		return false;
	}

	return true;
}

bool spawn_subproc() {
	LOG_INFO("Spawning subprocess...");
	subproc_pid = fork();
	if (subproc_pid < 0) {
		LOG_PERROR("fork failed.");
		return false;
	}

	// parent
	if (subproc_pid > 0)
		return true;

	// child
	if (opt_create_subproc_group) {
		if (setpgrp()) {
			LOG_PERROR("Error creating process group: setpgrp failed (pid=%d)", (int)getpid());
			exit(1);
		}
		if (tcsetpgrp(STDIN_FILENO, getpgrp()) && errno != ENOTTY) {
			LOG_PERROR("tcsetpgrp failed. (pgid=%d)", (int)getpgrp());
			exit(1);
		}
	}
	// Restoring the sigprocmask only after the tcsetpgrp()
	// call because that requires blocked SIGTTOU signal.
	if (sigprocmask(SIG_SETMASK, &orig_sigmask, NULL)) {
		LOG_PERROR("sigprocmask failed.");
		return false;
	}

	execvp(subproc_argv[0], subproc_argv);
	LOG_PERROR("execvp failed.");
	exit(1);
}

// Returns true if there are no children.
bool reap_zombies_and_subproc() {
	LOG_DEBUG("%s()", __FUNCTION__);
	for (;;) {
		int status;
		pid_t pid = waitpid(-1, &status, WNOHANG);
		LOG_DEBUG("waitpid finished. pid=%d errno=[%d] %s", (int)pid, errno, strerror(errno));

		if (pid == 0)
			return false;

		if (pid < 0) {
			if (errno != ECHILD)
				LOG_PERROR("waitpid failed");
			return true;
		}

		if (pid == subproc_pid) {
			subproc_pid = 0;
			if (WIFEXITED(status)) {
				exitcode = WEXITSTATUS(status);
				LOG_INFO("Subprocess (pid=%d) finished with exit code %d", (int)pid, exitcode);
			} else if (WIFSIGNALED(status)) {
				exitcode = 0x80 | WTERMSIG(status);
				LOG_INFO("Subprocess (pid=%d) was killed by singal=%d", (int)pid, WTERMSIG(status));
			} else {
				exitcode = -1;
				LOG_INFO("Subprocess (pid=%d) disappeared.", (int)pid);
			}
		}
	}
}

bool step_check_subproc_finished(bool first_try) {
	if (!subproc_argv)
		return true;
	if (first_try)
		LOG_INFO("Waiting for subprocess (pid=%d) to finish...", (int)subproc_pid);
	return subproc_pid <= 0;
}

bool step_check_exit_signal_received(bool first_try) {
	if (subproc_argv)
		// If we have a subprocess then the shutdown sequence is initiated by the
		// exit of the subprocess and not directly by a signal (SIGTERM/SIGINT)
		// sent to this process.
		return true;
	if (first_try)
		LOG_INFO("Waiting for SIGTERM%s to exit...", opt_exit_on_sigint ? "/SIGINT" : "");
	return exit_signal_received;
}

bool step_broadcast_sigterm(bool first_try) {
	if (opt_wait_for_children && opt_broadcast_sigterm_before_wait) {
		LOG_INFO("Broadcasting SIGTERM before waiting for children");
		kill(-1, SIGTERM);
	}
	return true;
}

bool step_check_there_are_no_children(bool first_try) {
	if (!opt_wait_for_children)
		return true;
	if (first_try)
		LOG_INFO("Waiting for child processes to finish...");
	return reap_zombies_and_subproc();
}

// fp_step() returns true if the step/check has passed. Otherwise we have to
// try the step/check again later after handling a meaningful signal that
// might have changed the state of our application.
typedef bool (*fp_step)(bool first_try);
const fp_step steps[] = {
	step_check_subproc_finished,
	step_check_exit_signal_received,
	step_broadcast_sigterm,
	step_check_there_are_no_children,
};
const size_t NUM_STEPS = ARRAY_LEN(steps);

// Returns true if this program has finished.
bool update_state() {
	static size_t step_idx = 0;
	static bool first_try = true;

	while (step_idx < NUM_STEPS) {
		LOG_DEBUG("trying step %d/%d (first_try=%d)", (int)step_idx+1, (int)NUM_STEPS, (int)first_try);
		bool step_finished = steps[step_idx](first_try);
		first_try = false;
		if (!step_finished) {
			// If step hasn't finished then we retry after the next signal.
			LOG_DEBUG("step %d/%d hasn't finished, retrying later...", (int)step_idx+1, (int)NUM_STEPS);
			return false;
		}

		LOG_DEBUG("finished step %d/%d", (int)step_idx+1, (int)NUM_STEPS);
		// Resetting first_try for the next step.
		first_try = true;
		step_idx++;
	}
	return true;
}

void forward_signal(int signum) {
	if (subproc_pid <= 0)
		return;
	LOG_DEBUG("Forwarding signal=%d (%s) to subprocess (pid=%d)", signum, strsignal(signum), (int)subproc_pid);
	kill(subproc_pid, signum);
}

// Returns true if the handled signal might have an effect on the internal state of this program.
bool handle_signal(int signum) {
	switch (signum) {
	case SIGCHLD:
		reap_zombies_and_subproc();
		return true;

	case SIGTERM:
		LOG_INFO("Received SIGTERM");
		if (subproc_pid > 0) {
			if (opt_create_subproc_group) {
				LOG_INFO("Forwarding SIGTERM to process group (pgid=%d)", (int)subproc_pid);
				killpg(subproc_pid, SIGTERM);
			} else {
				forward_signal(signum);
			}
		}
		exit_signal_received = true;
		return true;

	case SIGINT:
		if (subproc_argv) {
			forward_signal(signum);
		} else if (opt_exit_on_sigint) {
			exit_signal_received = true;
			return true;
		}
		return false;

	default:
		forward_signal(signum);
		return false;
	}
}

// Handles signals in a loop but returns when it encounters a signal
// that might have an effect on the internal state of this program.
void wait_and_handle_signals() {
	for (;;) {
		LOG_DEBUG("Waiting for signal...");
		int signum;
		int err = sigwait(&used_sigmask, &signum);
		if (err) {
			LOG_ERROR("sigwait failed. [errno=%d] %s", errno, strerror(errno));
			exit(1);
		}

		LOG_DEBUG("Received signal=%d (%s)", signum, strsignal(signum));

		if (handle_signal(signum))
			break;
	}
}

bool init(int argc, char* argv[]) {
	if (!parse_args(argc, argv))
		return false;
	if (opt_check_pid_1 && getpid() != 1) {
		LOG_ERROR("You have to either run this as pid 1 or specify the -D option");
		return false;
	}
	if (!setup_sigmask())
		return false;
	if (subproc_argv && !spawn_subproc())
		return false;
	return true;
}

int main(int argc, char* argv[]) {
	if (!init(argc, argv))
		return 1;

	for (;;) {
		if (update_state())
			break;
		wait_and_handle_signals();
	}

	LOG_INFO("Finished.");
	return exitcode;
}
