#include "posix.h"

#include <limits.h>
#include <string.h>

#include "buf_size.h"
#include "compilers.h"
#include "external/samu.h"
#include "filesystem.h"
#include "functions/default/options.h"
#include "log.h"
#include "output.h"
#include "path.h"
#include "tests.h"
#include "workspace.h"

const struct outpath outpath = {
	.private_dir = "muon-private",
	.setup = "setup.meson",
	.tests = "tests",
};

struct concat_strings_ctx {
	uint32_t *res;
};

struct output {
	FILE *build_ninja,
	     *tests,
	     *opts;
	bool compile_commands_comma;
};

static bool
concat_str(struct workspace *wk, uint32_t *dest, const char *s)
{
	if (strlen(s) >= BUF_SIZE_2k) {
		LOG_E("string too long in concat strings: '%s'", s);
		return false;
	}

	static char buf[BUF_SIZE_2k + 2] = { 0 };
	uint32_t i = 0;
	bool quote = false;

	for (; *s; ++s) {
		if (*s == ' ') {
			quote = true;
			buf[i] = '$';
			++i;
		} else if (*s == '"') {
			quote = true;
		}

		buf[i] = *s;
		++i;
	}

	buf[i] = 0;
	++i;

	if (quote) {
		wk_str_app(wk, dest, "'");
	}

	wk_str_app(wk, dest, buf);

	if (quote) {
		wk_str_app(wk, dest, "'");
	}

	wk_str_app(wk, dest, " ");
	return true;
}

static bool
tgt_build_dir(char buf[PATH_MAX], struct workspace *wk, struct obj *tgt)
{
	if (!path_relative_to(buf, PATH_MAX, wk->build_root, wk_str(wk, tgt->dat.tgt.build_dir))) {
		return false;
	}

	return true;
}

static bool
tgt_build_path(char buf[PATH_MAX], struct workspace *wk, struct obj *tgt)
{
	char tmp[PATH_MAX] = { 0 };
	if (!path_join(tmp, PATH_MAX, wk_str(wk, tgt->dat.tgt.build_dir), wk_str(wk, tgt->dat.tgt.build_name))) {
		return false;
	} else if (!path_relative_to(buf, PATH_MAX, wk->build_root, tmp)) {
		return false;
	}

	return true;
}

static bool
strobj(struct workspace *wk, uint32_t *dest, uint32_t src)
{
	struct obj *obj = get_obj(wk, src);

	switch (obj->type) {
	case obj_string:
		*dest = obj->dat.str;
		return true;
	case obj_file:
		*dest = obj->dat.file;
		return true;

	case obj_build_target: {
		char tmp1[PATH_MAX], path[PATH_MAX];
		if (!tgt_build_path(tmp1, wk, obj)) {
			return false;
		} else if (!path_executable(path, PATH_MAX, tmp1)) {
			return false;
		}

		*dest = wk_str_push(wk, path);
		return true;
	}
	default:
		LOG_E("cannot convert '%s' to string", obj_type_to_s(obj->type));
		return false;
	}
}

static bool
concat_strobj(struct workspace *wk, uint32_t *dest, uint32_t src)
{
	uint32_t str;
	if (!strobj(wk, &str, src)) {
		return false;
	}

	return concat_str(wk, dest, wk_str(wk, str));
}

static enum iteration_result
concat_strings_iter(struct workspace *wk, void *_ctx, uint32_t val)
{
	struct concat_strings_ctx *ctx = _ctx;
	if (!concat_strobj(wk, ctx->res, val)) {
		return ir_err;
	}

	return ir_cont;
}

static bool
concat_strings(struct workspace *wk, uint32_t arr, uint32_t *res)
{
	if (!*res) {
		*res = wk_str_push(wk, "");
	}

	struct concat_strings_ctx ctx = {
		.res = res,
	};

	return obj_array_foreach(wk, arr, &ctx, concat_strings_iter);
}

static const char *
get_dict_str(struct workspace *wk, uint32_t dict, const char *k, const char *fallback)
{
	bool found;
	uint32_t res;

	if (!obj_dict_index_strn(wk, dict, k, strlen(k), &res, &found)) {
		return fallback;
	} else if (!found) {
		return fallback;
	} else {
		return wk_objstr(wk, res);
	}
}

