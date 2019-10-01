/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  Implementation of option parsing routines (dealing with `struct
  fuse_args`).

  This program can be distributed under the terms of the GNU LGPLv2.
  See the file COPYING.LIB
*/

#include "config.h"
#include "fuse_opt.h"
#include "fuse_misc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

struct fuse_opt_context {
	void *data;
	const struct fuse_opt *opt;
	fuse_opt_proc_t proc;
	int argctr;
	int argc;
	char **argv;
	struct fuse_args outargs;
	char *opts;
	int nonopt;
};

void fuse_opt_free_args(struct fuse_args *args)
{
	if (args) {
		if (args->argv && args->allocated) {
			int i;
			for (i = 0; i < args->argc; i++)
				free(args->argv[i]);
			free(args->argv);
		}
		args->argc = 0;
		args->argv = NULL;
		args->allocated = 0;
	}
}

static int alloc_failed(void)
{
	fprintf(stderr, "fuse: memory allocation failed\n");
	return -1;
}

int fuse_opt_add_arg(struct fuse_args *args, const char *arg)	//Add the arg to the end of args and set args->allocated to be 1; return 0.
{
	char **newargv;
	char *newarg;

	assert(!args->argv || args->allocated);		//Abort if args is both NULL and unallocated

	newarg = strdup(arg);
	if (!newarg)
		return alloc_failed();
	//Copy the argv into a lager place(allocate memory)
	newargv = realloc(args->argv, (args->argc + 2) * sizeof(char *));	//Why is here 2 more sizeof(char *) space is allocated?		Answer: One is for arg,another is for NULL,and the original is `(args->argc + 1) * sizeof(char *)`
	if (!newargv) {
		free(newarg);
		return alloc_failed();
	}

	args->argv = newargv;
	args->allocated = 1;
	args->argv[args->argc++] = newarg;
	args->argv[args->argc] = NULL;
	return 0;
}

static int fuse_opt_insert_arg_common(struct fuse_args *args, int pos,
				      const char *arg)
{
	assert(pos <= args->argc);
	if (fuse_opt_add_arg(args, arg) == -1)
		return -1;

	if (pos != args->argc - 1) {
		char *newarg = args->argv[args->argc - 1];
		memmove(&args->argv[pos + 1], &args->argv[pos],
			sizeof(char *) * (args->argc - pos - 1));
		args->argv[pos] = newarg;
	}
	return 0;
}

int fuse_opt_insert_arg(struct fuse_args *args, int pos, const char *arg)
{
	return fuse_opt_insert_arg_common(args, pos, arg);
}

static int next_arg(struct fuse_opt_context *ctx, const char *opt)	//Check the boundary and add the ctx->argctr by 1.
{
	if (ctx->argctr + 1 >= ctx->argc) {
		fprintf(stderr, "fuse: missing argument after `%s'\n", opt);
		return -1;
	}
	ctx->argctr++;
	return 0;
}

static int add_arg(struct fuse_opt_context *ctx, const char *arg)	//Add arg to ctx->outargs; return 0.
{
	return fuse_opt_add_arg(&ctx->outargs, arg);
}

static int add_opt_common(char **opts, const char *opt, int esc)
{
	unsigned oldlen = *opts ? strlen(*opts) : 0;
	char *d = realloc(*opts, oldlen + 1 + strlen(opt) * 2 + 1);

	if (!d)
		return alloc_failed();

	*opts = d;
	if (oldlen) {
		d += oldlen;
		*d++ = ',';
	}

	for (; *opt; opt++) {
		if (esc && (*opt == ',' || *opt == '\\'))
			*d++ = '\\';
		*d++ = *opt;
	}
	*d = '\0';

	return 0;
}

int fuse_opt_add_opt(char **opts, const char *opt)
{
	return add_opt_common(opts, opt, 0);
}

int fuse_opt_add_opt_escaped(char **opts, const char *opt)
{
	return add_opt_common(opts, opt, 1);
}

static int add_opt(struct fuse_opt_context *ctx, const char *opt)
{
	return add_opt_common(&ctx->opts, opt, 1);
}

static int call_proc(struct fuse_opt_context *ctx, const char *arg, int key,
		     int iso)
{
	if (key == FUSE_OPT_KEY_DISCARD)
		return 0;

	if (key != FUSE_OPT_KEY_KEEP && ctx->proc) {
		int res = ctx->proc(ctx->data, arg, key, &ctx->outargs);
		if (res == -1 || !res)
			return res;
	}
	if (iso)
		return add_opt(ctx, arg);
	else
		return add_arg(ctx, arg);
}

