/*
 * ipmi.c
 *
 * MontaVista IPMI generic code
 *
 * Author: MontaVista Software, Inc.
 *         Corey Minyard <minyard@mvista.com>
 *         source@mvista.com
 *
 * Copyright 2002,2003 MontaVista Software Inc.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2 of
 *  the License, or (at your option) any later version.
 *
 *
 *  THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 *  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 *  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 *  OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 *  TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 *  USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this program; if not, write to the Free
 *  Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <config.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>

#include <OpenIPMI/os_handler.h>
#include <OpenIPMI/ipmi_conn.h>
#include <OpenIPMI/ipmi_err.h>
#include <OpenIPMI/ipmi_auth.h>
#include <OpenIPMI/ipmi_lan.h>
#include <OpenIPMI/ipmi_smi.h>
#include <OpenIPMI/ipmi_msgbits.h>

#include <OpenIPMI/internal/ipmi_domain.h>
#include <OpenIPMI/internal/ipmi_mc.h>
#include <OpenIPMI/internal/ipmi_int.h>
#include <OpenIPMI/internal/ipmi_oem.h>
#include <OpenIPMI/internal/locked_list.h>

#if defined(DEBUG_MSG) || defined(DEBUG_RAWMSG)
static void
dump_hex(const void *vdata, int len)
{
    const unsigned char *data = vdata;
    int i;
    for (i=0; i<len; i++) {
	if ((i != 0) && ((i % 16) == 0)) {
	    ipmi_log(IPMI_LOG_DEBUG_CONT, "\n  ");
	}
	ipmi_log(IPMI_LOG_DEBUG_CONT, " %2.2x", data[i]);
    }
}
#endif
static os_handler_t *ipmi_os_handler;

unsigned int i__ipmi_log_mask = 0;

os_handler_t *
ipmi_get_global_os_handler(void)
{
    return ipmi_os_handler;
}
    
int
ipmi_create_global_lock(ipmi_lock_t **new_lock)
{
    return ipmi_create_lock_os_hnd(ipmi_os_handler, new_lock);
}

int
ipmi_create_lock(ipmi_domain_t *domain, ipmi_lock_t **new_lock)
{
    return ipmi_create_lock_os_hnd(ipmi_domain_get_os_hnd(domain), new_lock);
}

void
ipmi_log(enum ipmi_log_type_e log_type, const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    if (ipmi_os_handler && ipmi_os_handler->vlog)
	ipmi_os_handler->vlog(ipmi_os_handler, log_type, format, ap);
    else {
	vfprintf(stderr, format, ap);
	switch (log_type) {
	case IPMI_LOG_DEBUG_START:
	case IPMI_LOG_DEBUG_CONT:
	    break;
	default:
	    fprintf(stderr, "\n");
	}
    }
    va_end(ap);
}

static long seq = 0;
static os_hnd_lock_t *seq_lock;
long
ipmi_get_seq(void)
{
    long rv;

    if (seq_lock)
	ipmi_os_handler->lock(ipmi_os_handler, seq_lock);
    rv = seq;
    seq++;
    if (seq_lock)
	ipmi_os_handler->unlock(ipmi_os_handler, seq_lock);

    return rv;
}

void
ipmi_event_state_init(ipmi_event_state_t *events)
{
    events->status = 0;
    events->__assertion_events = 0;
    events->__deassertion_events = 0;
}

void
ipmi_threshold_event_clear(ipmi_event_state_t          *events,
			   enum ipmi_thresh_e          type,
			   enum ipmi_event_value_dir_e value_dir,
			   enum ipmi_event_dir_e       dir)
{
    if (dir == IPMI_ASSERTION) {
	events->__assertion_events &= ~(1 << (type*2+value_dir));
    } else {
	events->__deassertion_events &= ~(1 << (type*2+value_dir));
    }
}

void
ipmi_threshold_event_set(ipmi_event_state_t          *events,
			 enum ipmi_thresh_e          type,
			 enum ipmi_event_value_dir_e value_dir,
			 enum ipmi_event_dir_e       dir)
{
    if (dir == IPMI_ASSERTION) {
	events->__assertion_events |= 1 << (type*2+value_dir);
    } else {
	events->__deassertion_events |= 1 << (type*2+value_dir);
    }
}

int
ipmi_is_threshold_event_set(ipmi_event_state_t          *events,
			    enum ipmi_thresh_e          type,
			    enum ipmi_event_value_dir_e value_dir,
			    enum ipmi_event_dir_e       dir)
{
    if (dir == IPMI_ASSERTION) {
	return (events->__assertion_events & (1 << (type*2+value_dir))) != 0;
    } else {
	return (events->__deassertion_events & (1 << (type*2+value_dir))) != 0;
    }
}

void
ipmi_discrete_event_clear(ipmi_event_state_t    *events,
			  int                   event_offset,
			  enum ipmi_event_dir_e dir)
{
    if (dir == IPMI_ASSERTION) {
	events->__assertion_events &= ~(1 << event_offset);
    } else {
	events->__deassertion_events &= ~(1 << event_offset);
    }
}

void
ipmi_discrete_event_set(ipmi_event_state_t    *events,
			int                   event_offset,
			enum ipmi_event_dir_e dir)
{
    if (dir == IPMI_ASSERTION) {
	events->__assertion_events |= 1 << event_offset;
    } else {
	events->__deassertion_events |= 1 << event_offset;
    }
}

int
ipmi_is_discrete_event_set(ipmi_event_state_t    *events,
			   int                   event_offset,
			   enum ipmi_event_dir_e dir)
{
    if (dir == IPMI_ASSERTION) {
	return (events->__assertion_events & (1 << event_offset)) != 0;
    } else {
	return (events->__deassertion_events & (1 << event_offset)) != 0;
    }
}

#define IPMI_SENSOR_EVENTS_ENABLED	0x80
#define IPMI_SENSOR_SCANNING_ENABLED	0x40
#define IPMI_SENSOR_BUSY		0x20

unsigned int ipmi_event_state_size(void)
{
    return sizeof(ipmi_event_state_t);
}

void
ipmi_copy_event_state(ipmi_event_state_t *dest, ipmi_event_state_t *src)
{
    *dest = *src;
}

void
ipmi_event_state_set_events_enabled(ipmi_event_state_t *events, int val)
{
    if (val)
	events->status |= IPMI_SENSOR_EVENTS_ENABLED;
    else
	events->status &= ~IPMI_SENSOR_EVENTS_ENABLED;
}

int
ipmi_event_state_get_events_enabled(ipmi_event_state_t *events)
{
    return (events->status >> 7) & 1;
}

void
ipmi_event_state_set_scanning_enabled(ipmi_event_state_t *events, int val)
{
    if (val)
	events->status |= IPMI_SENSOR_SCANNING_ENABLED;
    else
	events->status &= ~IPMI_SENSOR_SCANNING_ENABLED;
}

int
ipmi_event_state_get_scanning_enabled(ipmi_event_state_t *events)
{
    return (events->status >> 6) & 1;
}

void
ipmi_event_state_set_busy(ipmi_event_state_t *events, int val)
{
    if (val)
	events->status |= IPMI_SENSOR_BUSY;
    else
	events->status &= ~IPMI_SENSOR_BUSY;
}

int
ipmi_event_state_get_busy(ipmi_event_state_t *events)
{
    return (events->status >> 5) & 1;
}

unsigned int ipmi_thresholds_size(void)
{
    return sizeof(ipmi_thresholds_t);
}

void
ipmi_copy_thresholds(ipmi_thresholds_t *dest, ipmi_thresholds_t *src)
{
    *dest = *src;
}

int ipmi_thresholds_init(ipmi_thresholds_t *th)
{
    int i;
    for (i=0; i<6; i++)
	th->vals[i].status = 0;
    return 0;
}

int ipmi_threshold_set(ipmi_thresholds_t  *th,
		       ipmi_sensor_t      *sensor,
		       enum ipmi_thresh_e threshold,
		       double             value)
{
    int rv = 0;

    if (threshold > IPMI_UPPER_NON_RECOVERABLE)
	return EINVAL;

    if (sensor) {
	int val;
	rv = ipmi_sensor_threshold_settable(sensor, threshold, &val);
	if (rv)
	    return rv;
	if (!val)
	    return ENOSYS;
    }

    th->vals[threshold].status = 1;
    th->vals[threshold].val = value;
    return 0;
}

int ipmi_threshold_get(ipmi_thresholds_t  *th,
		       enum ipmi_thresh_e threshold,
		       double             *value)
{
    if (threshold > IPMI_UPPER_NON_RECOVERABLE)
	return EINVAL;

    if (th->vals[threshold].status) {
	*value = th->vals[threshold].val;
	return 0;
    } else {
	return ENOSYS;
    }
}

unsigned int ipmi_states_size(void)
{
    return sizeof(ipmi_states_t);
}

void
ipmi_copy_states(ipmi_states_t *dest, ipmi_states_t *src)
{
    *dest = *src;
}

void
ipmi_init_states(ipmi_states_t *states)
{
    states->__event_messages_enabled = 0;
    states->__sensor_scanning_enabled = 0;
    states->__initial_update_in_progress = 0;
    states->__states = 0;
}

int
ipmi_is_event_messages_enabled(ipmi_states_t *states)
{
    return states->__event_messages_enabled;
}

void
ipmi_set_event_messages_enabled(ipmi_states_t *states, int val)
{
    states->__event_messages_enabled = val;
}

int
ipmi_is_sensor_scanning_enabled(ipmi_states_t *states)
{
    return states->__sensor_scanning_enabled;
}

void
ipmi_set_sensor_scanning_enabled(ipmi_states_t *states, int val)
{
    states->__sensor_scanning_enabled = val;
}

int
ipmi_is_initial_update_in_progress(ipmi_states_t *states)
{
    return states->__initial_update_in_progress;
}

void
ipmi_set_initial_update_in_progress(ipmi_states_t *states, int val)
{
    states->__initial_update_in_progress = val;
}

int
ipmi_is_state_set(ipmi_states_t *states,
		  int           state_num)
{
    return (states->__states & (1 << state_num)) != 0;
}

void
ipmi_set_state(ipmi_states_t *states,
	       int           state_num,
	       int           val)
{
    if (val)
	states->__states |= 1 << state_num;
    else
	states->__states &= ~(1 << state_num);
}

int
ipmi_is_threshold_out_of_range(ipmi_states_t      *states,
			       enum ipmi_thresh_e thresh)
{
    return (states->__states & (1 << thresh)) != 0;
}

void
ipmi_set_threshold_out_of_range(ipmi_states_t      *states,
				enum ipmi_thresh_e thresh,
				int                val)
{
    if (val)
	states->__states |= 1 << thresh;
    else
	states->__states &= ~(1 << thresh);
}

void ipmi_oem_force_conn_init(void);
int ipmi_oem_motorola_mxp_init(void);
int ipmi_oem_intel_init(void);
int ipmi_oem_kontron_conn_init(void);
int ipmi_oem_atca_conn_init(void);
int ipmi_oem_atca_init(void);
int init_oem_test(void);
int i_ipmi_smi_init(os_handler_t *os_hnd);
int i_ipmi_lan_init(os_handler_t *os_hnd);
int ipmi_malloc_init(os_handler_t *os_hnd);
void ipmi_malloc_shutdown(void);
int i_ipmi_rakp_init(void);
int i_ipmi_aes_cbc_init(void);
int i_ipmi_hmac_init(void);
int i_ipmi_md5_init(void);
int i_ipmi_fru_init(void);
int i_ipmi_normal_fru_init(void);
int i_ipmi_fru_spd_decoder_init(void);
void i_ipmi_fru_shutdown(void);
void i_ipmi_normal_fru_shutdown(void);
void i_ipmi_fru_spd_decoder_shutdown(void);
int i_ipmi_sol_init(void);

void i_ipmi_rakp_shutdown(void);
void i_ipmi_aes_cbc_shutdown(void);
void i_ipmi_hmac_shutdown(void);
void i_ipmi_md5_shutdown(void);
void ipmi_oem_atca_conn_shutdown(void);
void ipmi_oem_intel_shutdown(void);
void ipmi_oem_kontron_conn_shutdown(void);
void ipmi_oem_atca_shutdown(void);
int i_ipmi_smi_shutdown(void);
int i_ipmi_lan_shutdown(void);
void i_ipmi_sol_shutdown(void);


static locked_list_t *con_type_list;
static int ipmi_initialized;

int
ipmi_init(os_handler_t *handler)
{
    int rv;

    if (ipmi_initialized)
	return 0;

    ipmi_os_handler = handler;

    /* Set up memory allocation first */
    ipmi_malloc_init(handler);

    /* Set up logging in malloc code. */
    ipmi_malloc_log = ipmi_log;

    con_type_list = locked_list_alloc(handler);

    rv = i_ipmi_conn_init(handler);
    if (rv)
	return rv;

    ipmi_initialized = 1;

    if (handler->create_lock) {
	rv = handler->create_lock(handler, &seq_lock);
	if (rv)
	    goto out_err;
    } else {
	seq_lock = NULL;
    }

