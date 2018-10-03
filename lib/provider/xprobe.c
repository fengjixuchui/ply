#define _GNU_SOURCE 		/* asprintf */
#include <assert.h>
#include <errno.h>
#include <fnmatch.h>
#include <glob.h>
#include <stdio.h>
#include <string.h>

#include <ply/ply.h>
#include <ply/internal.h>

#include "xprobe.h"

static int xprobe_stem(struct ply_probe *pb, char type, char *stem, size_t size)
{
	return snprintf(stem, size, "%c:%s/p%"PRIxPTR"_",
			type, pb->ply->group, (uintptr_t)pb);
}

static int __xprobe_create(FILE *ctrl, const char *stem, const char *func)
{
	int len = 

	fputs(stem, ctrl);
	fputs(func, ctrl);
	fputc( ' ', ctrl);
	fputs(func, ctrl);
	fputc('\n', ctrl);
	return strlen(stem) + 2 * strlen(func) + 2;
}

static int xprobe_glob(struct ply_probe *pb, glob_t *gl)
{
	char *evglob;
	int err;

	asprintf(&evglob, TRACEPATH "events/%s/p%"PRIxPTR"_*",
		 pb->ply->group, (uintptr_t)pb);

	err = glob(evglob, 0, NULL, gl);
	free(evglob);
	return err ? -EINVAL : 0;
}

static char *xprobe_func(struct ply_probe *pb, char *path)
{
	char *slash;

	path += strlen(TRACEPATH "events/");
	path += strlen(pb->ply->group);

	slash = strchr(path, '/');
	assert(slash);
	*slash = '\0';
	return path;
}


int xprobe_detach(struct ply_probe *pb)
{
	struct xprobe *xp = pb->provider_data;
	glob_t gl;
	size_t i, evstart;
	int err, pending;

	if (!xp->ctrl)
		return 0;

	for (i = 0; i < xp->n_evs; i++)
		close(xp->evfds[i]);

	err = xprobe_glob(pb, &gl);
	if (err)
		return err;

	assert(gl.gl_pathc == xp->n_evs);

	evstart = strlen(TRACEPATH "events/");
	pending = 0;

	for (i = 0; i < gl.gl_pathc; i++) {
		fputs("-:", xp->ctrl);
		pending += 2;
		fputs(&gl.gl_pathv[i][evstart], xp->ctrl);
		pending += strlen(&gl.gl_pathv[i][evstart]);
		fputc('\n', xp->ctrl);
		pending++;

		/* The kernel parser doesn't deal with a probe definition
		 * being split across two writes. So if there's less than
		 * 512 bytes left, flush the buffer. */
		if (pending > (0x1000 - 0x200)) {
			err = fflush(xp->ctrl);
			if (err)
				break;

			pending = 0;
		}
	}

	globfree(&gl);
	fclose(xp->ctrl);
	return err;
}


static int xprobe_create_pattern(struct ply_probe *pb)
{
	struct xprobe *xp = pb->provider_data;
	struct ksym *sym;
	int err, pending = 0;

	ksyms_foreach(sym, pb->ply->ksyms) {
		if (fnmatch(xp->pattern, sym->sym, FNM_EXTMATCH))
			continue;

		pending += __xprobe_create(xp->ctrl, xp->stem, sym->sym);
		xp->n_evs++;

		/* The kernel parser doesn't deal with a probe definition
		 * being split across two writes. So if there's less than
		 * 512 bytes left, flush the buffer. */
		if (pending > (0x1000 - 0x200)) {
			err = fflush(xp->ctrl);
			if (err)
				return -errno;

			pending = 0;
		}
	}

	return 0;
}	

static int xprobe_create(struct ply_probe *pb)
{
	struct xprobe *xp = pb->provider_data;
	int err = 0;

	xprobe_stem(pb, xp->type, xp->stem, sizeof(xp->stem));

	if (strpbrk(xp->pattern, "?*[!@") && pb->ply->ksyms) {
		err = xprobe_create_pattern(pb);
	} else {
		__xprobe_create(xp->ctrl, xp->stem, xp->pattern);
		xp->n_evs++;
	}

	if (!err)
		err = fflush(xp->ctrl) ? -errno : 0;
	return err;
}

static int __xprobe_attach(struct ply_probe *pb)
{
	struct xprobe *xp = pb->provider_data;
	glob_t gl;
	int err, i;

	err = xprobe_glob(pb, &gl);
	if (err)
		return err;

	if (gl.gl_pathc != xp->n_evs) {
		_d("n:%d c:%d\n", xp->n_evs, gl.gl_pathc);
		pause();
	}
	
	assert(gl.gl_pathc == xp->n_evs);
	for (i = 0; i < (int)gl.gl_pathc; i++) {
		xp->evfds[i] = perf_event_attach(pb, gl.gl_pathv[i]);
		if (xp->evfds[i] < 0) {
			err = xp->evfds[i];
			break;
		}
	}

	globfree(&gl);
	return err;
}

int xprobe_attach(struct ply_probe *pb)
{
	struct xprobe *xp = pb->provider_data;
	char *func;
	int err;

	/* TODO: mode should be a+ and we should clean this up on
	 * detach. */
	xp->ctrl = fopenf("a+", TRACEPATH "%s", xp->ctrl_name);
	if (!xp->ctrl)
		return -errno;

	err = setvbuf(xp->ctrl, NULL, _IOFBF, 0x1000);
	if (err) {
		err = -errno;
		goto err_close;
	}

	err = xprobe_create(pb);
	if (err)
		goto err_close;

	xp->evfds = xcalloc(xp->n_evs, sizeof(xp->evfds));

	err = __xprobe_attach(pb);
	if (err)
		goto err_destroy;

	return 0;

err_destroy:
	/* xprobe_destroy(xp); */

err_close:
	fclose(xp->ctrl);
	return err;
}
