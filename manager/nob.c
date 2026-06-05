#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX
#include "./thirdparty/nob.h"

#define BUILD_FOLDER "bin/"
#define SRC_FOLDER   "src/"

static char* run_pkg_config(const char* flag, const char* pkgs) {
	char cmdline[256];
	snprintf(cmdline, sizeof(cmdline), "pkg-config %s %s 2>/dev/null", flag, pkgs);
	FILE* p = popen(cmdline, "r");
	if (!p) return NULL;
	char buf[1024]; size_t n = fread(buf, 1, sizeof(buf) - 1, p);
	int rc = pclose(p);
	if (rc != 0 || n == 0) return NULL;
	while (n && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) n--;
	buf[n] = '\0';
	return strdup(buf);
}

int main(int argc, char** argv) {
	NOB_GO_REBUILD_URSELF(argc, argv);
	if (!mkdir_if_not_exists(BUILD_FOLDER)) return 1;

	// 1.x names (upstream), 2.x names (Arch/Manjaro mongo-c-driver package)
	const char* mongo_pkgs = "libmongoc-1.0 libbson-1.0";
	char* mongo_cflags = run_pkg_config("--cflags", mongo_pkgs);
	if (!mongo_cflags) {
		mongo_pkgs = "mongoc2 bson2";
		mongo_cflags = run_pkg_config("--cflags", mongo_pkgs);
	}
	if (!mongo_cflags) {
		nob_log(ERROR, "pkg-config libmongoc-1.0/libbson-1.0 not found; install mongo-c-driver");
		return 1;
	}
	char* mongo_libs = run_pkg_config("--libs", mongo_pkgs);

	Cmd cmd = {0};
	cmd_append(&cmd, "gcc",
		"-Wall", "-Wextra", "-O2", "-pthread",
		"-o", BUILD_FOLDER"manager", SRC_FOLDER"main.c");

	for (char* tok = strtok(mongo_cflags, " \t"); tok; tok = strtok(NULL, " \t"))
		cmd_append(&cmd, tok);

	cmd_append(&cmd, "-lrabbitmq");
	if (mongo_libs) {
		for (char* tok = strtok(mongo_libs, " \t"); tok; tok = strtok(NULL, " \t"))
			cmd_append(&cmd, tok);
	} else {
		cmd_append(&cmd, "-lmongoc-1.0", "-lbson-1.0");
	}

	int ok = cmd_run(&cmd);
	free(mongo_cflags);
	free(mongo_libs);
	return ok ? 0 : 1;
}