static int match_template(const char *t, const char *arg, unsigned *sepp)	//Compare the whole temple(sepp is 0) or the string before '=' / ' ' which is followed by NULL or '%' with arg(sepp is the length of compared templet).
{
	int arglen = strlen(arg);
	const char *sep = strchr(t, '=');
	sep = sep ? sep : strchr(t, ' ');	//Set sep be the pointer to '=' or ' ', with '=' first.
	if (sep && (!sep[1] || sep[1] == '%')) {	//If find '=' or ' ' in the templet, and followed by NULL or '%'
		int tlen = sep - t;
		if (sep[0] == '=')	//If sep is '=', compared content contains the '='.
			tlen ++;
		if (arglen >= tlen && strncmp(arg, t, tlen) == 0) {
			*sepp = sep - t;	//The compared length(NOT contain the '=').
			return 1;
		}
	}	//Until now, it compare the content before the content before '=' or ' ' and followed by NULL or '%', sepp represent the compared length.
	if (strcmp(t, arg) == 0) {
		*sepp = 0;
		return 1;
	}
	return 0;
}

static const struct fuse_opt *find_opt(const struct fuse_opt *opt,		//Find the option matach with the arg
				       const char *arg, unsigned *sepp)
{
	for (; opt && opt->templ; opt++)	//Understand: Since opt is a single pointer(NOT a struct pinter array and sometimes it is in a struct), how could it add 1 to the opt and make sure that it's not overflow?
		if (match_template(opt->templ, arg, sepp))
			return opt;
	return NULL;
}

int fuse_opt_match(const struct fuse_opt *opts, const char *opt)
{
	unsigned dummy;
	return find_opt(opts, opt, &dummy) ? 1 : 0;
}
//According to *format, copy appropricate string from *param to *var.
static int process_opt_param(void *var, const char *format, const char *param,	//Offset data inside the data,uncompared templet,uncompared arg,the whole arg.
			     const char *arg)
{
	assert(format[0] == '%');	//If format[0] is NOT '%',stop  program.
	if (format[1] == 's') {
		char **s = var;	//Cast void * to char **
		char *copy = strdup(param);
		if (!copy)
			return alloc_failed();

		free(*s);	//Free *var
		*s = copy;	//Store *param in var(type char **).
	} else {
		if (sscanf(param, format, var) != 1) {	//Copy the appropricate char from *param to *var.
			fprintf(stderr, "fuse: invalid parameter in option `%s'\n", arg);
			return -1;
		}
	}
	return 0;
}

static int process_opt(struct fuse_opt_context *ctx,	//According to opt->templet, copy appropricate string from *(arg+sep) or opt->value to ctx->data.
		       const struct fuse_opt *opt, unsigned sep,
		       const char *arg, int iso)
{
	if (opt->offset == -1U) {	//If opt->offset is the default value, call proc
		if (call_proc(ctx, arg, opt->value, iso) == -1)
			return -1;
	} else {
		void *var = (char *)ctx->data + opt->offset;	//Find the offset inside the data.
		if (sep && opt->templ[sep + 1]) {	//If templet is partial compared.
			const char *param = arg + sep;
			if (opt->templ[sep] == '=')	//Skip over the '='.
				param ++;
			if (process_opt_param(var, opt->templ + sep + 1,	//Offset data inside the data,uncompared templet,uncompared arg,the whole arg
					      param, arg) == -1)	//According to uncompared templet, copy string from uncompared param to var.
				return -1;
		} else
			*(int *)var = opt->value;	//Set var to opt->value.
	}
	return 0;
}

static int process_opt_sep_arg(struct fuse_opt_context *ctx,	//Merge the seperated arguments in ctx->argv and copy the appropricate arguments to ctx->data according to opt->templet.
			       const struct fuse_opt *opt, unsigned sep,
			       const char *arg, int iso)
{
	int res;
	char *newarg;
	char *param;

	if (next_arg(ctx, arg) == -1)
		return -1;

	param = ctx->argv[ctx->argctr];
	newarg = malloc(sep + strlen(param) + 1);
	if (!newarg)
		return alloc_failed();

	memcpy(newarg, arg, sep);
	strcpy(newarg + sep, param);	//Copy the arg[0-sep] and the next arg to newarg.
	res = process_opt(ctx, opt, sep, newarg, iso);	//According to opt->templet, copy appropricate string from *(newarg+sep) or opt->value to ctx->data.
	free(newarg);

	return res;
}

static int process_gopt(struct fuse_opt_context *ctx, const char *arg, int iso)	//Find the opt matching with the arg, and copy arg to the appropricate position of ctx->data.
{
	unsigned sep;
	const struct fuse_opt *opt = find_opt(ctx->opt, arg, &sep);	//Find the opt matching with the arg, sep is the compared length of templet.
	if (opt) {	//If matched opt has been found.
		for (; opt; opt = find_opt(opt + 1, arg, &sep)) {
			int res;
			if (sep && opt->templ[sep] == ' ' && !arg[sep])	//If only part of tamplet is compared and latest compared char is ' ' and arg has next char.
				res = process_opt_sep_arg(ctx, opt, sep, arg,
							  iso);
			else
				res = process_opt(ctx, opt, sep, arg, iso);	//Until now, copy appropricate data from *(arg+sep) to ctx->data.
			if (res == -1)
				return -1;
		}
		return 0;
	} else	//If have not found matched opt, call proc.
		return call_proc(ctx, arg, FUSE_OPT_KEY_OPT, iso);
}

