#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX

#include "./thirdparty/nob.h"

#define BUILD_FOLDER "bin/"
#define SRC_FOLDER   "src/"

#define EXIT_FAILURE 1
#define EXIT_SUCCESS 0

int main(int argc, char** argv) {
	NOB_GO_REBUILD_URSELF(argc, argv);
	if (!mkdir_if_not_exists(BUILD_FOLDER)) {
		return EXIT_FAILURE;
	}

	Cmd cmd = {0};

	cmd_append(&cmd, "cc", "-Wall", "-Wextra", "-O3", "-o", BUILD_FOLDER"worker", SRC_FOLDER"main.c");
	if (!cmd_run(&cmd)) {
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