#ifdef HAVE_OPENIPMI_SMI
    rv = i_ipmi_smi_init(handler);
    if (rv)
	goto out_err;
#endif

    rv = i_ipmi_lan_init(handler);
    if (rv)
	goto out_err;

    i_ipmi_domain_init();
    i_ipmi_mc_init();

    rv = i_ipmi_rakp_init();
    if (rv)
	goto out_err;
    rv = i_ipmi_aes_cbc_init();
    if (rv)
	goto out_err;
    rv = i_ipmi_hmac_init();
    if (rv)
	goto out_err;
    rv = i_ipmi_md5_init();
    if (rv)
	goto out_err;

    rv = i_ipmi_fru_init();
    if (rv)
	goto out_err;

    rv = i_ipmi_normal_fru_init();
    if (rv)
	goto out_err;

    rv = i_ipmi_fru_spd_decoder_init();
    if (rv)
	goto out_err;

    rv = i_ipmi_sol_init();
    if (rv)
	goto out_err;

    /* Call the OEM handlers. */
    ipmi_oem_force_conn_init();
    ipmi_oem_motorola_mxp_init();
    ipmi_oem_intel_init();
    ipmi_oem_kontron_conn_init();
    ipmi_oem_atca_conn_init();
    ipmi_oem_atca_init();
    init_oem_test();

    return 0;

 out_err:
    ipmi_shutdown();
    return rv;
}

