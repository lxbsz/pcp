/*
 * PAPI PMDA
 *
 * Copyright (c) 2014 Red Hat.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <pcp/pmapi.h>
#include <pcp/impl.h>
#include <pcp/pmda.h>
#include "domain.h"
#include <papi.h>
#include <assert.h>
#if defined(HAVE_GRP_H)
#include <grp.h>
#endif
#include <string.h>
#include <time.h>


enum {
    CLUSTER_PAPI = 0,	// hardware event counters
    CLUSTER_CONTROL,	// control variables
    CLUSTER_AVAILABLE,	// available hardware
};

typedef struct {
    char papi_string_code[PAPI_HUGE_STR_LEN]; //same length as the papi 'symbol' or name of the event
    pmID pmid;
    int position; /* >=0 implies actively counting in EventSet, index into values[] */
    time_t metric_enabled; /* >=0: time until user desires this metric; -1 forever. */
    long_long prev_value;
    PAPI_event_info_t info;
} papi_m_user_tuple;

#define METRIC_ENABLED_FOREVER ((time_t)-1)
static __uint32_t auto_enable_time = 120; /* seconds; 0:disabled */
static int auto_enable_afid = -1; /* pmaf(3) identifier for periodic callback */

static papi_m_user_tuple *papi_info;
static unsigned int number_of_events; /* cardinality of papi_info[] */

static int isDSO = 1; /* == 0 if I am a daemon */
static int EventSet = PAPI_NULL;
static long_long *values;
struct uid_tuple {
    int uid_flag; /* uid attribute received. */
    int uid; /* uid received from PCP_ATTR_* */
}; 
static struct uid_tuple *ctxtab;
static int ctxtab_size;
static int number_of_counters; // XXX: collapse into number_of_events
static unsigned int size_of_active_counters; // XXX: eliminate
static __pmnsTree *papi_tree;

static int refresh_metrics(int);
static void auto_enable_expiry_cb(int, void *);

static char helppath[MAXPATHLEN];
static pmdaMetric *metrictab;
static int nummetrics;

static int enable_multiplexing;

static int
permission_check(int context)
{
    if (ctxtab[context].uid_flag && ctxtab[context].uid == 0)
	return 1;
    return 0;
}

static void
expand_papi_info(int size)
{
    if (number_of_events <= size) {
	size_t new_size = (size + 1) * sizeof(papi_m_user_tuple);
	papi_info = realloc(papi_info, new_size);
	if (papi_info == NULL)
	    __pmNoMem("papi_info tuple", new_size, PM_FATAL_ERR);
	while (number_of_events <= size)
	    memset(&papi_info[number_of_events++], 0, sizeof(papi_m_user_tuple));
    }
}

static void
expand_values(int size)  // XXX: collapse into expand_papi_info()
{
    if (size_of_active_counters <= size) {
	size_t new_size = (size + 1) * sizeof(long_long);
	values = realloc(values, new_size);
	if (values == NULL)
	    __pmNoMem("values", new_size, PM_FATAL_ERR);
	while (size_of_active_counters <= size) {
	    memset(&values[size_of_active_counters++], 0, sizeof(long_long));
	    if (pmDebug & DBG_TRACE_APPL0) {
		__pmNotifyErr(LOG_DEBUG, "memsetting to zero, %d counters\n",
				size_of_active_counters);
	    }
	}
    }
}

static void
enlarge_ctxtab(int context)
{
    /* Grow the context table if necessary. */
    if (ctxtab_size /* cardinal */ <= context /* ordinal */) {
        size_t need = (context + 1) * sizeof(struct uid_tuple);
        ctxtab = realloc(ctxtab, need);
        if (ctxtab == NULL)
            __pmNoMem("papi ctx table", need, PM_FATAL_ERR);
        /* Blank out new entries. */
        while (ctxtab_size <= context)
            memset(&ctxtab[ctxtab_size++], 0, sizeof(struct uid_tuple));
    }
}

