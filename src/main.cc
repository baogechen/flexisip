/*
	Flexisip, a flexible SIP proxy server with media capabilities.
	Copyright (C) 2010-2015  Belledonne Communications SARL, All rights reserved.

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU Affero General Public License as
	published by the Free Software Foundation, either version 3 of the
	License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU Affero General Public License for more details.

	You should have received a copy of the GNU Affero General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include "flexisip-config.h"
#endif
#ifndef CONFIG_DIR
#define CONFIG_DIR
#endif
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

#include <iostream>
#include <tclap/CmdLine.h>

#ifdef ENABLE_TRANSCODER
#include <mediastreamer2/msfactory.h>
#endif

#include "agent.hh"
#include "stun.hh"
#include "module.hh"

#include <cstdlib>
#include <cstdio>
#include <csignal>

#include "expressionparser.hh"
#include "configdumper.hh"

#include <sofia-sip/su_log.h>
#include <sofia-sip/msg.h>
#include <sofia-sip/sofia_features.h>
#ifdef ENABLE_SNMP
#include "snmp-agent.h"
#endif
#ifndef VERSION
#define VERSION "DEVEL"
#endif // VERSION

#include "flexisip_gitversion.h"
#ifndef FLEXISIP_GIT_VERSION
#define FLEXISIP_GIT_VERSION "undefined"
#endif

#include "log/logmanager.hh"
#include <ortp/ortp.h>
#include <functional>
#include <list>

#include "etchosts.hh"

#include <fstream>

#ifdef ENABLE_PRESENCE
#include "presence/presence-server.hh"
#include "presence/presence-longterm.hh"
#endif // ENABLE_PRESENCE

#include "monitor.hh"

#include <openssl/opensslconf.h>
#if defined(OPENSSL_THREADS)
// thread support enabled
#else
// no thread support
#error "No thread support in openssl"
#endif

#include <openssl/crypto.h>

static int run = 1;
static int pipe_wdog_flexisip[2] = {
	-1}; // This is the pipe that flexisip will write to to signify it has started to the Watchdog
static pid_t flexisip_pid = -1;
static pid_t monitor_pid = -1;
static su_root_t *root = NULL;

using namespace std;

static unsigned long threadid_cb(){
	return (unsigned long)pthread_self();
}

static void locking_function(int mode, int n, const char *file, int line){
	static mutex *mutextab=NULL;
	if (mutextab==NULL)
		mutextab=new mutex[CRYPTO_num_locks()];
	if (mode & CRYPTO_LOCK)
		mutextab[n].lock();
	else mutextab[n].unlock();
}

static void setOpenSSLThreadSafe(){
	CRYPTO_set_id_callback(&threadid_cb);
	CRYPTO_set_locking_callback(&locking_function);
}

static void flexisip_stop(int signum) {
	if (flexisip_pid > 0) {
		// We can't log from the parent process
		// LOGD("Watchdog received quit signal...passing to child.");
		/*we are the watchdog, pass the signal to our child*/
		kill(flexisip_pid, signum);
	} else if (run != 0) {
		// LOGD("Received quit signal...");
		run = 0;
		if (root) {
			su_root_break(root);
		}
	} //else nop
}

static void flexisip_stat(int signum) {
}

static void sofiaLogHandler(void *, const char *fmt, va_list ap) {
	// remove final \n from sofia
	if (fmt) {
		char* copy= strdup(fmt);
		copy[strlen(copy)-1] = '\0';
		LOGDV(copy, ap);
		free(copy);
	}
}

static void timerfunc(su_root_magic_t *magic, su_timer_t *t, Agent *a) {
	a->idle();
}

static std::map<msg_t *, string> msg_map;

static void flexisip_msg_create(msg_t *msg) {
	msg_map[msg] = "";
	LOGE("New <-> msg %p", msg);
}

static void flexisip_msg_destroy(msg_t *msg) {
	auto it = msg_map.find(msg);
	if (it != msg_map.end()) {
		msg_map.erase(it);
	}
}

static void dump_remaining_msgs() {
	LOGE("### Remaining messages: %lu", (unsigned long)msg_map.size());
	for (auto it = msg_map.begin(); it != msg_map.end(); ++it) {
		LOGE("### \t- %p\n", it->first);
	}
}