static enum iteration_result
write_compiler_rule_iter(struct workspace *wk, void *_ctx, uint32_t lang, uint32_t comp_id)
{
	FILE *out = _ctx;
	struct obj *comp = get_obj(wk, comp_id);
	assert(comp->type == obj_compiler);

	enum compiler_type t = comp->dat.compiler.type;

	fprintf(out, "rule %s_COMPILER\n"
		" command = %s %s\n"
		" deps = %s\n"
		" depfile = %s\n"
		" description = %s\n",
		wk_objstr(wk, lang),
		wk_str(wk, comp->dat.compiler.name),
		compilers[t].command,
		compilers[t].deps,
		compilers[t].depfile,
		compilers[t].description);

	return ir_cont;
}

static void
write_hdr(FILE *out, struct workspace *wk, struct project *main_proj)
{
	uint32_t sep, sources;
	make_obj(wk, &sep, obj_string)->dat.str = wk_str_push(wk, " ");
	obj_array_join(wk, wk->sources, sep, &sources);

	fprintf(
		out,
		"# This is the build file for project \"%s\"\n"
		"# It is autogenerated by the muon build system.\n"
		"\n"
		"ninja_required_version = 1.7.1\n"
		"\n"
		"# Rules for compiling.\n"
		"\n",
		wk_str(wk, main_proj->cfg.name)
		);

	obj_dict_foreach(wk, main_proj->compilers, out, write_compiler_rule_iter);

	fprintf(out,
		"# Rules for linking.\n"
		"\n"
		"rule STATIC_LINKER\n"
		" command = rm -f $out && %s $LINK_ARGS $out $in\n"
		" description = Linking static target $out\n"
		"\n"
		"rule c_LINKER\n"
		" command = %s $ARGS -o $out $in $LINK_ARGS\n"
		" description = Linking target $out\n"
		"\n"
		"# Other rules\n"
		"\n"
		"rule CUSTOM_COMMAND\n"
		" command = $COMMAND\n"
		" description = $DESCRIPTION\n"
		" restat = 1\n"
		"\n"
		"rule REGENERATE_BUILD\n"
		" command = %s build -r -c %s%c%s\n"
		" description = Regenerating build files.\n"
		" generator = 1\n"
		"\n"
		"build build.ninja: REGENERATE_BUILD %s\n"
		" pool = console\n"
		"\n"
		"# targets\n\n",
		get_dict_str(wk, wk->binaries, "ar", "ar"),
		get_dict_str(wk, wk->binaries, "c_ld", "cc"),
		wk->argv0,
		outpath.private_dir, PATH_SEP, outpath.setup,
		wk_objstr(wk, sources)
		);
}

struct write_tgt_iter_ctx {
	char *tgt_parts_dir;
	struct obj *tgt;
	struct output *output;
	uint32_t args_id;
	uint32_t object_names_id;
	uint32_t order_deps_id;
	bool have_order_deps;
	uint32_t link_args_id;
	uint32_t implicit_deps_id;
	bool have_implicit_deps;
};

static bool
suffixed_by(const char *str, const char *suffix)
{
	uint32_t len = strlen(str), sufflen = strlen(suffix);
	if (sufflen > len) {
		return false;
	}

	return strcmp(&str[len - sufflen], suffix) == 0;
}