void
ipmi_shutdown(void)
{
    if (! ipmi_initialized)
	return;

    i_ipmi_rakp_shutdown();
    i_ipmi_aes_cbc_shutdown();
    i_ipmi_hmac_shutdown();
    i_ipmi_md5_shutdown();
    i_ipmi_sol_shutdown();
    i_ipmi_lan_shutdown();
#ifdef HAVE_OPENIPMI_SMI
    i_ipmi_smi_shutdown();
#endif
    ipmi_oem_atca_shutdown();
    ipmi_oem_atca_conn_shutdown();
    ipmi_oem_intel_shutdown();
    ipmi_oem_kontron_conn_shutdown();
    i_ipmi_mc_shutdown();
    i_ipmi_domain_shutdown();
    i_ipmi_fru_spd_decoder_shutdown();
    i_ipmi_conn_shutdown();
    i_ipmi_normal_fru_shutdown();
    i_ipmi_fru_shutdown();
    if (seq_lock)
	ipmi_os_handler->destroy_lock(ipmi_os_handler, seq_lock);
    if (con_type_list)
	locked_list_destroy(con_type_list);
    ipmi_debug_malloc_cleanup();
    ipmi_malloc_shutdown();

    ipmi_os_handler = NULL;

    ipmi_initialized = 0;
}

