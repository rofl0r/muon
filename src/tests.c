#include "posix.h"

#include <limits.h>
#include <string.h>

#include "args.h"
#include "buf_size.h"
#include "lang/serial.h"
#include "log.h"
#include "output/output.h"
#include "platform/filesystem.h"
#include "platform/mem.h"
#include "platform/path.h"
#include "platform/run_cmd.h"
#include "tests.h"

static enum iteration_result
run_test(struct workspace *wk, void *_ctx, uint32_t t)
{
	struct obj *test = get_obj(wk, t);

	uint32_t cmdline;
	make_obj(wk, &cmdline, obj_array);
	obj_array_push(wk, cmdline, test->dat.test.exe);

	if (test->dat.test.args) {
		uint32_t test_args;
		if (!arr_to_args(wk, test->dat.test.args, &test_args)) {
			return ir_err;
		}

		obj_array_extend(wk, cmdline, test_args);
	}

	char *argv[MAX_ARGS], *envp[MAX_ARGS] = { 0 };

	if (!join_args_argv(wk, argv, MAX_ARGS, cmdline)) {
		LOG_E("failed to prepare arguments");
		return ir_err;
	}

	if (test->dat.test.env) {
		if (!join_args_argv(wk, envp, MAX_ARGS, test->dat.test.env)) {
			LOG_E("failed to prepare environment");
			return ir_err;
		}
	}

	enum iteration_result ret = ir_err;
	struct run_cmd_ctx cmd_ctx = { 0 };

	if (!run_cmd(&cmd_ctx, wk_objstr(wk, test->dat.test.exe), argv, envp)) {
		LOG_E("test command failed: %s", cmd_ctx.err_msg);
		goto ret;
	}

	if (cmd_ctx.status && !test->dat.test.should_fail) {
		LOG_E("%s - failed (%d)", wk_objstr(wk, test->dat.test.name), cmd_ctx.status);
		log_plain("%s", cmd_ctx.err);
		goto ret;
	} else {
		LOG_I("%s - success (%d)", wk_objstr(wk, test->dat.test.name), cmd_ctx.status);
	}

	ret = ir_cont;
ret:
	run_cmd_ctx_destroy(&cmd_ctx);
	return ret;
}

static enum iteration_result
run_project_tests(struct workspace *wk, void *_ctx, uint32_t proj_name, uint32_t tests)
{
	LOG_I("running tests for project '%s'", wk_objstr(wk, proj_name));

	if (!obj_array_foreach(wk, tests, NULL, run_test)) {
		return ir_err;
	}

	return ir_cont;
}

bool
tests_run(const char *build_dir)
{
	bool ret = false;
	char tests_src[PATH_MAX], private[PATH_MAX], build_root[PATH_MAX];

	if (!path_make_absolute(build_root, PATH_MAX, build_dir)) {
		return false;
	} else if (!path_join(private, PATH_MAX, build_root, outpath.private_dir)) {
		return false;
	} else if (!path_join(tests_src, PATH_MAX, private, outpath.tests)) {
		return false;
	}

	FILE *f;
	if (!(f = fs_fopen(tests_src, "r"))) {
		return false;
	}

	struct workspace wk;
	workspace_init_bare(&wk);

	uint32_t tests_dict;
	if (!serial_load(&wk, &tests_dict, f)) {
		LOG_E("invalid tests file");
		goto ret;
	} else if (!fs_fclose(f)) {
		goto ret;
	} else if (!path_chdir(build_root)) {
		goto ret;
	} else if (!obj_dict_foreach(&wk, tests_dict, NULL, run_project_tests)) {
		goto ret;
	}

	ret = true;
ret:
	workspace_destroy_bare(&wk);
	return ret;
}
