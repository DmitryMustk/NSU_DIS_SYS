#include <stdlib.h>
#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX

#include "./thirdparty/nob.h"

#define BUILD_FOLDER   "bin/"
#define INCLUDE_FOLDER "include/"
#define SRC_FOLDER     "src/"

#define EXIT_FAILURE 1
#define EXIT_SUCCESS 0

typedef struct {
	char** items;
	size_t count;
	size_t capacity;
} GeneratedFiles ;

int main(int argc, char** argv) {
	NOB_GO_REBUILD_URSELF(argc, argv);
	if (!mkdir_if_not_exists(BUILD_FOLDER) || !mkdir_if_not_exists(INCLUDE_FOLDER)) {
		return EXIT_FAILURE;
	}

	Cmd cmd = {0};
	
	GeneratedFiles files = {0};

	da_append(&files, "./include/instpow.h");
	da_append(&files, "./include/dto.h");

	da_foreach(char*, file, &files) {
		if (needs_rebuild1(*file, "./generate.py")) {
			cmd_append(&cmd, "python3", "generate.py");
			if (!cmd_run(&cmd)) {
				nob_log(ERROR, "Can't rebuild %s but it is need to. Do it manualy!", *file);
				return EXIT_FAILURE;
			}
		}

	}
	


	cmd_append(&cmd, "cc", "-Wall", "-Wextra", "-O3", "-o", BUILD_FOLDER"worker", SRC_FOLDER"main.c");
	if (!cmd_run(&cmd)) {
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