#define CHECK_ARG \
    do { \
        if (*curr_arg >= arg_count) { \
	    rv = EINVAL; \
	    goto out_err; \
        } \
    } while(0)

int
ipmi_parse_args(int         *curr_arg,
		int         arg_count,
		char        * const *args,
		ipmi_args_t **iargs)
{
    int rv;
    CHECK_ARG;

    if (strcmp(args[*curr_arg], "smi") == 0) {
	/* Format is unchanged. */
	return ipmi_parse_args2(curr_arg, arg_count, args, iargs);
    } else if (strcmp(args[*curr_arg], "lan") == 0) {
	/* Convert the format over to the new one and call the new
	   handler. */
	char *largs[16];
	int  c = 0;
	int  newcarg = 0;
	char *addr, *addr2 = NULL;
	char *port, *port2 = NULL;
	char *auth;
	char *priv;
	char *username;
	char *pw;

	largs[c] = args[*curr_arg];
	(*curr_arg)++; CHECK_ARG;
	c++;

	addr = args[*curr_arg];
	(*curr_arg)++; CHECK_ARG;

	port = args[*curr_arg];
	(*curr_arg)++; CHECK_ARG;

	if ((strcmp(args[*curr_arg], "none") != 0)
	    && (strcmp(args[*curr_arg], "md2") != 0)
	    && (strcmp(args[*curr_arg], "md5") != 0)
	    && (strcmp(args[*curr_arg], "straight") != 0)
	    && (strcmp(args[*curr_arg], "rmcp+") != 0))
	{
	    addr2 = args[*curr_arg];
	    (*curr_arg)++; CHECK_ARG;

	    port2 = args[*curr_arg];
	    (*curr_arg)++; CHECK_ARG;
	}

	auth = args[*curr_arg];
	(*curr_arg)++; CHECK_ARG;

	priv = args[*curr_arg];
	(*curr_arg)++; CHECK_ARG;

	username = args[*curr_arg];
	(*curr_arg)++; CHECK_ARG;

	pw = args[*curr_arg];
	(*curr_arg)++;

	largs[c] = "-U"; c++; largs[c] = username; c++;
	largs[c] = "-P"; c++; largs[c] = pw; c++;
	largs[c] = "-A"; c++; largs[c] = auth; c++;
	largs[c] = "-L"; c++; largs[c] = priv; c++;
	if (addr2) {
	    largs[c] = "-s"; c++;
	}
	largs[c] = "-p"; c++; largs[c] = port; c++;
	if (port2) {
	    largs[c] = "-p2"; c++; largs[c] = port2; c++;
	}
	largs[c] = addr; c++;
	if (addr2) {
	    largs[c] = addr2; c++;
	}

	return ipmi_parse_args2(&newcarg, c, largs, iargs);
    } else {
	rv = EINVAL;
	goto out_err;
    }
    return 0;

 out_err:
    return rv;
}

struct ipmi_args_s
{
    ipmi_args_free_cb     free;
    ipmi_args_connect_cb  connect;
    ipmi_args_get_val_cb  get_val;
    ipmi_args_set_val_cb  set_val;
    ipmi_args_copy_cb     copy;
    ipmi_args_validate_cb validate;
    ipmi_args_free_val_cb free_val;
    ipmi_args_get_type_cb get_type;
};

struct ipmi_con_setup_s
{
    ipmi_con_parse_args_cb parse;
    ipmi_con_get_help_cb   help;
    ipmi_con_alloc_args_cb alloc;
};
ipmi_con_setup_t *
i_ipmi_alloc_con_setup(ipmi_con_parse_args_cb parse,
		       ipmi_con_get_help_cb   help,
		       ipmi_con_alloc_args_cb alloc)
{
    ipmi_con_setup_t *rv;

    rv = ipmi_mem_alloc(sizeof(*rv));
    if (!rv)
	return NULL;
    memset(rv, 0, sizeof(*rv));
    rv->parse = parse;
    rv->help = help;
    rv->alloc = alloc;
    return rv;
}