static void
expand_metric_tab(int size)
{
    size_t need = sizeof(pmdaMetric)*(nummetrics+1);
    metrictab = realloc(metrictab, need);
    if (metrictab == NULL)
	__pmNoMem("metrictab expansion", need, PM_FATAL_ERR);

    metrictab[nummetrics-1].m_desc.pmid = papi_info[size].pmid;
    metrictab[nummetrics-1].m_desc.type = PM_TYPE_64;
    metrictab[nummetrics-1].m_desc.indom = PM_INDOM_NULL;
    metrictab[nummetrics-1].m_desc.sem = PM_SEM_COUNTER;
    metrictab[nummetrics-1].m_desc.units = (pmUnits) PMDA_PMUNITS(0,0,1,0,0,PM_COUNT_ONE);
}

static int
check_papi_state()
{
    int state = 0;
    int sts;

    sts = PAPI_state(EventSet, &state);
    if (sts != PAPI_OK)
	return sts;
    return state;
}

static void
papi_endContextCallBack(int context)
{
    if (pmDebug & DBG_TRACE_APPL0)
	__pmNotifyErr(LOG_DEBUG, "end context %d received\n", context);

    /* ensure clients re-using this slot re-authenticate */
    if (context >= 0 && context < ctxtab_size)
	ctxtab[context].uid_flag = 0;
}

static int
papi_fetchCallBack(pmdaMetric *mdesc, unsigned int inst, pmAtomValue *atom)
{
    __pmID_int *idp = (__pmID_int *)&(mdesc->m_desc.pmid);
    int sts;
    int i;
    int state;
    char local_string[32];
    static char status_string[4096];
    int first_metric = 0;
    time_t now;

    now = time(NULL);
    sts = check_papi_state();
    if (sts & PAPI_RUNNING) {
	sts = PAPI_read(EventSet, values);
	if (sts != PAPI_OK) {
	    __pmNotifyErr(LOG_ERR, "PAPI_read: %s\n", PAPI_strerror(sts));
	    return PM_ERR_VALUE;
	}
    }

    switch (idp->cluster) {
    case CLUSTER_PAPI:
	if (idp->item >= 0 && idp->item <= number_of_events) {
	    // the 'case' && 'idp->item' value we get is the pmns_position
	    if (papi_info[idp->item].position >= 0) {
		atom->ll = papi_info[idp->item].prev_value + values[papi_info[idp->item].position];
                // if previously auto-enabled, extend the timeout
                if (papi_info[idp->item].metric_enabled != METRIC_ENABLED_FOREVER &&
                    auto_enable_time)
                    papi_info[idp->item].metric_enabled = now + auto_enable_time;
		return PMDA_FETCH_STATIC;
	    }
	    else {
                if (auto_enable_time) {
                    // auto-enable this metric for a while
                    papi_info[idp->item].metric_enabled = now + auto_enable_time;
                    sts = refresh_metrics(0);
                    if (sts < 0)
                        return sts;
                }
		return PMDA_FETCH_NOVALUES;
            }
	}

	return PM_ERR_PMID;

    case CLUSTER_CONTROL:
	switch (idp->item) {
	case 0:
	    atom->cp = ""; /* papi.control.enable */
	    return PMDA_FETCH_STATIC;

	case 1:
	    /* papi.control.reset */
	    atom->cp = "";
	    return PMDA_FETCH_STATIC;

	case 2:
	    /* papi.control.disable */
	    atom->cp = "";
	    if ((sts = check_papi_state()) & PAPI_RUNNING)
		return PMDA_FETCH_STATIC;
	    return 0;

	case 3:
	    sts = PAPI_state(EventSet, &state);
	    if (sts != PAPI_OK)
		return PM_ERR_VALUE;
	    strcpy(status_string, "Papi ");
	    if(state & PAPI_STOPPED)
		strcat(status_string, "is stopped, ");
	    if (state & PAPI_RUNNING)
		strcat(status_string, "is running, ");
	    if (state & PAPI_PAUSED)
		strcat(status_string,"is paused, ");
	    if (state & PAPI_NOT_INIT)
		strcat(status_string, "is defined but not initialized, ");
	    if (state & PAPI_OVERFLOWING)
		strcat(status_string, "has overflowing enabled, ");
	    if (state & PAPI_PROFILING)
		strcat(status_string, "eventset has profiling enabled, ");
	    if (state & PAPI_MULTIPLEXING)
		strcat(status_string,"has multiplexing enabled, ");
	    if (state & PAPI_ATTACHED)
	        strcat(status_string, "is attached to another process/thread, ");
	    if (state & PAPI_CPU_ATTACHED)
		strcat(status_string, "is attached to a specific CPU, ");

            first_metric = 1;
	    for(i = 0; i < number_of_events; i++){
		if(papi_info[i].position < 0)
                    continue;
                sprintf(local_string, "%s%s(%d): %lld",
                        (first_metric ? "" : ", "),
                        papi_info[i].papi_string_code,
                        (papi_info[i].metric_enabled == METRIC_ENABLED_FOREVER ? -1 :
                         (int)(papi_info[i].metric_enabled - now)), // number of seconds left
                        (papi_info[i].prev_value + values[papi_info[i].position]));
                first_metric = 0;
                if ((strlen(status_string) + strlen(local_string) + 1) < sizeof(status_string))
                    strcat(status_string, local_string);
	    }
	    atom->cp = status_string;
	    return PMDA_FETCH_STATIC;

	case 4:
	    /* papi.control.auto_enable */
	    atom->ul = auto_enable_time;
            return PMDA_FETCH_STATIC;

	case 5:
	    /* papi.control.multiplex */
	    atom->ul = enable_multiplexing;
            return PMDA_FETCH_STATIC;

	default:
	    return PM_ERR_PMID;
	}
	break;

    case CLUSTER_AVAILABLE:
	if (idp->item == 0) {
	    atom->ul = number_of_counters; /* papi.available.num_counters */
	    return PMDA_FETCH_STATIC;
	}
	return PM_ERR_PMID;

    default:
	return PM_ERR_PMID;
    }

    return PMDA_FETCH_NOVALUES;
}