static enum iteration_result
write_tgt_sources_iter(struct workspace *wk, void *_ctx, uint32_t val_id)
{
	struct write_tgt_iter_ctx *ctx = _ctx;
	struct obj *src = get_obj(wk, val_id);
	assert(src->type == obj_file);

	if (suffixed_by(wk_str(wk, src->dat.file), ".h")) {
		return ir_cont;
	}

	char src_path[PATH_MAX];
	if (!path_relative_to(src_path, PATH_MAX, wk->build_root, wk_str(wk, src->dat.file))) {
		return ir_err;
	}

	char rel[PATH_MAX], dest_path[PATH_MAX];
	const char *base;

	if (path_is_subpath(wk_str(wk, ctx->tgt->dat.tgt.build_dir),
		wk_str(wk, src->dat.file))) {
		base = wk_str(wk, ctx->tgt->dat.tgt.build_dir);
	} else if (path_is_subpath(wk_str(wk, ctx->tgt->dat.tgt.cwd),
		wk_str(wk, src->dat.file))) {
		base = wk_str(wk, ctx->tgt->dat.tgt.cwd);
	} else {
		base = wk->source_root;
	}

	if (!path_relative_to(rel, PATH_MAX, base, wk_str(wk, src->dat.file))) {
		return ir_err;
	} else if (!path_join(dest_path, PATH_MAX, ctx->tgt_parts_dir, rel)) {
		return ir_err;
	} else if (!path_add_suffix(dest_path, PATH_MAX, ".o")) {
		return ir_err;
	}

	wk_str_appf(wk, &ctx->object_names_id, "%s ", dest_path);

	/* TODO: add order_deps here */
	fprintf(ctx->output->build_ninja,
		"build %s: c_COMPILER %s %s\n"
		" DEPFILE = %s.d\n"
		" DEPFILE_UNQUOTED = %s.d\n"
		" ARGS = %s\n\n",
		dest_path, src_path,
		ctx->have_order_deps ? wk_str(wk, ctx->order_deps_id) : "",
		dest_path,
		dest_path,
		wk_str(wk, ctx->args_id)
		);

	return ir_cont;
}

static enum iteration_result
process_source_includes_iter(struct workspace *wk, void *_ctx, uint32_t val_id)
{
	struct write_tgt_iter_ctx *ctx = _ctx;
	struct obj *src = get_obj(wk, val_id);
	assert(src->type == obj_file);

	if (!suffixed_by(wk_str(wk, src->dat.file), ".h")) {
		return ir_cont;
	}

	char dir[PATH_MAX], path[PATH_MAX];

	if (!path_relative_to(path, PATH_MAX, wk->build_root, wk_str(wk, src->dat.file))) {
		return ir_err;
	}

	wk_str_appf(wk, &ctx->order_deps_id, "%s ", path);
	ctx->have_order_deps = true;

	if (!path_dirname(dir, PATH_MAX, path)) {
		return ir_err;
	}

	wk_str_appf(wk, &ctx->args_id, "-I%s ", dir);

	return ir_cont;
}

static enum iteration_result
process_dep_args_includes_iter(struct workspace *wk, void *_ctx, uint32_t inc_id)
{
	struct write_tgt_iter_ctx *ctx = _ctx;

	assert(get_obj(wk, inc_id)->type == obj_file);
	wk_str_appf(wk, &ctx->args_id, "-I%s ", wk_file_path(wk, inc_id));
	return ir_cont;
}

static enum iteration_result
process_dep_args_iter(struct workspace *wk, void *_ctx, uint32_t val_id)
{
	struct obj *dep = get_obj(wk, val_id);

	if (dep->dat.dep.include_directories) {
		struct obj *inc = get_obj(wk, dep->dat.dep.include_directories);
		assert(inc->type == obj_array);
		if (!obj_array_foreach_flat(wk, dep->dat.dep.include_directories,
			_ctx, process_dep_args_includes_iter)) {
			return ir_err;
		}
	}

	return ir_cont;
}

static enum iteration_result process_dep_links_iter(struct workspace *wk, void *_ctx, uint32_t val_id);

static enum iteration_result
process_link_with_iter(struct workspace *wk, void *_ctx, uint32_t val_id)
{
	struct write_tgt_iter_ctx *ctx = _ctx;

	struct obj *tgt = get_obj(wk, val_id);

	switch (tgt->type) {
	case  obj_build_target: {
		char path[PATH_MAX];

		if (!tgt_build_path(path, wk, tgt)) {
			return ir_err;
		} else if (!path_add_suffix(path, PATH_MAX, " ")) {
			return ir_err;
		}

		if (ctx->tgt->dat.tgt.type == tgt_executable) {
			wk_str_app(wk, &ctx->implicit_deps_id, path);
			wk_str_app(wk, &ctx->link_args_id, path);
			ctx->have_implicit_deps = true;
		}

		if (!tgt_build_dir(path, wk, tgt)) {
			return ir_err;
		}

		wk_str_appf(wk, &ctx->args_id, "-I%s ", path);

		if (tgt->dat.tgt.deps) {
			if (!obj_array_foreach(wk, tgt->dat.tgt.deps, ctx, process_dep_links_iter)) {
				return ir_err;
			}
		}
		break;
	}
	case obj_string:
		if (ctx->tgt->dat.tgt.type == tgt_executable) {
			wk_str_appf(wk, &ctx->link_args_id, "%s ", wk_str(wk, tgt->dat.str));
		}
		break;
	default:
		LOG_E("invalid type for link_with: '%s'", obj_type_to_s(tgt->type));
		return ir_err;
	}

	return ir_cont;
}