void
i_ipmi_free_con_setup(ipmi_con_setup_t *v)
{
    ipmi_mem_free(v);
}

typedef struct con_type_alloc_data_s
{
    char        *con_type;
    ipmi_args_t *args;
    int         err;
} con_type_alloc_data_t;

static int
con_type_alloc_handler(void *cb_data, void *item1, void *item2)
{
    ipmi_con_setup_t      *setup = item2;
    con_type_alloc_data_t *data = cb_data;

    if (strcmp(data->con_type, item1) == 0) {
	data->args = setup->alloc();
	if (!data->args)
	    data->err = ENOMEM;
	else
	    data->err = 0;
	return LOCKED_LIST_ITER_STOP;
    }
    return LOCKED_LIST_ITER_CONTINUE;
}

int
ipmi_args_alloc(char *con_type, ipmi_args_t **args)
{
    con_type_alloc_data_t data;

    data.con_type = con_type;
    data.err = EINVAL;
    locked_list_iterate(con_type_list, con_type_alloc_handler, &data);
    if (!data.err)
	*args = data.args;
    return data.err;
}

typedef struct con_type_help_data_s
{
    ipmi_iter_help_cb help_cb;
    void              *cb_data;
} con_type_help_data_t;

static int
con_type_help_handler(void *cb_data, void *item1, void *item2)
{
    ipmi_con_setup_t     *setup = item2;
    con_type_help_data_t *data = cb_data;

    data->help_cb(item1, setup->help(), data->cb_data);
    return LOCKED_LIST_ITER_CONTINUE;
}

void
ipmi_parse_args_iter_help(ipmi_iter_help_cb help_cb, void *cb_data)
{
    con_type_help_data_t data;

    data.help_cb = help_cb;
    data.cb_data = cb_data;
    locked_list_iterate(con_type_list, con_type_help_handler, &data);
}

typedef struct con_setup_data_s
{
    const char  *name;
    int         err;

    int         *curr_arg;
    int         arg_count;
    char        * const *args;
    ipmi_args_t *iargs;
} con_setup_data_t;

static int
con_type_check_parse(void *cb_data, void *item1, void *item2)
{
    con_setup_data_t *data = cb_data;

    if (strcmp(item1, data->name) == 0) {
	ipmi_con_setup_t *setup = item2;
	data->err = setup->parse(data->curr_arg, data->arg_count, data->args,
				 &data->iargs);
	return LOCKED_LIST_ITER_STOP;
    }
    return LOCKED_LIST_ITER_CONTINUE;
}

int
ipmi_parse_args2(int         *curr_arg,
		 int         arg_count,
		 char        * const *args,
		 ipmi_args_t **iargs)
{
    con_setup_data_t data;
    int              rv;

    CHECK_ARG;
    data.err = EINVAL;
    data.name = args[*curr_arg];
    (*curr_arg)++;
    data.curr_arg = curr_arg;
    data.arg_count = arg_count;
    data.args = args;
    locked_list_iterate(con_type_list, con_type_check_parse, &data);
    if (data.err) {
	rv = data.err;
	goto out_err;
    }

    *iargs = data.iargs;
    return 0;

 out_err:
    return rv;
}

void
ipmi_free_args(ipmi_args_t *args)
{
    if (args->free)
	args->free(args);
    ipmi_mem_free(args);
}

int
ipmi_args_setup_con(ipmi_args_t  *args,
		    os_handler_t *handlers,
		    void         *user_data,
		    ipmi_con_t   **con)
{
    return args->connect(args, handlers, user_data, con);
}

const char *
ipmi_args_get_type(ipmi_args_t *args)
{
    return args->get_type(args);
}

int
ipmi_args_get_val(ipmi_args_t  *args,
		  unsigned int argnum,
		  const char   **name,
		  const char   **type,
		  const char   **help,
		  char         **value,
		  const char   ***range)
{
    return args->get_val(args, argnum, name, type, help, value, range);
}

int
ipmi_args_set_val(ipmi_args_t  *args,
		  unsigned int argnum,
		  const char   *name,
		  const char   *value)
{
    return args->set_val(args, argnum, name, value);
}

void
ipmi_args_free_str(ipmi_args_t *args, char *str)
{
    args->free_val(args, str);
}

ipmi_args_t *
ipmi_args_copy(ipmi_args_t *args)
{
    return args->copy(args);
}

int
ipmi_args_validate(ipmi_args_t *args, int *argnum)
{
    return args->validate(args, argnum);
}