static int
papi_fetch(int numpmid, pmID pmidlist[], pmResult **resp, pmdaExt *pmda)
{
    int sts;

    __pmAFblock();
    auto_enable_expiry_cb(0, NULL); // run auto-expiry
    if (permission_check(pmda->e_context))
	sts = pmdaFetch(numpmid, pmidlist, resp, pmda);
    else
        sts = PM_ERR_PERMISSION;
    __pmAFunblock();
    return sts;
}

static void
handle_papi_error(int error, int logged)
{
    if (logged || (pmDebug & DBG_TRACE_APPL0))
	__pmNotifyErr(LOG_ERR, "Papi error: %s\n", PAPI_strerror(error));
}

/*
 * Iterate across all papi_info[].  Some of them are presumed to have
 * changed metric_enabled states (we don't care which way).  Shut down
 * the PAPI eventset and collect the then-current values; create a new
 * PAPI eventset with the survivors; restart.  (These steps are
 * necessary because PAPI doesn't let one modify a PAPI_RUNNING
 * EventSet, nor (due to a bug) subtract even from a PAPI_STOPPED one.)
 *
 * "log" parameter indicates whether errors are to be recorded in the
 * papi.log file, or if there is a calling process we can send 'em to
 * (in which case, they are not logged).
 */
static int
refresh_metrics(int log)
{
    int sts = 0;
    int state = 0;
    int i;
    int number_of_active_counters = 0;
    time_t now;

    now = time(NULL);

    /* Shut down, save previous state. */
    state = check_papi_state();
    if (state & PAPI_RUNNING) {
	sts = PAPI_stop(EventSet, values);
        if (sts != PAPI_OK) {
            /* futile to continue */
            handle_papi_error(sts, log);
            return PM_ERR_VALUE;
        }

        /* Save previous values */ 
        for (i = 0; i < number_of_events; i++){
            if(papi_info[i].position >= 0) {
                papi_info[i].prev_value += values[papi_info[i].position];
                papi_info[i].position = -1;
            }
        }

        /* Clean up eventset */
        sts = PAPI_cleanup_eventset(EventSet);
        if (sts != PAPI_OK) {
            handle_papi_error(sts, log);
            /* FALLTHROUGH */
        }
        
        sts = PAPI_destroy_eventset(&EventSet); /* sets EventSet=NULL */
        if (sts != PAPI_OK) {
            handle_papi_error(sts, log);
            /* FALLTHROUGH */
        }
    }

    /* Initialize new EventSet */
    EventSet = PAPI_NULL;
    if ((sts = PAPI_create_eventset(&EventSet)) != PAPI_OK) {
	handle_papi_error(sts, log);
	return PM_ERR_GENERIC;
    }
    if ((sts = PAPI_assign_eventset_component(EventSet, 0 /*CPU*/)) != PAPI_OK) {
	handle_papi_error(sts, log);
	return PM_ERR_GENERIC;
    }
    if (enable_multiplexing && (sts = PAPI_set_multiplex(EventSet)) != PAPI_OK) {
	handle_papi_error(sts, log);
        /* not fatal - FALLTHROUGH */
    }

    /* Add all survivor events to new EventSet */
    number_of_active_counters = 0;
    for (i = 0; i < number_of_events; i++) {
	if (papi_info[i].metric_enabled == METRIC_ENABLED_FOREVER ||
            papi_info[i].metric_enabled >= now) {
	    sts = PAPI_add_event(EventSet, papi_info[i].info.event_code);
	    if (sts != PAPI_OK) {
                if (pmDebug & DBG_TRACE_APPL0) {
                    char eventname[PAPI_MAX_STR_LEN];
                    PAPI_event_code_to_name(papi_info[i].info.event_code, eventname);
                    __pmNotifyErr(LOG_DEBUG, "Unable to add: %s due to error: %s\n",
                                  eventname, PAPI_strerror(sts));
                }
		handle_papi_error(sts, log);
                /*
                 * This is where we'd see if a requested counter was
                 * "one too many".  We must leave a note for the
                 * function to return an error, but must continue (so
                 * that reactivating other counters is still
                 * attempted).  
                 */
                sts = PM_ERR_VALUE;
                continue;
	    }
	    papi_info[i].position = number_of_active_counters++;
	}
    }

    /* Restart counting. */
    if (number_of_active_counters > 0) {
	sts = PAPI_start(EventSet);
	if (sts != PAPI_OK) {
	    handle_papi_error(sts, log);
	    return PM_ERR_VALUE;
	}
    }
    return 0;
}

