#ifndef MUON_BACKEND_COMMON_ARGS_H
#define MUON_BACKEND_COMMON_ARGS_H

#include "lang/workspace.h"

bool setup_compiler_args(struct workspace *wk, const struct obj *tgt,
	const struct project *proj, obj include_dirs, obj dep_args,
	obj *joined_args);
void setup_linker_args(struct workspace *wk, const struct project *proj,
	const struct obj *tgt, enum linker_type linker,
	enum compiler_language link_lang,
	obj rpaths, obj link_args, obj link_with);

struct setup_compiler_args_includes_ctx {
	obj args;
	enum compiler_type t;
};
enum iteration_result setup_compiler_args_includes(struct workspace *wk, void *_ctx, obj v);
#endif