static enum iteration_result
process_dep_links_iter(struct workspace *wk, void *_ctx, uint32_t val_id)
{
	struct obj *dep = get_obj(wk, val_id);

	if (dep->dat.dep.link_with) {
		if (!obj_array_foreach(wk, dep->dat.dep.link_with, _ctx, process_link_with_iter)) {
			return ir_err;
		}
	}

	return ir_cont;
}

static enum iteration_result
process_include_dirs_iter(struct workspace *wk, void *_ctx, uint32_t val_id)
{
	uint32_t *args_id = _ctx;
	struct obj *inc = get_obj(wk, val_id);
	assert(inc->type == obj_file);

	wk_str_appf(wk, args_id, "-I%s ", wk_str(wk, inc->dat.file));
	return ir_cont;
}

struct write_tgt_ctx {
	struct output *output;
	struct project *proj;
};

static const char *
get_optimization_flag(struct workspace *wk, struct project *proj)
{
	uint32_t i;
	static const char *lvls = "0g123";
	static char custom[] = "-OX\0XX";
	static struct {
		const char *name, *flag;
	} tbl[] = {
		{ "plain", "-O0" },
		{ "debug", "-g" },
		{ "debugoptimized", "-g -Og" },
		{ "release", "-O3" },
		{ "minsize", "-Os" },
		{ "custom", NULL },
		{ NULL }
	};

	uint32_t buildtype, optimization, debug;

	if (!get_option(wk, proj, "buildtype", &buildtype)) {
		assert(false);
	} else if (!get_option(wk, proj, "optimization", &optimization)) {
		assert(false);
	} else if (!get_option(wk, proj, "debug", &debug)) {
		assert(false);
	}

	const char *str = wk_objstr(wk, buildtype);

	for (i = 0; tbl[i].name; ++i) {
		if (strcmp(str, tbl[i].name) == 0) {
			if (tbl[i].flag) {
				return tbl[i].flag;
			} else {
				str = wk_objstr(wk, optimization);

				if (strlen(str) != 1 || !strchr(lvls, *str)) {
					LOG_E("invalid optimization: '%s'", str);
					return NULL;
				}

				custom[2] = *str;

				if (get_obj(wk, debug)->dat.boolean) {
					custom[3] = ' ';
					custom[4] = '-';
					custom[5] = 'g';
				}

				return custom;
			}
		}
	}

	LOG_E("invalid build type: '%s'", wk_objstr(wk, buildtype));
	return NULL;
}

static const char *
get_warning_flag(struct workspace *wk, struct project *proj)
{
	uint32_t lvl;

	if (!get_option(wk, proj, "warning_level", &lvl)) {
		assert(false);
	}

	assert(get_obj(wk, lvl)->type == obj_number);

	switch (get_obj(wk, lvl)->dat.num) {
	case 0:
		return "";
	case 1:
		return "-Wall";
	case 2:
		return "-Wall -Wextra";
	case 3:
		return "-Wall -Wextra -Wpedantic";
	default:
		assert(false);
		return NULL;
	}
}

static const char *
get_std_flag(struct workspace *wk, struct project *proj)
{
	uint32_t std;

	if (!get_option(wk, proj, "c_std", &std)) {
		assert(false);
	}

	static char buf[BUF_SIZE_2k + 1] = { 0 };
	const char *s = wk_objstr(wk, std);

	if (strcmp(s, "none") == 0) {
		return "";
	} else {
		snprintf(buf, BUF_SIZE_2k, "-std=%s", s);
		return buf;
	}
}