/* The pmaf(3)-based callback for auto-enabled metric expiry. */
static void
auto_enable_expiry_cb(int ignored1, void *ignored2)
{
    int i;
    time_t now;
    int must_refresh;

    /* All we need to do here is to scan through all the enabled
     * metrics, and if some have just expired, call refresh_metrics().
     * We don't want to call it unconditionally, since it's disruptive.
     */
    now = time(NULL);
    must_refresh = 0;
    for (i = 0; i < number_of_events; i++) {
	if (papi_info[i].position >= 0 && // enabled at papi level
	    papi_info[i].metric_enabled != METRIC_ENABLED_FOREVER &&
	    papi_info[i].metric_enabled < now) // just expired
	    must_refresh = 1;
    }
    if (must_refresh)
	refresh_metrics(1);
}

static int
papi_setup_auto_af(void)
{
    if (auto_enable_afid >= 0)
	__pmAFunregister(auto_enable_afid);
    auto_enable_afid = -1;

    if (auto_enable_time) {
	struct timeval t;

	t.tv_sec = (time_t) auto_enable_time;
	t.tv_usec = 0;
	auto_enable_afid = __pmAFregister(&t, NULL, auto_enable_expiry_cb);
	return auto_enable_afid < 0 ? auto_enable_afid : 0;
    }
    return 0;
}