ipmi_args_t *
i_ipmi_args_alloc(ipmi_args_free_cb     free,
		 ipmi_args_connect_cb  connect,
		 ipmi_args_get_val_cb  get_val,
		 ipmi_args_set_val_cb  set_val,
		 ipmi_args_copy_cb     copy,
		 ipmi_args_validate_cb validate,
		 ipmi_args_free_val_cb free_val,
		 ipmi_args_get_type_cb get_type,
		 unsigned int          extra_data_len)
{
    ipmi_args_t *val;

    val = ipmi_mem_alloc(sizeof(*val) + extra_data_len);
    if (!val)
	return NULL;
    memset(val, 0, sizeof(*val) + extra_data_len);
    val->free = free;
    val->connect = connect;
    val->get_val = get_val;
    val->set_val = set_val;
    val->copy = copy;
    val->validate = validate;
    val->free_val = free_val;
    val->get_type = get_type;
    return val;
}

void *
i_ipmi_args_get_extra_data(ipmi_args_t *args)
{
    return ((char *) args) + sizeof(*args);
}

int
i_ipmi_register_con_type(const char *name, ipmi_con_setup_t *setup)
{
    if (locked_list_add(con_type_list, (void *) name, setup))
	return 0;
    else
	return ENOMEM;
}

typedef struct con_type_check_remover_s
{
    const char *name;
    int        err;
} con_type_check_remover_t;

static int
con_type_check_remove(void *cb_data, void *item1, void *item2)
{
    con_type_check_remover_t *data = cb_data;

    if (strcmp(item1, data->name) == 0) {
	locked_list_remove(con_type_list, item1, item2);
	data->err = 0;
	return LOCKED_LIST_ITER_STOP;
    }
    return LOCKED_LIST_ITER_CONTINUE;
}

int
i_ipmi_unregister_con_type(const char *name, ipmi_con_setup_t *setup)
{
    con_type_check_remover_t data;

    data.err = EINVAL;
    data.name = name;
    locked_list_iterate(con_type_list, con_type_check_remove, &data);
    return data.err;
}

int
ipmi_parse_options(ipmi_open_option_t *option,
		   char               *arg)
{
    if (strcmp(arg, "-noall") == 0) {
	option->option = IPMI_OPEN_OPTION_ALL;
	option->ival = 0;
    } else if (strcmp(arg, "-all") == 0) {
	option->option = IPMI_OPEN_OPTION_ALL;
	option->ival = 1;
    } else if (strcmp(arg, "-nosdrs") == 0) {
	option->option = IPMI_OPEN_OPTION_SDRS;
	option->ival = 0;
    } else if (strcmp(arg, "-sdrs") == 0) {
	option->option = IPMI_OPEN_OPTION_SDRS;
	option->ival = 1;
    } else if (strcmp(arg, "-nofrus") == 0) {
	option->option = IPMI_OPEN_OPTION_FRUS;
	option->ival = 0;
    } else if (strcmp(arg, "-frus") == 0) {
	option->option = IPMI_OPEN_OPTION_FRUS;
	option->ival = 1;
    } else if (strcmp(arg, "-nosel") == 0) {
	option->option = IPMI_OPEN_OPTION_SEL;
	option->ival = 0;
    } else if (strcmp(arg, "-sel") == 0) {
	option->option = IPMI_OPEN_OPTION_SEL;
	option->ival = 1;
    } else if (strcmp(arg, "-noipmbscan") == 0) {
	option->option = IPMI_OPEN_OPTION_IPMB_SCAN;
	option->ival = 0;
    } else if (strcmp(arg, "-ipmbscan") == 0) {
	option->option = IPMI_OPEN_OPTION_IPMB_SCAN;
	option->ival = 1;
    } else if (strcmp(arg, "-nooeminit") == 0) {
	option->option = IPMI_OPEN_OPTION_OEM_INIT;
	option->ival = 0;
    } else if (strcmp(arg, "-oeminit") == 0) {
	option->option = IPMI_OPEN_OPTION_OEM_INIT;
	option->ival = 1;
    } else if (strcmp(arg, "-noseteventrcvr") == 0) {
	option->option = IPMI_OPEN_OPTION_SET_EVENT_RCVR;
	option->ival = 0;
    } else if (strcmp(arg, "-seteventrcvr") == 0) {
	option->option = IPMI_OPEN_OPTION_SET_EVENT_RCVR;
	option->ival = 1;
    } else if (strcmp(arg, "-noactivate") == 0) {
	option->option = IPMI_OPEN_OPTION_ACTIVATE_IF_POSSIBLE;
	option->ival = 0;
    } else if (strcmp(arg, "-activate") == 0) {
	option->option = IPMI_OPEN_OPTION_ACTIVATE_IF_POSSIBLE;
	option->ival = 1;
    } else if (strcmp(arg, "-nosetseltime") == 0) {
	option->option = IPMI_OPEN_OPTION_SET_SEL_TIME;
	option->ival = 0;
    } else if (strcmp(arg, "-setseltime") == 0) {
	option->option = IPMI_OPEN_OPTION_SET_SEL_TIME;
	option->ival = 1;
    } else if (strcmp(arg, "-nolocalonly") == 0) {
	option->option = IPMI_OPEN_OPTION_LOCAL_ONLY;
	option->ival = 0;
    } else if (strcmp(arg, "-localonly") == 0) {
	option->option = IPMI_OPEN_OPTION_LOCAL_ONLY;
	option->ival = 1;
    } else if (strcmp(arg, "-nocache") == 0) {
	option->option = IPMI_OPEN_OPTION_USE_CACHE;
	option->ival = 0;
    } else if (strcmp(arg, "-cache") == 0) {
	option->option = IPMI_OPEN_OPTION_USE_CACHE;
	option->ival = 1;
    } else
	return EINVAL;

    return 0;
}