static enum iteration_result
write_build_tgt(struct workspace *wk, void *_ctx, uint32_t tgt_id)
{
	struct output *output = ((struct write_tgt_ctx *)_ctx)->output;
	struct project *proj = ((struct write_tgt_ctx *)_ctx)->proj;

	struct obj *tgt = get_obj(wk, tgt_id);
	LOG_I("writing rules for target '%s'", wk_str(wk, tgt->dat.tgt.build_name));

	char path[PATH_MAX];
	if (!tgt_build_path(path, wk, tgt)) {
		return ir_err;
	} else if (!path_add_suffix(path, PATH_MAX, ".p")) {
		return ir_err;
	}

	struct write_tgt_iter_ctx ctx = {
		.tgt = tgt,
		.output = output,
		.tgt_parts_dir = path,
		.implicit_deps_id = wk_str_push(wk, "| "),
		.order_deps_id = wk_str_push(wk, "|| "),
	};

	const char *rule;
	switch (tgt->dat.tgt.type) {
	case tgt_executable:
		rule = "c_LINKER";
		ctx.link_args_id = wk_str_push(wk, "-Wl,--as-needed -Wl,--no-undefined -Wl,--start-group ");

		break;
	case tgt_library:
		rule = "STATIC_LINKER";
		ctx.link_args_id = wk_str_push(wk, "csrD");
		break;
	default:
		assert(false);
		return ir_err;
	}

	const char *opt_flag;
	if (!(opt_flag = get_optimization_flag(wk, proj))) {
		LOG_E("unable to get optimization flags");
		return ir_err;
	}

	const char *warning_flag;
	if (!(warning_flag = get_warning_flag(wk, proj))) {
		LOG_E("unable to get warning flags");
		return ir_err;
	}

	const char *c_std;
	if (!(c_std = get_std_flag(wk, proj))) {
		LOG_E("unable to get std flag");
		return ir_err;
	}

	/* default arguments */
	ctx.args_id = wk_str_pushf(wk, "%s %s %s -I%s ", c_std, opt_flag, warning_flag, wk_str(wk, proj->cwd));

	{ /* includes */

		if (tgt->dat.tgt.include_directories) {
			struct obj *inc = get_obj(wk, tgt->dat.tgt.include_directories);
			assert(inc->type == obj_array);
			if (!obj_array_foreach_flat(wk, tgt->dat.tgt.include_directories, &ctx.args_id, process_include_dirs_iter)) {
				return ir_err;
			}
		}

		{ /* dep includes */
			if (tgt->dat.tgt.deps) {
				if (!obj_array_foreach(wk, tgt->dat.tgt.deps, &ctx, process_dep_args_iter)) {
					return ir_err;
				}
			}
		}

		/* sources includes */
		if (!obj_array_foreach(wk, tgt->dat.tgt.src, &ctx, process_source_includes_iter)) {
			return ir_err;
		}
	}

	{ /* dependencies / link_with */
		if (tgt->dat.tgt.deps) {
			if (!obj_array_foreach(wk, tgt->dat.tgt.deps, &ctx, process_dep_links_iter)) {
				return ir_err;
			}
		}

		if (tgt->dat.tgt.link_with) {
			if (!obj_array_foreach(wk, tgt->dat.tgt.link_with, &ctx, process_link_with_iter)) {
				return ir_err;
			}
		}
	}

	{ /* trailing args */
		if (!concat_strings(wk, proj->cfg.args, &ctx.args_id)) {
			return ir_err;
		}

		if (tgt->dat.tgt.c_args) {
			if (!concat_strings(wk, tgt->dat.tgt.c_args, &ctx.args_id)) {
				return ir_err;
			}
		}
	}

	{ /* sources */
		ctx.object_names_id = wk_str_push(wk, "");
		if (!obj_array_foreach(wk, tgt->dat.tgt.src, &ctx, write_tgt_sources_iter)) {
			return ir_err;
		}
	}

	if (tgt->dat.tgt.type == tgt_executable) {
		wk_str_app(wk, &ctx.link_args_id, " -Wl,--end-group");
	}

	if (!tgt_build_path(path, wk, tgt)) {
		return ir_err;
	}

	fprintf(output->build_ninja, "build %s: %s %s %s %s"
		"\n LINK_ARGS = %s\n\n",
		path,
		rule,
		wk_str(wk, ctx.object_names_id),
		ctx.have_implicit_deps ? wk_str(wk, ctx.implicit_deps_id) : "",
		ctx.have_order_deps ? wk_str(wk, ctx.order_deps_id) : "",

		wk_str(wk, ctx.link_args_id)
		);

	return ir_cont;
}

