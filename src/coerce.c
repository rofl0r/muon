#include "posix.h"

#include <assert.h>
#include <string.h>

#include "coerce.h"
#include "lang/interpreter.h"
#include "platform/filesystem.h"
#include "platform/path.h"

bool
coerce_executable(struct workspace *wk, uint32_t node, uint32_t val, uint32_t *res)
{
	struct obj *obj;
	uint32_t str;

	switch ((obj = get_obj(wk, val))->type) {
	case obj_file:
		*res = val;
		return true;
	case obj_build_target: {
		char tmp1[PATH_MAX], dest[PATH_MAX];

		if (!path_join(dest, PATH_MAX, wk_str(wk, obj->dat.tgt.build_dir),
			wk_str(wk, obj->dat.tgt.build_name))) {
			return false;
		} else if (!path_relative_to(tmp1, PATH_MAX, wk->build_root, dest)) {
			return false;
		} else if (!path_executable(dest, PATH_MAX, tmp1)) {
			return false;
		}

		str = wk_str_push(wk, dest);
		break;
	}
	case obj_external_program:
		str = obj->dat.external_program.full_path;
		break;
	default:
		interp_error(wk, node, "unable to coerce '%s' into executable", obj_type_to_s(obj->type));
		return false;
	}

	make_obj(wk, res, obj_string)->dat.str = str;
	return true;
}

bool
coerce_requirement(struct workspace *wk, struct args_kw *kw_required, enum requirement_type *requirement)
{
	if (kw_required->set) {
		if (get_obj(wk, kw_required->val)->type == obj_bool) {
			if (get_obj(wk, kw_required->val)->dat.boolean) {
				*requirement = requirement_required;
			} else {
				*requirement = requirement_auto;
			}
		} else if (get_obj(wk, kw_required->val)->type == obj_feature_opt) {
			switch (get_obj(wk, kw_required->val)->dat.feature_opt.state) {
			case feature_opt_disabled:
				*requirement = requirement_skip;
				break;
			case feature_opt_enabled:
				*requirement = requirement_required;
				break;
			case feature_opt_auto:
				*requirement = requirement_auto;
				break;
			}
		} else {
			interp_error(wk, kw_required->node, "expected type %s or %s, got %s",
				obj_type_to_s(obj_bool),
				obj_type_to_s(obj_feature_opt),
				obj_type_to_s(get_obj(wk, kw_required->val)->type)
				);
			return false;
		}
	} else {
		*requirement = requirement_required;
	}

	return true;
}

typedef bool (*exists_func)(const char *);

enum coerce_into_files_mode {
	mode_input,
	mode_output,
};

struct coerce_into_files_ctx {
	uint32_t node;
	uint32_t arr;
	const char *type;
	exists_func exists;
	enum coerce_into_files_mode mode;
};

static enum iteration_result
coerce_custom_target_output_iter(struct workspace *wk, void *_ctx, uint32_t val)
{
	struct coerce_into_files_ctx *ctx = _ctx;
	assert(get_obj(wk, val)->type == obj_file);

	obj_array_push(wk, ctx->arr, val);
	return ir_cont;
}

static enum iteration_result
coerce_into_files_iter(struct workspace *wk, void *_ctx, uint32_t val)
{
	struct coerce_into_files_ctx *ctx = _ctx;

	switch (get_obj(wk, val)->type) {
	case obj_string: {
		uint32_t path;
		char buf[PATH_MAX];

		switch (ctx->mode) {
		case mode_input:
			if (path_is_absolute(wk_objstr(wk, val))) {
				path = get_obj(wk, val)->dat.str;
			} else {
				if (!path_join(buf, PATH_MAX, wk_str(wk, current_project(wk)->cwd), wk_objstr(wk, val))) {
					return ir_err;
				}

				path = wk_str_push(wk, buf);
			}

			if (!ctx->exists(wk_str(wk, path))) {
				interp_error(wk, ctx->node, "%s '%s' does not exist",
					ctx->type, wk_str(wk, path));
				return ir_err;
			}
			break;
		case mode_output:
			if (!path_is_basename(wk_objstr(wk, val))) {
				interp_error(wk, ctx->node, "output file '%s' contains path seperators", wk_objstr(wk, val));
				return ir_err;
			}

			if (!path_join(buf, PATH_MAX, wk_str(wk, current_project(wk)->build_dir), wk_objstr(wk, val))) {
				return ir_err;
			}

			path = wk_str_push(wk, buf);
			break;
		default:
			assert(false);
			return ir_err;
		}

		uint32_t file;
		make_obj(wk, &file, obj_file)->dat.file = path;
		obj_array_push(wk, ctx->arr, file);
		break;
	}
	case obj_custom_target: {
		if (!obj_array_foreach(wk, get_obj(wk, val)->dat.custom_target.output,
			ctx, coerce_custom_target_output_iter)) {
			return ir_err;
		}
		break;
	}
	case obj_build_target: {
		struct obj *tgt = get_obj(wk, val);

		char path[PATH_MAX];
		if (!path_join(path, PATH_MAX, wk_str(wk, tgt->dat.tgt.build_dir), wk_str(wk, tgt->dat.tgt.build_name))) {
			return ir_err;
		}

		uint32_t file;
		make_obj(wk, &file, obj_file)->dat.file = wk_str_push(wk, path);
		obj_array_push(wk, ctx->arr, file);
		break;
	}
	case obj_file:
		if (ctx->mode == mode_output) {
			goto type_error;
		}
		obj_array_push(wk, ctx->arr, val);
		break;
	default:
type_error:
		interp_error(wk, ctx->node, "unable to coerce object with type %s into %s",
			obj_type_to_s(get_obj(wk, val)->type), ctx->type);
		return ir_err;
	}

	return ir_cont;
}

static bool
_coerce_files(struct workspace *wk, uint32_t node, uint32_t val, uint32_t *res,
	const char *type_name, exists_func exists, enum coerce_into_files_mode mode)
{
	make_obj(wk, res, obj_array);

	struct coerce_into_files_ctx ctx = {
		.node = node,
		.arr = *res,
		.type = type_name,
		.exists = exists,
		.mode = mode,
	};

	switch (get_obj(wk, val)->type) {
	case obj_array:
		return obj_array_foreach_flat(wk, val, &ctx, coerce_into_files_iter);
	default:
		switch (coerce_into_files_iter(wk, &ctx, val)) {
		case ir_err:
			return false;
		default:
			return true;
		}
	}
}

bool
coerce_output_files(struct workspace *wk, uint32_t node, uint32_t val, uint32_t *res)
{
	return _coerce_files(wk, node, val, res, "output file", NULL, mode_output);
}

bool
coerce_files(struct workspace *wk, uint32_t node, uint32_t val, uint32_t *res)
{
	return _coerce_files(wk, node, val, res, "file", fs_file_exists, mode_input);
}

bool
coerce_dirs(struct workspace *wk, uint32_t node, uint32_t val, uint32_t *res)
{
	return _coerce_files(wk, node, val, res, "directory", fs_dir_exists, mode_input);
}