static int
papi_store(pmResult *result, pmdaExt *pmda)
{
    int sts;
    int i, j;
    const char *delim = " ,";
    char *substring;

    if (!permission_check(pmda->e_context))
	return PM_ERR_PERMISSION;
    for (i = 0; i < result->numpmid; i++) {
	pmValueSet *vsp = result->vset[i];
	__pmID_int *idp = (__pmID_int *)&(vsp->pmid);
	pmAtomValue av;

	if (idp->cluster != CLUSTER_CONTROL)
	    return PM_ERR_PERMISSION;

	switch (idp->item) {
	case 0: //papi.enable
	case 2: //papi.disable // NB: almost identical handling!
	    if ((sts = pmExtractValue(vsp->valfmt, &vsp->vlist[0],
				PM_TYPE_STRING, &av, PM_TYPE_STRING)) < 0)
		return sts;
	    substring = strtok(av.cp, delim);
	    while (substring != NULL) {
		for (j = 0; j < number_of_events; j++) {
		    if (!strcmp(substring, papi_info[j].papi_string_code)) {
			papi_info[j].metric_enabled =
			    (idp->item == 0 /* papi.enable */) ? METRIC_ENABLED_FOREVER : 0;
			break;
		    }
		}
		if (j == number_of_events) {
		    if (pmDebug & DBG_TRACE_APPL0)
			__pmNotifyErr(LOG_DEBUG, "metric name %s does not match any known metrics\n", substring);
		    sts = 1;
		    /* NB: continue for other event names that may succeed */
		}
		substring = strtok(NULL, delim);
	    }
            if (sts) { /* any unknown metric name encountered? */
		sts = refresh_metrics(0); /* still enable those that we can */
		if (sts == 0)
		    sts = PM_ERR_CONV; /* but return overall error */
	    } else {
		sts = refresh_metrics(0);
	    }
	    return sts;

	case 1: //papi.reset
            for (j = 0; j < number_of_events; j++)
                papi_info[j].metric_enabled = 0;
            return refresh_metrics(0);

	case 4: //papi.control.auto_enable
	    if ((sts = pmExtractValue(vsp->valfmt, &vsp->vlist[0],
				 PM_TYPE_U32, &av, PM_TYPE_U32)) < 0)
		return sts;
            auto_enable_time = av.ul;
            return papi_setup_auto_af();

	case 5: //papi.control.multiplex
	    if ((sts = pmExtractValue(vsp->valfmt, &vsp->vlist[0],
				 PM_TYPE_U32, &av, PM_TYPE_U32)) < 0)
		return sts;
	    enable_multiplexing = av.ul;
	    return refresh_metrics(0);

	default:
	    return PM_ERR_PMID;
	}
    }
    return 0;
}

static int
papi_desc(pmID pmid, pmDesc *desc, pmdaExt *pmda)
{
    return pmdaDesc(pmid, desc, pmda);
}

static int
papi_text(int ident, int type, char **buffer, pmdaExt *ep)
{
    __pmID_int *pmidp = (__pmID_int *)&ident;

    /* no indoms - we only deal with metric help text */
    if ((type & PM_TEXT_PMID) != PM_TEXT_PMID)
	return PM_ERR_TEXT;

    if (pmidp->cluster == CLUSTER_PAPI) {
	if (pmidp->item < number_of_events) {
	    if (type & PM_TEXT_ONELINE)
		*buffer = papi_info[pmidp->item].info.short_descr;
	    else
		*buffer = papi_info[pmidp->item].info.long_descr;
	    return 0;
	}
	return pmdaText(ident, type, buffer, ep);
    }
    return pmdaText(ident, type, buffer, ep);
}

static int
papi_name_lookup(const char *name, pmID *pmid, pmdaExt *pmda)
{
    return pmdaTreePMID(papi_tree, name, pmid);
}

static int
papi_children(const char *name, int traverse, char ***offspring, int **status, pmdaExt *pmda)
{
    return pmdaTreeChildren(papi_tree, name, traverse, offspring, status);
}