static int getSystemFdLimit() {
	static int max_sys_fd = -1;
	if (max_sys_fd == -1) {
#ifdef __linux
		char tmp[256] = {0}; // make valgrind happy
		int fd = open("/proc/sys/fs/file-max", O_RDONLY);
		if (fd != -1) {
			if (read(fd, tmp, sizeof(tmp)) > 0) {
				int val = 0;
				if (sscanf(tmp, "%i", &val) == 1) {
					max_sys_fd = val;
					LOGI("System wide maximum number of file descriptors is %i", max_sys_fd);
				}
			}
			close(fd);
			fd = open("/proc/sys/fs/nr_open", O_RDONLY);
			if (fd != -1) {
				if (read(fd, tmp, sizeof(tmp)) > 0) {
					int val = 0;
					if (sscanf(tmp, "%i", &val) == 1) {
						LOGI("System wide maximum number open files is %i", val);
						if (val < max_sys_fd) {
							max_sys_fd = val;
						}
					}
				}
				close(fd);
			}
		}
#else
		LOGW("Guessing of system wide fd limit is not implemented.");
		max_sys_fd = 2048;
#endif
	}
	return max_sys_fd;
}

static void increase_fd_limit(void) {
	struct rlimit lm;
	if (getrlimit(RLIMIT_NOFILE, &lm) == -1) {
		LOGE("getrlimit(RLIMIT_NOFILE) failed: %s", strerror(errno));
	} else {
		unsigned new_limit = (unsigned)getSystemFdLimit();
		int old_lim = (int)lm.rlim_cur;
		LOGI("Maximum number of open file descriptors is %i, limit=%i, system wide limit=%i", (int)lm.rlim_cur,
			 (int)lm.rlim_max, getSystemFdLimit());

		if (lm.rlim_cur < new_limit) {
			lm.rlim_cur = lm.rlim_max = new_limit;
			if (setrlimit(RLIMIT_NOFILE, &lm) == -1) {
				LOGE("setrlimit(RLIMIT_NOFILE) failed: %s. Limit of number of file descriptors is low (%i).",
					 strerror(errno), old_lim);
				LOGE("Flexisip will not be able to process a big number of calls.");
			}
			if (getrlimit(RLIMIT_NOFILE, &lm) == 0) {
				LOGI("Maximum number of file descriptor set to %i.", (int)lm.rlim_cur);
			}
		}
	}
}

/* Allows to detach the watchdog from the PTY so that we don't get traces clobbering the terminal */
static void detach() {
	int fd;
	setsid();
	fd = open("/dev/null", O_RDWR);
	if (fd == -1) {
		fprintf(stderr, "Could not open /dev/null\n");
		exit(-1);
	}
	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);
	close(fd);
}

static void makePidFile(const char *pidfile) {
	if (pidfile) {
		FILE *f = fopen(pidfile, "w");
		fprintf(f, "%i", getpid());
		fclose(f);
	}
}

static void set_process_name(const char *process_name) {
#ifdef PR_SET_NAME
	if (prctl(PR_SET_NAME, process_name, NULL, NULL, NULL) == -1) {
		LOGW("prctl() failed: %s", strerror(errno));
	}
#endif
}