const char *
ipmi_parse_options_help(void)
{
    return
	"-[no]all - all automatic handling\n"
	"-[no]sdrs - sdr fetching\n"
	"-[no]frus - FRU fetching\n"
	"-[no]sel - SEL fetching\n"
	"-[no]ipmbscan - IPMB bus scanning\n"
	"-[no]oeminit - special OEM processing (like ATCA)\n"
	"-[no]seteventrcvr - setting event receivers\n"
	"-[no]setseltime - setting the SEL clock\n"
	"-[no]activate - connection activation\n"
	"-[no]localonly - Just talk to the local BMC, (ATCA-only, for blades)\n"
        "-[no]cache - use the local cache for SDRs.  On by default."
	"-wait_til_up - wait until the domain is up before returning";
}

/* This is the number of seconds between 1/1/70 (IPMI event date) and
   1/1/98 (ipmi SNMP trap local timestamp). */
#define IPMI_SNMP_DATE_OFFSET 883612800

int
ipmi_handle_snmp_trap_data(const void          *src_addr,
			   unsigned int        src_addr_len,
			   int                 src_addr_type,
			   long                specific,
			   const unsigned char *data,
			   unsigned int        data_len)
{
    int           handled = 0;
    unsigned char pet_ack[12];
    ipmi_msg_t    *msg = NULL;

    if (DEBUG_RAWMSG) {
	ipmi_log(IPMI_LOG_DEBUG_START, "Got SNMP trap from:\n  ");
	dump_hex(src_addr, src_addr_len);
	ipmi_log(IPMI_LOG_DEBUG_CONT, "\n data is:\n  ");
	dump_hex(data, data_len);
	ipmi_log(IPMI_LOG_DEBUG_END, " ");
    }

    if (data_len < 46)
	return 0;

    /* I will take this opportunity to note that the SNMP trap format
       from IPMI is insufficient to actually perform the job.  It does
       not have:
       1 A guaranteed way to correlate the timestamp to the SEL.
       2 A guaranteed way to correlate the record id to the SEL.
       3 The channel or the LUN for the event generator.

       Because of these, there is no guaranteed way to correlate the
       data from the SNMP trap to an SEL event.  This can result in
       duplicate events, which is very bad.  So we currently do not
       deliver the events this way, we pass a NULL in the event
       message to tell the domain code to rescan the SEL for this MC.
       In addition, item 3 above means that you cannot determine which
       sensor issued the event, since the channel and the LUN are
       required to find the sensor.
    */

 /* Until we have some way to get full valid data from the trap, we
    just disable it. */
#if 0
    ipmi_msg_t     tmsg;
    unsigned char  edata[17];
    unsigned long  timestamp;
    int16_t        utc_off;
    unsigned short record_id;
    timestamp = ntohl(*((uint32_t *) (data+18)));
    if (data[27] == 0xff)
	/* Can't handle unspecific event generator */
	return 0;
    if ((data[28] == 0xff) || (data[28] == 0x00))
	/* Can't handle unspecific sensor */
	return 0;
    if (timestamp == 0)
	/* Can't handle unspecified timestamp. */
	return 0;
    utc_off = ntohs(*((uint16_t *) (data+22)));
    if (utc_off == -1)
	/* If unspecified (0xffff), we assume zero (UTC). */
	utc_off = 0;
    timestamp -= utc_off; /* Remove timezone offset */
    timestamp += IPMI_SNMP_DATE_OFFSET; /* Convert to 1/1/70 offset */

    /* We assume the record id is in the sequence # field, since that
       makes the most sense. */
    record_id = ntohs(*((uint16_t *) (data+16)));

    tmsg.netfn = IPMI_APP_NETFN;
    tmsg.cmd = IPMI_READ_EVENT_MSG_BUFFER_CMD;
    tmsg.data = edata;
    tmsg.data_len = 17;
    msg = &tmsg;
    edata[0] = 0;
    edata[1] = record_id & 0xff;
    edata[2] = (record_id >> 8) & 0xff;
    edata[3] = 2; /* record type - system event */
    edata[4] = timestamp & 0xff;
    edata[5] = (timestamp >> 8) & 0xff;
    edata[6] = (timestamp >> 16) & 0xff;
    edata[7] = (timestamp >> 24) & 0xff;
    edata[8] = data[27]; /* Event generator */
    /* FIXME - is there a way to get the LUN? */
    edata[9] = 0; /* Assume channel 0, lun 0 */
    edata[10] = 0x04; /* IPMI 1.5 revision */
    edata[11] = (specific >> 16) & 0xff; /* Sensor type */
    edata[12] = data[28]; /* Sensor number */
    edata[13] = (specific >> 8) & 0xff; /* Event dir/type */
    memcpy(edata+14, data+31, 3); /* Event data 1-3 */
#endif

    pet_ack[0] = data[17]; /* Record id */
    pet_ack[1] = data[16];
    pet_ack[2] = data[21]; /* Timestamp */
    pet_ack[3] = data[20];
    pet_ack[4] = data[19];
    pet_ack[5] = data[18];
    pet_ack[6] = data[25]; /* Event source type */
    pet_ack[7] = data[27]; /* Sensor device */
    pet_ack[8] = data[28]; /* Sensor number */
    memcpy(pet_ack+9, data+31, 3); /* Event data 1-3 */

    if (src_addr_type == IPMI_EXTERN_ADDR_IP)
	handled = ipmi_lan_handle_external_event(src_addr, msg, pet_ack);

    return handled;
}