static int process_real_option_group(struct fuse_opt_context *ctx, char *opts)
{
	char *s = opts;
	char *d = s;
	int end = 0;

	while (!end) {
		if (*s == '\0')
			end = 1;
		if (*s == ',' || end) {		//If *s is ',' or '\0'
			int res;

			*d = '\0';
			res = process_gopt(ctx, opts, 1);	//Find the ctx->opt matching with the opts, and copy arg(part of opts) to the appropricate position of ctx->data.
			if (res == -1)
				return -1;
			d = opts;
		} else {
			if (s[0] == '\\' && s[1] != '\0') {		//If s[0] is '\' and has next char.
				s++;
				if (s[0] >= '0' && s[0] <= '3' &&	//Copy the \[0-3][0-7][0-7] formate string into a single char
				    s[1] >= '0' && s[1] <= '7' &&
				    s[2] >= '0' && s[2] <= '7') {
					*d++ = (s[0] - '0') * 0100 +
						(s[1] - '0') * 0010 +
						(s[2] - '0');
					s += 2;
				} else {
					*d++ = *s;
				}
			} else {
				*d++ = *s;
			}
		}
		s++;
	}

	return 0;
}

static int process_option_group(struct fuse_opt_context *ctx, const char *opts)
{
	int res;
	char *copy = strdup(opts);

	if (!copy) {
		fprintf(stderr, "fuse: memory allocation failed\n");
		return -1;
	}
	res = process_real_option_group(ctx, copy);
	free(copy);
	return res;
}

static int process_one(struct fuse_opt_context *ctx, const char *arg)
{
	if (ctx->nonopt || arg[0] != '-')	//If ctx have no opt or the arg is not begin with '-',call the proc.
		return call_proc(ctx, arg, FUSE_OPT_KEY_NONOPT, 0);
	else if (arg[1] == 'o') {	//If arg start with '-o'.
		if (arg[2])		//If arg start with '-o' and the following char is not NULL.
			return process_option_group(ctx, arg + 2);
		else {
			if (next_arg(ctx, arg) == -1)
				return -1;

			return process_option_group(ctx,
						    ctx->argv[ctx->argctr]);
		}
	} else if (arg[1] == '-' && !arg[2]) {
		if (add_arg(ctx, arg) == -1)
			return -1;
		ctx->nonopt = ctx->outargs.argc;
		return 0;
	} else
		return process_gopt(ctx, arg, 0);
}

static int opt_parse(struct fuse_opt_context *ctx)
{
	if (ctx->argc) {
		if (add_arg(ctx, ctx->argv[0]) == -1)	//Add the command name to the end of ctx->outatgs
			return -1;
	}

	for (ctx->argctr = 1; ctx->argctr < ctx->argc; ctx->argctr++)
		if (process_one(ctx, ctx->argv[ctx->argctr]) == -1)
			return -1;

	if (ctx->opts) {
		if (fuse_opt_insert_arg(&ctx->outargs, 1, "-o") == -1 ||
		    fuse_opt_insert_arg(&ctx->outargs, 2, ctx->opts) == -1)
			return -1;
	}

	/* If option separator ("--") is the last argument, remove it */
	if (ctx->nonopt && ctx->nonopt == ctx->outargs.argc &&
	    strcmp(ctx->outargs.argv[ctx->outargs.argc - 1], "--") == 0) {
		free(ctx->outargs.argv[ctx->outargs.argc - 1]);
		ctx->outargs.argv[--ctx->outargs.argc] = NULL;
	}

	return 0;
}

int fuse_opt_parse(struct fuse_args *args, void *data,
		   const struct fuse_opt opts[], fuse_opt_proc_t proc)
{
	int res;
	struct fuse_opt_context ctx = {		//Copy the data into ctx
		.data = data,
		.opt = opts,
		.proc = proc,
	};

	if (!args || !args->argv || !args->argc)	//Check the args and arc
		return 0;

	ctx.argc = args->argc;
	ctx.argv = args->argv;
	//Until now, all the DATA from the caller is copied into the parameter ctx
	res = opt_parse(&ctx);
	if (res != -1) {
		struct fuse_args tmp = *args;
		*args = ctx.outargs;
		ctx.outargs = tmp;
	}
	free(ctx.opts);
	fuse_opt_free_args(&ctx.outargs);
	return res;
}