static enum iteration_result
custom_tgt_outputs_iter(struct workspace *wk, void *_ctx, uint32_t val_id)
{
	uint32_t *dest = _ctx;

	struct obj *out = get_obj(wk, val_id);
	assert(out->type == obj_file);

	char buf[PATH_MAX];

	if (!path_relative_to(buf, PATH_MAX, wk->build_root, wk_str(wk, out->dat.file))) {
		return ir_err;
	}

	return concat_str(wk, dest, buf) == true ? ir_cont : ir_err;
}

static enum iteration_result
write_custom_tgt(struct workspace *wk, void *_ctx, uint32_t tgt_id)
{
	struct output *output = ((struct write_tgt_ctx *)_ctx)->output;

	struct obj *tgt = get_obj(wk, tgt_id);
	LOG_I("writing rules for custom target '%s'", wk_str(wk, tgt->dat.custom_target.name));

	uint32_t outputs, inputs = 0, cmdline_pre, cmdline = 0;

	if (!concat_strings(wk, tgt->dat.custom_target.input, &inputs)) {
		return ir_err;
	}

	outputs = wk_str_push(wk, "");
	if (!obj_array_foreach(wk, tgt->dat.custom_target.output, &outputs, custom_tgt_outputs_iter)) {
		return ir_err;
	}

	if (tgt->dat.custom_target.flags & custom_target_capture) {
		cmdline_pre = wk_str_pushf(wk, "%s internal exe ", wk->argv0);

		wk_str_app(wk, &cmdline_pre, "-c ");

		uint32_t elem;
		if (!obj_array_index(wk, tgt->dat.custom_target.output, 0, &elem)) {
			return ir_err;
		}

		if (custom_tgt_outputs_iter(wk, &cmdline_pre, elem) == ir_err) {
			return ir_err;
		}

		wk_str_app(wk, &cmdline_pre, "--");
	} else {
		cmdline_pre = wk_str_push(wk, "");
	}

	if (!concat_strings(wk, tgt->dat.custom_target.args, &cmdline)) {
		return ir_err;
	}

	fprintf(output->build_ninja, "build %s: CUSTOM_COMMAND %s | %s\n"
		" COMMAND = %s %s\n"
		" DESCRIPTION = %s%s\n\n",
		wk_str(wk, outputs),
		wk_str(wk, inputs),
		wk_objstr(wk, tgt->dat.custom_target.cmd),

		wk_str(wk, cmdline_pre),
		wk_str(wk, cmdline),
		wk_str(wk, cmdline),
		tgt->dat.custom_target.flags & custom_target_capture ? "(captured)": ""
		);

	return ir_cont;
}

static enum iteration_result
write_tgt_iter(struct workspace *wk, void *_ctx, uint32_t tgt_id)
{
	switch (get_obj(wk, tgt_id)->type) {
	case obj_build_target:
		return write_build_tgt(wk, _ctx, tgt_id);
	case obj_custom_target:
		return write_custom_tgt(wk, _ctx, tgt_id);
	default:
		LOG_E("invalid tgt type '%s'", obj_type_to_s(get_obj(wk, tgt_id)->type));
		return ir_err;
	}
}

static enum iteration_result
write_test_args_iter(struct workspace *wk, void *_ctx, uint32_t arg)
{
	struct write_tgt_ctx *ctx = _ctx;
	fputc(0, ctx->output->tests);

	uint32_t str;
	if (!strobj(wk, &str, arg)) {
		return ir_err;
	}

	fputs(wk_str(wk, str), ctx->output->tests);
	return ir_cont;
}