char *
ipmi_openipmi_version(void)
{
    return OPENIPMI_VERSION;
}

ipmi_msgi_t *
ipmi_alloc_msg_item(void)
{
    ipmi_msgi_t *rv;

    rv = ipmi_mem_alloc(sizeof(ipmi_msgi_t));
    if (!rv)
	return NULL;
    memset(rv, 0, sizeof(*rv));
    rv->msg.data = rv->data;
    return rv;
}

void
ipmi_free_msg_item(ipmi_msgi_t *item)
{
    if (item->msg.data && (item->msg.data != item->data))
	ipmi_free_msg_item_data(item->msg.data);
    ipmi_mem_free(item);
}

void *
ipmi_alloc_msg_item_data(unsigned int size)
{
    return ipmi_mem_alloc(size);
}

void
ipmi_free_msg_item_data(void *data)
{
    ipmi_mem_free(data);
}

void
ipmi_move_msg_item(ipmi_msgi_t *new_item, ipmi_msgi_t *old_item)
{
    if (new_item->msg.data && (new_item->msg.data != new_item->data))
	ipmi_free_msg_item_data(new_item->msg.data);
    new_item->msg = old_item->msg;

    if (!old_item->msg.data) {
	/* Nothing to do */
    } else if (old_item->msg.data != old_item->data) {
	/* Copied the actual data pointer. */
	old_item->msg.data = NULL;
    } else {
	memcpy(new_item->data, old_item->data, old_item->msg.data_len);
	new_item->msg.data = new_item->data;
    }
}

void
ipmi_handle_rsp_item_copyall(ipmi_con_t            *ipmi,
			     ipmi_msgi_t           *rspi,
			     const ipmi_addr_t     *addr,
			     unsigned int          addr_len,
			     const ipmi_msg_t      *msg,
			     ipmi_ll_rsp_handler_t rsp_handler)
{
    int used = IPMI_MSG_ITEM_NOT_USED;

    memcpy(&rspi->addr, addr, addr_len);
    rspi->addr_len = addr_len;
    rspi->msg = *msg;
    memcpy(rspi->data, msg->data, msg->data_len);
    rspi->msg.data = rspi->data;

    /* call the user handler. */
    if (rsp_handler)
	used = rsp_handler(ipmi, rspi);

    if (!used)
	ipmi_free_msg_item(rspi);
}

void
ipmi_handle_rsp_item_copymsg(ipmi_con_t            *ipmi,
			     ipmi_msgi_t           *rspi,
			     const ipmi_msg_t      *msg,
			     ipmi_ll_rsp_handler_t rsp_handler)
{
    int used = IPMI_MSG_ITEM_NOT_USED;

    rspi->msg = *msg;
    memcpy(rspi->data, msg->data, msg->data_len);
    rspi->msg.data = rspi->data;

    /* call the user handler. */
    if (rsp_handler)
	used = rsp_handler(ipmi, rspi);

    if (!used)
	ipmi_free_msg_item(rspi);
}

void
ipmi_handle_rsp_item(ipmi_con_t            *ipmi,
		     ipmi_msgi_t           *rspi,
		     ipmi_ll_rsp_handler_t rsp_handler)
{
    int used = IPMI_MSG_ITEM_NOT_USED;

    /* call the user handler. */
    if (rsp_handler)
	used = rsp_handler(ipmi, rspi);

    if (!used)
	ipmi_free_msg_item(rspi);
}

os_handler_t *
ipmi_alloc_os_handler(void)
{
    os_handler_t *rv = ipmi_mem_alloc(sizeof(*rv));
    if (rv)
	memset(rv, 0, sizeof(*rv));
    return rv;
}

void
ipmi_free_os_handler(os_handler_t *handler)
{
    ipmi_mem_free(handler);
}

