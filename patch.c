/*= -*- c-basic-offset: 4; indent-tabs-mode: nil; -*-
 *
 * librsync -- the library for network deltas
 * $Id$
 * 
 * Copyright (C) 2000, 2001 by Martin Pool <mbp@samba.org>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */


                              /*
                               * This is Tranquility Base.
                               */


#include <config.h>

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

#include "rsync.h"
#include "util.h"
#include "trace.h"
#include "protocol.h"
#include "netint.h"
#include "command.h"
#include "sumset.h"
#include "prototab.h"
#include "stream.h"
#include "job.h"



static rs_result rs_patch_s_cmdbyte(rs_job_t *);
static rs_result rs_patch_s_params(rs_job_t *);
static rs_result rs_patch_s_run(rs_job_t *);
static rs_result rs_patch_s_literal(rs_job_t *);
static rs_result rs_patch_s_copy(rs_job_t *);



/**
 * State of trying to read the first byte of a command.  Once we've
 * taken that in, we can know how much data to read to get the
 * arguments.
 */
static rs_result rs_patch_s_cmdbyte(rs_job_t *job)
{
    rs_result result;
        
    if ((result = rs_suck_byte(job, &job->op)) != RS_DONE)
        return result;

    job->cmd = &rs_prototab[job->op];
        
    rs_trace("got command byte 0x%02x (%s), len_1=%ld", job->op,
             rs_op_kind_name(job->cmd->kind),
             (long) job->cmd->len_1);

    if (job->cmd->len_1)
        job->statefn = rs_patch_s_params;
    else {
        job->param1 = job->cmd->immediate;
        job->statefn = rs_patch_s_run;
    }

    return RS_RUNNING;
}


/**
 * Called after reading a command byte to pull in its parameters and
 * then setup to execute the command.
 */
static rs_result rs_patch_s_params(rs_job_t *job)
{
    rs_result result;
    int len = job->cmd->len_1 + job->cmd->len_2;
    void *p;

    assert(len);

    result = rs_scoop_readahead(job, len, &p);
    if (result != RS_DONE)
        return result;

    /* we now must have LEN bytes buffered */
    result = rs_suck_netint(job, &job->param1, job->cmd->len_1);
    /* shouldn't fail, since we already checked */
    assert(result == RS_DONE);

    if (job->cmd->len_2) {
        result = rs_suck_netint(job, &job->param2, job->cmd->len_2);
        assert(result == RS_DONE);
    }

    job->statefn = rs_patch_s_run;

    return RS_RUNNING;
}



/**
 * Called when we've read in the whole command and we need to execute it.
 */
static rs_result rs_patch_s_run(rs_job_t *job)
{
    rs_trace("running command 0x%x, kind %d", job->op, job->cmd->kind);

    switch (job->cmd->kind) {
    case RS_KIND_LITERAL:
        job->statefn = rs_patch_s_literal;
        return RS_RUNNING;
        
    case RS_KIND_END:
        return RS_DONE;
        /* so we exit here; trying to continue causes an error */

    case RS_KIND_COPY:
        job->statefn = rs_patch_s_copy;
        return RS_RUNNING;

    default:
        rs_error("bogus command 0x%02x", job->op);
        return RS_CORRUPT;
    }
}


/**
 * Called when trying to copy through literal data.
 */
static rs_result rs_patch_s_literal(rs_job_t *job)
{
    rs_long_t   len = job->param1;
    
    rs_trace("LITERAL(len=%ld)", (long) len);

    job->stats.lit_cmds++;
    job->stats.lit_bytes += len;

    rs_tube_copy(job, len);

    job->statefn = rs_patch_s_cmdbyte;
    return RS_RUNNING;
}



static rs_result rs_patch_s_copy(rs_job_t *job)
{
    rs_long_t  where, len;

    where = job->param1;
    len = job->param2;
        
    rs_trace("COPY(where=%ld, len=%ld)", (long) where, (long) len);

    /*    rs_tube_copy(job, len); */

    job->statefn = rs_patch_s_cmdbyte;
    return RS_RUNNING;
}


/**
 * Called while we're trying to read the header of the patch.
 */
static rs_result rs_patch_s_header(rs_job_t *job)
{
        int v;
        int result;

        
        if ((result =rs_suck_n4(job, &v)) != RS_DONE)
                return result;

        if (v != RS_DELTA_MAGIC)
                return RS_BAD_MAGIC;

        rs_trace("got patch magic %#10x", v);

        job->statefn = rs_patch_s_cmdbyte;

        return RS_RUNNING;
}



/**
 * \brief Apply a \ref gloss_delta to a \ref gloss_basis to recreate
 * the new file.
 *
 * This gives you back a ::rs_job_t object, which can be cranked by
 * calling rs_job_iter() and updating the stream pointers.  When
 * finished, call rs_job_finish() to dispose of it.
 *
 * \param stream Contains pointers to input and output buffers, to be
 * adjusted by caller on each iteration.
 *
 * \param copy_cb Callback used to retrieve content from the basis
 * file.
 *
 * \param copy_arg Opaque environment pointer passed through to the
 * callback.
 *
 * \todo As output is produced, accumulate the MD4 checksum of the
 * output.  Then if we find a CHECKSUM command we can check it's
 * contents against the output.
 *
 * \todo Implement COPY commands.
 *
 * \sa rs_patch_file()
 */
rs_job_t *
rs_patch_begin(rs_copy_cb *copy_cb, void *copy_arg)
{
    rs_job_t *job = rs_job_new("patch", rs_patch_s_header);
        
    job->copy_cb = copy_cb;
    job->copy_arg = copy_arg;

    rs_mdfour_begin(&job->output_md4);

    return job;
}