static enum iteration_result
write_test_iter(struct workspace *wk, void *_ctx, uint32_t test)
{
	struct write_tgt_ctx *ctx = _ctx;
	struct obj *t = get_obj(wk, test);

	uint32_t test_flags = 0;

	if (t->dat.test.should_fail) {
		test_flags |= test_flag_should_fail;
	}

	fs_fwrite(&test_flags, sizeof(uint32_t), ctx->output->tests);

	fputs(wk_objstr(wk, t->dat.test.name), ctx->output->tests);
	fputc(0, ctx->output->tests);
	fputs(wk_objstr(wk, t->dat.test.exe), ctx->output->tests);

	if (t->dat.test.args) {
		if (!obj_array_foreach_flat(wk, t->dat.test.args, ctx, write_test_args_iter)) {
			LOG_E("failed to write test '%s'", wk_objstr(wk, t->dat.test.name));
			return ir_err;
		}
	}
	fputc(0, ctx->output->tests);
	fputc(0, ctx->output->tests);

	return ir_cont;
}

static bool
write_project(struct output *output, struct workspace *wk, struct project *proj)
{
	struct write_tgt_ctx ctx = { .output = output, .proj = proj };

	if (!obj_array_foreach(wk, proj->targets, &ctx, write_tgt_iter)) {
		return false;
	}

	LOG_I("writing tests");

	if (!obj_array_foreach(wk, proj->tests, &ctx, write_test_iter)) {
		return false;
	}

	return true;
}

static bool
write_opts(FILE *f, struct workspace *wk)
{
	struct project *proj;
	uint32_t i;
	char buf[2048];
	uint32_t opts;
	proj = darr_get(&wk->projects, 0);

	if (!obj_dict_dup(wk, proj->opts, &opts)) {
		return false;
	}

	for (i = 1; i < wk->projects.len; ++i) {
		proj = darr_get(&wk->projects, i);
		uint32_t str;
		make_obj(wk, &str, obj_string)->dat.str = proj->subproject_name;
		obj_dict_set(wk, opts, str, proj->opts);
	}

	if (!obj_to_s(wk, opts, buf, 2048)) {
		return false;
	}

	fprintf(f, "setup(\n\t'%s',\n\tsource: '%s',\n\toptions: %s\n)\n", wk->build_root, wk->source_root, buf);
	return true;
}

static FILE *
open_out(const char *dir, const char *name)
{
	char path[PATH_MAX];
	if (!path_join(path, PATH_MAX, dir, name)) {
		return NULL;
	}

	return fs_fopen(path, "w");
}

bool
output_private_file(struct workspace *wk, char dest[PATH_MAX],
	const char *path, const char *txt)
{
	if (!path_join(dest, PATH_MAX, wk->muon_private, path)) {
		return false;
	}

	return fs_write(dest, (uint8_t *)txt, strlen(txt));
}

bool
output_build(struct workspace *wk)
{
	struct output output = { 0 };

	if (!(output.build_ninja = open_out(wk->build_root, "build.ninja"))) {
		return false;
	} else if (!(output.tests = open_out(wk->muon_private, outpath.tests))) {
		return false;
	} else if (!(output.opts = open_out(wk->muon_private, outpath.setup))) {
		return false;
	}

	write_hdr(output.build_ninja, wk, darr_get(&wk->projects, 0));
	write_opts(output.opts, wk);

	uint32_t i;
	for (i = 0; i < wk->projects.len; ++i) {
		if (!write_project(&output, wk, darr_get(&wk->projects, i))) {
			return false;
		}
	}

	if (!fs_fclose(output.build_ninja)) {
		return false;
	} else if (!fs_fclose(output.tests)) {
		return false;
	} else if (!fs_fclose(output.opts)) {
		return false;
	}

	if (have_samu) {
		char compile_commands[PATH_MAX];
		if (!path_join(compile_commands, PATH_MAX, wk->build_root, "compile_commands.json")) {
			return false;
		}

		if (!muon_samu_compdb(wk->build_root, compile_commands)) {
			return false;
		}
	}

	return true;
}