static void forkAndDetach(const char *pidfile, bool auto_respawn, bool startMonitor) {
	int pipe_launcher_wdog[2];
	int err = pipe(pipe_launcher_wdog);
	bool launcherExited = false;
	if (err == -1) {
		LOGE("Could not create pipes: %s", strerror(errno));
		exit(EXIT_FAILURE);
	}

	/* Creation of the watch-dog process */
	pid_t pid = fork();
	if (pid < 0) {
		fprintf(stderr, "Could not fork: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}
	if (pid == 0) {
		/* We are in the watch-dog process */
		uint8_t buf[4];
		close(pipe_launcher_wdog[0]);
		set_process_name("flexisip_wdog");

	/* Creation of the flexisip process */
	fork_flexisip:
		err = pipe(pipe_wdog_flexisip);
		if (err == -1) {
			LOGE("Could not create pipes: %s", strerror(errno));
			exit(EXIT_FAILURE);
		}
		flexisip_pid = fork();
		if (flexisip_pid < 0) {
			fprintf(stderr, "Could not fork: %s\n", strerror(errno));
			exit(EXIT_FAILURE);
		}
		if (flexisip_pid == 0) {

			/* This is the real flexisip process now.
			 * We can proceed with real start
			 */
			close(pipe_wdog_flexisip[0]);
			set_process_name("flexisip");
			makePidFile(pidfile);
			return;
		} else {
			LOGI("[WDOG] Flexisip PID: %d", flexisip_pid);
		}

		/*
		 * We are in the watch-dog process again
		 * Waiting for successful initialisation of the flexisip process
		 */
		close(pipe_wdog_flexisip[1]);
		err = read(pipe_wdog_flexisip[0], buf, sizeof(buf));
		if (err == -1 || err == 0) {
			int errno_ = errno;
			LOGE("[WDOG] Read error from flexisip : %s", strerror(errno_));
			close(pipe_launcher_wdog[1]); // close launcher pipe to signify the error
			exit(EXIT_FAILURE);
		}
		close(pipe_wdog_flexisip[0]);

	/*
	 * Flexisip has successfully started.
	 * We can now start the Flexisip monitor if it is requierd
	 */
	fork_monitor:
		if (startMonitor) {
			int pipe_wd_mo[2];
			err = pipe(pipe_wd_mo);
			if (err == -1) {
				LOGE("Cannot create pipe. %s", strerror(errno));
				kill(flexisip_pid, SIGTERM);
				exit(EXIT_FAILURE);
			}
			monitor_pid = fork();
			if (monitor_pid < 0) {
				fprintf(stderr, "Could not fork: %s\n", strerror(errno));
				exit(EXIT_FAILURE);
			}
			if (monitor_pid == 0) {
				/* We are in the monitor process */
				set_process_name("flexisip_mon");
				close(pipe_launcher_wdog[1]);
				close(pipe_wd_mo[0]);
				Monitor::exec(pipe_wd_mo[1]);
				LOGE("Fail to launch the Flexisip monitor");
				exit(EXIT_FAILURE);
			}
			/* We are in the watchdog process */
			close(pipe_wd_mo[1]);
			err = read(pipe_wd_mo[0], buf, sizeof(buf));
			if (err == -1 || err == 0) {
				LOGE("[WDOG] Read error from Monitor process, killing flexisip");
				kill(flexisip_pid, SIGTERM);
				exit(EXIT_FAILURE);
			}
			close(pipe_wd_mo[0]);
		}

		/*
		 * We are in the watchdog process once again, and all went well, tell the launcher that it can exit
		 */

		if (!launcherExited && write(pipe_launcher_wdog[1], "ok", 3) == -1) {
			LOGE("[WDOG] Write to pipe failed, exiting");
			exit(EXIT_FAILURE);
		} else {
			close(pipe_launcher_wdog[1]);
			launcherExited = true;
		}

		/* Detach ourselves from the PTY. */
		detach();

		/*
		 * This loop aims to restart childs of the watchdog process
		 * when they have a crash
		 */
		while (true) {
			int status = 0;
			pid_t retpid = wait(&status);
			if (retpid > 0) {
				if (retpid == flexisip_pid) {
					if (startMonitor)
						kill(monitor_pid, SIGTERM);
					if (WIFEXITED(status)) {
						if (WEXITSTATUS(status) == RESTART_EXIT_CODE) {
							LOGI("Flexisip restart to apply new config...");
							sleep(1);
							goto fork_flexisip;
						} else {
							LOGD("Flexisip exited normally");
							exit(EXIT_SUCCESS);
						}
					} else if (auto_respawn) {
						LOGE("Flexisip apparently crashed, respawning now...");
						sleep(1);
						goto fork_flexisip;
					}
				} else if (retpid == monitor_pid) {
					LOGE("The Flexisip monitor has crashed or has been illegally terminated. Restarting now");
					sleep(1);
					goto fork_monitor;
				}
			} else if (errno != EINTR) {
				LOGE("waitpid() error: %s", strerror(errno));
				exit(EXIT_FAILURE);
			}
		}
	} else {
		/* This is the initial process.
		 * It should block until flexisip has started sucessfully or rejected to start.
		 */
		LOGE("[LAUNCHER] Watchdog PID: %d", pid);
		uint8_t buf[4];
		// we don't need the write side of the pipe:
		close(pipe_launcher_wdog[1]);

		// Wait for WDOG to tell us "ok" if all went well, or close the pipe if flexisip failed somehow
		err = read(pipe_launcher_wdog[0], buf, sizeof(buf));
		if (err == -1 || err == 0) {
			// pipe was closed, flexisip failed to start -> exit with failure
			LOGE("[LAUNCHER] Flexisip failed to start.");
			exit(EXIT_FAILURE);
		} else {
			// pipe written to, flexisip was OK
			LOGE("[LAUNCHER] Flexisip started correctly: exit");
			exit(EXIT_SUCCESS);
		}
	}
}

static void depthFirstSearch(string &path, GenericEntry *config, list<string> &allCompletions) {
	GenericStruct *gStruct = dynamic_cast<GenericStruct *>(config);
	if (gStruct) {
		string newpath;
		if (!path.empty())
			newpath += path + "/";
		if (config->getName() != "flexisip")
			newpath += config->getName();
		for (auto it = gStruct->getChildren().cbegin(); it != gStruct->getChildren().cend(); ++it) {
			depthFirstSearch(newpath, *it, allCompletions);
		}
		return;
	}

	ConfigValue *cValue = dynamic_cast<ConfigValue *>(config);
	if (cValue) {
		string completion;
		if (!path.empty())
			completion += path + "/";
		completion += cValue->getName();
		allCompletions.push_back(completion);
	}
}

static int dump_config(su_root_t *root, const std::string &dump_cfg_part, bool with_experimental,
					   const string &format) {
	shared_ptr<Agent> a = make_shared<Agent>(root);
	GenericStruct *rootStruct = GenericManager::get()->getRoot();

	if (dump_cfg_part != "all") {

		size_t prefix_location = dump_cfg_part.find("module::");
		rootStruct = dynamic_cast<GenericStruct *>(rootStruct->find(dump_cfg_part));

		if (dump_cfg_part != "global" && prefix_location != 0) {
			cerr << "Module name should start with 'module::' or be the special module 'global' (was given "
				 << dump_cfg_part << " )" << endl;
			return EXIT_FAILURE;

		} else if (rootStruct == NULL) {
			cerr << "Couldn't find node " << dump_cfg_part << endl;
			return EXIT_FAILURE;

		} else if (prefix_location == 0) {
			string moduleName = dump_cfg_part.substr(strlen("module::"));
			Module *module = a->findModule(moduleName);
			if (module && module->type() == ModuleTypeExperimental && !with_experimental) {
				cerr << "Module " << moduleName
					 << " is experimental, not returning anything. To override, specify '--with-experimental'" << endl;
				return EXIT_FAILURE;
			}
		}
	}
	ConfigDumper *dumper = NULL;
	if (format == "tex") {
		dumper = new TexFileConfigDumper(rootStruct);
	} else if (format == "doku") {
		dumper = new DokuwikiConfigDumper(rootStruct);
	} else if (format == "file") {
		dumper = new FileConfigDumper(rootStruct);
	} else if (format == "media") {
		dumper = new MediaWikiConfigDumper(rootStruct);
	} else {
		cerr << "Invalid output format '" << format << "'" << endl;
		return EXIT_FAILURE;
	}
	dumper->setDumpExperimentalEnabled(with_experimental);
	dumper->dump(cout);
	delete dumper;
	return EXIT_SUCCESS;
}

static void list_modules() {
	shared_ptr<Agent> a = make_shared<Agent>(root);
	GenericStruct *rootStruct = GenericManager::get()->getRoot();
	list<GenericEntry *> children = rootStruct->getChildren();
	for (auto it = children.begin(); it != children.end(); ++it) {
		GenericEntry *child = (*it);
		if (child->getName().find("module::") == 0) {
			cout << child->getName() << endl;
		}
	}
}

static string version() {
	string version = VERSION " (git: " FLEXISIP_GIT_VERSION ")\n";

	version += "sofia-sip version " SOFIA_SIP_VERSION "\n";
	version += "\nCompiled with:\n";
#if ENABLE_SNMP
	version += "- SNMP\n";
#endif
#if ENABLE_TRANSCODER
	version += "- Transcoder\n";
#endif
#if ENABLE_REDIS
	version += "- Redis\n";
#endif
#if ENABLE_PUSHNOTIFICATION
	version += "- Push Notification\n";
#endif
#if ENABLE_ODBC
	version += "- ODBC\n";
#endif
#if ENABLE_SOCI
	version += "- Soci\n";
#endif
#if ENABLE_ODB
	version += "- ODB\n";
#endif
#if ENABLE_PROTOBUF
	version += "- Protobuf\n";
#endif
#if ENABLE_PRESENCE
	version += "- Presence\n";
#endif

	return version;
}

int main(int argc, char *argv[]) {
	shared_ptr<Agent> a;
	StunServer *stun = NULL;
	bool debug;
	map<string, string> oset;

	string versionString = version();

	// clang-format off
	TCLAP::CmdLine cmd("", ' ', versionString);

	TCLAP::ValueArg<string>     configFile("c", "config", 			"Specify the location of the configuration file.", TCLAP::ValueArgOptional, CONFIG_DIR "/flexisip.conf", "file", cmd);
	TCLAP::SwitchArg            daemonMode("",  "daemon", 			"Launch in daemon mode.", cmd);
	TCLAP::SwitchArg              useDebug("d", "debug", 			"Force debug mode (overrides the configuration).", cmd);
	TCLAP::ValueArg<string>        pidFile("p", "pidfile", 			"PID file location, used when running in daemon mode.", TCLAP::ValueArgOptional, "", "file", cmd);
	TCLAP::SwitchArg             useSyslog("",  "syslog", 			"Use syslog for logging.", cmd);
	TCLAP::SwitchArg           trackAllocs("",  "track-allocations","Tracks allocations of SIP messages, only use with caution.", cmd);

	TCLAP::SwitchArg          boolExprEval("", 	"debug-bool-eval",	"Print debug information for the boolean expression evaluation (check the documentation about filters to understand what this is).", cmd);
	TCLAP::SwitchArg         boolParseEval("", 	"debug-bool-parse",	"Print debug information for the boolean expression parsing (check the documentation about filters to understand what this is).", cmd);

	TCLAP::ValueArg<string>  transportsArg("t", "transports", 		"The list of transports to handle (overrides the ones defined in the configuration file).", TCLAP::ValueArgOptional, "", "sips:* sip:*", cmd);

	TCLAP::SwitchArg              dumpMibs("",  "dump-mibs", 		"Will dump the MIB files for Flexisip performance counters and other related SNMP items.", cmd);
	TCLAP::ValueArg<string>    dumpDefault("",  "dump-default",		"Dump default config, with specifier for the module to dump. Use 'all' to dump all modules, or 'MODULENAME' to dump "
										   							"a specific module. For instance, to dump the Router module default config, issue 'flexisip --dump-default module::Router.",
										   TCLAP::ValueArgOptional, "", "all", cmd);

	TCLAP::SwitchArg               dumpAll("",  "dump-all-default", "Will dump all the configuration. This is equivalent to '--dump-default all'.", cmd);
	TCLAP::ValueArg<string>     dumpFormat("",  "dump-format",		"Select the format in which the dump-default will print. The default is 'file'. Possible values are: file, tex, doku, media.",
										   TCLAP::ValueArgOptional, "file", "file", cmd);

	TCLAP::SwitchArg           listModules("",  "list-modules", 	"Will print a list of available modules. This is useful if you want to combine with --dump-default "
										   							"to have specific documentation for a module.", cmd);

	TCLAP::SwitchArg   displayExperimental("",  "show-experimental","Use in conjunction with --dump-default: will dump the configuration for a module even if it is marked as experiemental.", cmd);

	/* Overriding values */
	TCLAP::ValueArg<string>  listOverrides("",  "list-overrides",	"List the configuration values that you can override. Useful in conjunction with --set. "
																	"Pass a module to specify the module for which to dump the available values. Use 'all' to get all possible overrides.",
										   TCLAP::ValueArgOptional, "", "module", cmd);

	TCLAP::MultiArg<string> overrideConfig("s", "set", 				"Allows to override the configuration file setting. Use --list-overrides to get a list of values that you can override.",
										   TCLAP::ValueArgOptional, "global/debug=true");

	TCLAP::MultiArg<string>  hostsOverride("",  "hosts",			"Overrides a host address by passing it. You can use this flag multiple times. "
																	"Also, you can remove an association by providing an empty value: '--hosts myhost='.",
										   TCLAP::ValueArgOptional, "host=ip");

	// clang-format on

	cmd.add(hostsOverride);
	cmd.add(overrideConfig);

	try {
		// Try parsing input
		cmd.parse(argc, argv);
		debug = useDebug.getValue();

	} catch (TCLAP::ArgException &e) {

		cerr << "Error parsing arguments: " << e.error() << " for arg " << e.argId() << endl;
		exit(EXIT_FAILURE);
	}

	if (overrideConfig.getValue().size() != 0) {
		auto values = overrideConfig.getValue();
		for (auto it = values.begin(); it != values.end(); ++it) {
			auto pair = *it;
			size_t eq = pair.find("=");
			if (eq != pair.npos) {
				oset[pair.substr(0, eq)] = pair.substr(eq + 1);
			}
		}
	}

	// in case we don't plan to launch flexisip, don't setup the logs.
	if (!dumpDefault.getValue().length() && !listOverrides.getValue().length() && !listModules && !dumpMibs &&
		!dumpAll) {
		ortp_init();
		flexisip::log::preinit(useSyslog.getValue(), useDebug.getValue());
	} else {
		flexisip::log::disableGlobally();
	}

	// Instanciate the Generic manager
	GenericManager *cfg = GenericManager::get();

	// list default config and exit
	std::string module = dumpDefault.getValue();
	if (dumpAll) {
		module = "all";
	}

	if (module.length() != 0) {
		int status = dump_config(root, module, displayExperimental, dumpFormat.getValue());
		return status;
	}

	// list all mibs and exit
	if (dumpMibs) {
		a = make_shared<Agent>(root);
		cout << MibDumper(GenericManager::get()->getRoot());
		return EXIT_SUCCESS;
	}

	// list modules and exit
	if (listModules) {
		list_modules();
		return EXIT_SUCCESS;
	}

	// list the overridable values and exit
	if (listOverrides.getValue().length() != 0) {
		a = make_shared<Agent>(root);
		list<string> allCompletions;
		allCompletions.push_back("nosnmp");

		string empty;
		string &filter = listOverrides.getValue();

		depthFirstSearch(empty, GenericManager::get()->getRoot(), allCompletions);

		for (auto it = allCompletions.cbegin(); it != allCompletions.cend(); ++it) {
			if (filter == "all") {
				cout << *it << "\n";
			} else if (0 == it->compare(0, filter.length(), filter)) {
				cout << *it << "\n";
			}
		}
		return EXIT_SUCCESS;
	}

	GenericManager::get()->setOverrideMap(oset);

	if (cfg->load(configFile.getValue().c_str()) == -1) {
		fprintf(stderr, "Flexisip version %s\n"
						"No configuration file found at %s.\nPlease specify a valid configuration file.\n"
						"A default flexisip.conf.sample configuration file should be installed in " CONFIG_DIR "\n"
						"Please edit it and restart flexisip when ready.\n"
						"Alternatively a default configuration sample file can be generated at any time using "
						"'--dump-default all' option.\n",
				versionString.c_str(), configFile.getValue().c_str());
		return -1;
	}

	if (!debug)
		debug = cfg->getGlobal()->get<ConfigBoolean>("debug")->read();

	bool dump_cores = cfg->getGlobal()->get<ConfigBoolean>("dump-corefiles")->read();

	// Initialize
	flexisip::log::initLogs(useSyslog, debug);
	//flexisip::log::updateFilter(cfg->getGlobal()->get<ConfigString>("log-filter")->read());

	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, flexisip_stop);
	signal(SIGINT, flexisip_stop);
	signal(SIGUSR1, flexisip_stat);

	if (dump_cores) {
		/*enable core dumps*/
		struct rlimit lm;
		lm.rlim_cur = RLIM_INFINITY;
		lm.rlim_max = RLIM_INFINITY;
		if (setrlimit(RLIMIT_CORE, &lm) == -1) {
			LOGE("Cannot enable core dump, setrlimit() failed: %s", strerror(errno));
		}
	}

	su_init();
	/*tell parser to support extra headers */
	sip_update_default_mclass(sip_extend_mclass(NULL));

	log_boolean_expression_evaluation(boolExprEval.getValue());
	log_boolean_expression_parsing(boolParseEval.getValue());

	if (hostsOverride.getValue().size() != 0) {
		auto hosts = hostsOverride.getValue();
		auto etcResolver = EtcHostsResolver::get();

		for (auto it = hosts.begin(); it != hosts.end(); ++it) {
			size_t pos = it->find("=");
			if (pos != it->npos) {
				etcResolver->setHost(it->substr(0, pos), it->substr(pos + 1));
			}
		}
	}

	su_log_redirect(NULL, sofiaLogHandler, NULL);
	if (useDebug) {
		su_log_set_level(NULL, 9);
	}
	/*
	 NEVER NEVER create pthreads before this point : threads do not survive the fork below !!!!!!!!!!
	*/
	bool monitorEnabled = cfg->getRoot()->get<GenericStruct>("monitor")->get<ConfigBoolean>("enabled")->read();
	if (daemonMode) {
		/*now that we have successfully loaded the config, there is nothing that can prevent us to start (normally).
		So we can detach.*/
		bool autoRespawn = cfg->getGlobal()->get<ConfigBoolean>("auto-respawn")->read();
		forkAndDetach(pidFile.getValue().c_str(), autoRespawn, monitorEnabled);
	} else if (pidFile.getValue().length() != 0) {
		// not daemon but we want a pidfile anyway
		LOGN("Pidfile is %s", pidFile.getValue().c_str())
		makePidFile(pidFile.getValue().c_str());
	}

	LOGN("Starting flexisip version %s (git %s)", VERSION, FLEXISIP_GIT_VERSION);
	GenericManager::get()->sendTrap("Flexisip starting");

	root = su_root_create(NULL);
	a = make_shared<Agent>(root);
	a->start(transportsArg.getValue());
	setOpenSSLThreadSafe();
#ifdef ENABLE_SNMP
	SnmpAgent lAgent(*a, *cfg, oset);
#endif

	ortp_init();

	if (!oset.empty())
		cfg->applyOverrides(true); // using default + overrides

	a->loadConfig(cfg);

	// Create cached test accounts for the Flexisip monitor if necessary
	if (monitorEnabled) {
		try {
			Monitor::createAccounts();
		} catch (const FlexisipException &e) {
			LOGE("Could not create test accounts for the monitor. %s", e.str().c_str());
		}
	}

	increase_fd_limit();

	if (daemonMode) {
		if (write(pipe_wdog_flexisip[1], "ok", 3) == -1) {
			LOGF("Failed to write starter pipe: %s", strerror(errno));
		}
		close(pipe_wdog_flexisip[1]);
	}

	if (cfg->getRoot()->get<GenericStruct>("stun-server")->get<ConfigBoolean>("enabled")->read()) {
		stun = new StunServer(cfg->getRoot()->get<GenericStruct>("stun-server")->get<ConfigInt>("port")->read());
		stun->start();
	}

#ifdef ENABLE_PRESENCE
	bool enableLongTermPresence = (cfg->getRoot()->get<GenericStruct>("presence-server")->get<ConfigBoolean>("long-term-enabled")->read());
	flexisip::PresenceServer presenceServer(configFile.getValue());
	flexisip::PresenceLongterm *presenceLongTerm = NULL;
	if (enableLongTermPresence) {
		presenceLongTerm = new flexisip::PresenceLongterm(presenceServer.getBelleSipMainLoop());
		presenceServer.addNewPresenceInfoListener(presenceLongTerm);
	}
	presenceServer.start();
#endif // ENABLE_PRESENCE

	if (trackAllocs)
		msg_set_callbacks(flexisip_msg_create, flexisip_msg_destroy);

	su_timer_t *timer = su_timer_create(su_root_task(root), 5000);
	su_timer_set_for_ever(timer, (su_timer_f)timerfunc, a.get());
	su_root_run(root);
	su_timer_destroy(timer);
	a.reset();
	if (stun) {
		stun->stop();
		delete stun;
	}
	su_root_destroy(root);
#ifdef ENABLE_PRESENCE
	if (enableLongTermPresence) {
		presenceServer.removeNewPresenceInfoListener(presenceLongTerm);
		delete presenceLongTerm;
	}
#endif // ENABLE_PRESENCE

	LOGN("Flexisip exiting normally.");
	if (trackAllocs)
		dump_remaining_msgs();
	GenericManager::get()->sendTrap("Flexisip exiting normally");
	return 0;
}