static int
papi_internal_init(pmdaInterface *dp)
{
    int ec;
    int sts;
    PAPI_event_info_t info;
    char entry[PAPI_HUGE_STR_LEN]; // the length papi uses for the symbol name
    unsigned int i = 0;
    pmID pmid;

    if ((sts = __pmNewPMNS(&papi_tree)) < 0) {
	__pmNotifyErr(LOG_ERR, "%s failed to create dynamic papi pmns: %s\n",
		      pmProgname, pmErrStr(sts));
	papi_tree = NULL;
	return PM_ERR_GENERIC;
    }

    number_of_counters = PAPI_num_counters();
    if (number_of_counters < 0) {
	__pmNotifyErr(LOG_ERR, "hardware does not support performance counters\n");
	return PM_ERR_APPVERSION;
    }

    sts = PAPI_library_init(PAPI_VER_CURRENT);
    if (sts != PAPI_VER_CURRENT) {
	__pmNotifyErr(LOG_ERR, "PAPI_library_init error (%d)\n", sts);
	return PM_ERR_GENERIC;
    }

    ec = PAPI_PRESET_MASK;
    PAPI_enum_event(&ec, PAPI_ENUM_FIRST);
    do {
	if (PAPI_get_event_info(ec, &info) == PAPI_OK) {
	    if (info.count && PAPI_PRESET_ENUM_AVAIL) {
		expand_papi_info(i);
		memcpy(&papi_info[i].info, &info, sizeof(PAPI_event_info_t));
		memcpy(&papi_info[i].papi_string_code, info.symbol + 5, strlen(info.symbol)-5);
		snprintf(entry, sizeof(entry),"papi.system.%s", papi_info[i].papi_string_code);
		pmid = pmid_build(dp->domain, CLUSTER_PAPI, i);
		papi_info[i].pmid = pmid;
		__pmAddPMNSNode(papi_tree, pmid, entry);
		memset(&entry[0], 0, sizeof(entry));
		papi_info[i].position = -1;
		papi_info[i].metric_enabled = 0;
		nummetrics++;
		expand_metric_tab(i);
		expand_values(i);
		i++;
	    }
	}
    } while(PAPI_enum_event(&ec, 0) == PAPI_OK);
    pmdaTreeRebuildHash(papi_tree, number_of_events);

    /* Set one-time settings for all future EventSets. */
    if ((sts = PAPI_set_domain(PAPI_DOM_ALL)) != PAPI_OK) {
	handle_papi_error(sts, 0);
	return PM_ERR_GENERIC;
    }
    if ((sts = PAPI_multiplex_init()) != PAPI_OK) {
	handle_papi_error(sts, 0);
	return PM_ERR_GENERIC;
    }

    sts = refresh_metrics(0);
    if (sts != PAPI_OK)
	return PM_ERR_GENERIC;
    return 0;
}

/* use documented in pmdaAttribute(3) */
static int
papi_contextAttributeCallBack(int context, int attr,
			      const char *value, int length, pmdaExt *pmda)
{
    int id = -1;

    enlarge_ctxtab(context);
    assert(ctxtab != NULL && context < ctxtab_size);

    if (attr != PCP_ATTR_USERID)
	return 0;

    ctxtab[context].uid_flag = 1;
    ctxtab[context].uid = id = atoi(value);
    if (id != 0) {
	if (pmDebug & DBG_TRACE_AUTH)
	    __pmNotifyErr(LOG_DEBUG, "access denied attr=%d id=%d\n", attr, id);
	return PM_ERR_PERMISSION;
    }

    if (pmDebug & DBG_TRACE_AUTH)
	__pmNotifyErr(LOG_DEBUG, "access granted attr=%d id=%d\n", attr, id);
    return 0;
}

void
__PMDA_INIT_CALL
papi_init(pmdaInterface *dp)
{
    int i;
    int sts;

    enable_multiplexing = 1;
    nummetrics = 7;
    metrictab = malloc(nummetrics*sizeof(pmdaMetric));
    if (metrictab == NULL)
	__pmNoMem("initial metrictab allocation", (nummetrics*sizeof(pmdaMetric)), PM_FATAL_ERR);
    for(i = 0; i < nummetrics; i++) {
	switch (i) {
	case 4: // papi.control.auto_enable
	case 5:
	    metrictab[i].m_desc.pmid = pmid_build(dp->domain, CLUSTER_CONTROL, i);
	    metrictab[i].m_desc.type = PM_TYPE_U32;
	    metrictab[i].m_desc.indom = PM_INDOM_NULL;
	    metrictab[i].m_desc.sem = PM_SEM_DISCRETE;
	    metrictab[i].m_desc.units = (pmUnits) PMDA_PMUNITS(0,1,0,0,PM_TIME_SEC,0);
	    break;
	case 6: // papi.available.num_counters
	    metrictab[i].m_desc.pmid = pmid_build(dp->domain, CLUSTER_AVAILABLE, 0);
	    metrictab[i].m_desc.type = PM_TYPE_U32;
	    metrictab[i].m_desc.indom = PM_INDOM_NULL;
	    metrictab[i].m_desc.sem = PM_SEM_DISCRETE;
	    metrictab[i].m_desc.units = (pmUnits) PMDA_PMUNITS(0,0,1,0,0,PM_COUNT_ONE);
	    break;
	default: // papi.control.{enable,reset,disable,status}
	    metrictab[i].m_desc.pmid = pmid_build(dp->domain, CLUSTER_CONTROL, i);
	    metrictab[i].m_desc.type = PM_TYPE_STRING;
	    metrictab[i].m_desc.indom = PM_INDOM_NULL;
	    metrictab[i].m_desc.sem = PM_SEM_INSTANT;
	    metrictab[i].m_desc.units = (pmUnits) PMDA_PMUNITS(0,0,0,0,0,0);
	    break;
	}
    }

    if (isDSO) {
	int	sep = __pmPathSeparator();

	snprintf(helppath, sizeof(helppath), "%s%c" "papi" "%c" "help",
		 pmGetConfig("PCP_PMDAS_DIR"), sep, sep);
	pmdaDSO(dp, PMDA_INTERFACE_6, "papi DSO", helppath);
    }

    if (dp->status != 0)
	return;

    dp->comm.flags |= PDU_FLAG_AUTH;

    if ((sts = papi_internal_init(dp)) < 0) {
	__pmNotifyErr(LOG_ERR, "papi_internal_init: %s\n", pmErrStr(sts));
	dp->status = PM_ERR_GENERIC;
	return;
    }

    if ((sts = papi_setup_auto_af()) < 0) {
	__pmNotifyErr(LOG_ERR, "papi_setup_auto_af: %s\n", pmErrStr(sts));
	dp->status = PM_ERR_GENERIC;
	return;
    }

    dp->version.six.fetch = papi_fetch;
    dp->version.six.store = papi_store;
    dp->version.six.attribute = papi_contextAttributeCallBack;
    dp->version.six.desc = papi_desc;
    dp->version.any.text = papi_text;
    dp->version.four.pmid = papi_name_lookup;
    dp->version.four.children = papi_children;
    pmdaSetFetchCallBack(dp, papi_fetchCallBack);
    pmdaSetEndContextCallBack(dp, papi_endContextCallBack);
    pmdaInit(dp, NULL, 0, metrictab, nummetrics);
}

static pmLongOptions longopts[] = {
    PMDA_OPTIONS_HEADER("Options"),
    PMOPT_DEBUG,
    PMDAOPT_DOMAIN,
    PMDAOPT_LOGFILE,
    PMOPT_HELP,
    PMDA_OPTIONS_END
};

static pmdaOptions opts = {
    .short_options = "D:d:l:?",
    .long_options = longopts,
};

/*
 * Set up agent if running as daemon.
 */
int
main(int argc, char **argv)
{
    int sep = __pmPathSeparator();
    pmdaInterface dispatch;

    isDSO = 0;
    __pmSetProgname(argv[0]);

    snprintf(helppath, sizeof(helppath), "%s%c" "papi" "%c" "help",
	     pmGetConfig("PCP_PMDAS_DIR"), sep, sep);
    pmdaDaemon(&dispatch, PMDA_INTERFACE_6, pmProgname, PAPI, "papi.log", helppath);    
    pmdaGetOptions(argc, argv, &opts, &dispatch);
    if (opts.errors) {
	pmdaUsageMessage(&opts);
	exit(1);
    }
 
    pmdaOpenLog(&dispatch);
    papi_init(&dispatch);
    pmdaConnect(&dispatch);
    pmdaMain(&dispatch);

    free(ctxtab);
    free(papi_info);
    free(values);
    free(metrictab);

    exit(0);
}
