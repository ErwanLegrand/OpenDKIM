/*
**  Copyright (c) 2011, The OpenDKIM Project.  All rights reserved.
*/

#ifndef lint
static char repute_c_id[] = "$Id$";
#endif /* ! lint */

/* system includes */
#include <sys/param.h>
#include <sys/types.h>
#include <pthread.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

/* libxml includes */
#include <libxml/parser.h>
#include <libxml/tree.h>

/* libcurl includes */
#include <curl/curl.h>

/* libut includes */
#include <ut.h>

/* librepute includes */
#include "repute.h"

/* limits */
#define	REPUTE_BUFBASE	1024
#define	REPUTE_URL	1024

/* data types */
struct repute_io
{
	CURL *			repute_curl;
	size_t			repute_alloc;
	size_t			repute_offset;
	char *			repute_buf;
	struct repute_io *	repute_next;
};

struct repute_handle
{
	pthread_mutex_t		rep_lock;
	struct repute_io *	rep_ios;
	const char *		rep_server;
	char			rep_uritemp[REPUTE_URL + 1];
	char			rep_error[REPUTE_BUFBASE + 1];
};

struct repute_lookup
{
	int			rt_code;
	const char *		rt_name;
};

/* lookup tables */
struct repute_lookup repute_lookup_elements[] =
{
	{ REPUTE_XML_CODE_ASSERTION,	REPUTE_XML_ASSERTION },
	{ REPUTE_XML_CODE_EXTENSION,	REPUTE_XML_EXTENSION },
	{ REPUTE_XML_CODE_RATED,	REPUTE_XML_RATED },
	{ REPUTE_XML_CODE_RATER,	REPUTE_XML_RATER },
	{ REPUTE_XML_CODE_RATER_AUTH,	REPUTE_XML_RATER_AUTH },
	{ REPUTE_XML_CODE_RATING,	REPUTE_XML_RATING },
	{ REPUTE_XML_CODE_SAMPLE_SIZE,	REPUTE_XML_SAMPLE_SIZE },
	{ REPUTE_XML_CODE_UPDATED,	REPUTE_XML_UPDATED },
	{ REPUTE_XML_CODE_UNKNOWN,	NULL }
};

/*
**  REPUTE_LIBXML2_ERRHANDLER -- error handler function provided to libxml2
**
**  Parameters:
**  	ctx -- a "parsing" context (generally a FILE *)
**  	fmt -- message format
**  	... -- variable arguments
**
**  Return value:
** 	None.
**
**  Notes:
**  	Oddly, libxml2 writes errors to stderr by default without a provided
**  	handler function.  We check for errors in other ways and this
**  	program typically runs as a daemon, so we'll suppress that by
**  	providing an error handler that does nothing.
*/

static void
repute_libxml2_errhandler(void *ctx, const char *fmt, ...)
{
	return;
}

/*
**  REPUTE_CURL_WRITEDATA -- callback for libcurl to deliver data
**
**  Parameters:
**  	ptr -- pointer to the retrieved data
**  	size -- unit size
**  	nmemb -- unit count
**  	userdata -- opaque userdata (points to a repute_io structure)
**
**  Return value:
**  	Number of bytes taken in.  If different from "size", libcurl reports
**  	an error.
*/

static size_t
repute_curl_writedata(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	size_t need;
	struct repute_io *io;

	io = userdata;

	need = size * nmemb;

	if (io->repute_buf == NULL)
	{
		io->repute_alloc = MAX(REPUTE_BUFBASE, need);
		io->repute_buf = malloc(io->repute_alloc);
		if (io->repute_buf == NULL)
			return 0;
		memset(io->repute_buf, '\0', io->repute_alloc);
	}
	else if (io->repute_offset + need < io->repute_alloc)
	{
		size_t newsize;
		char *newbuf;

		newsize = MAX(io->repute_alloc * 2, io->repute_alloc + need);
		newbuf = realloc(io->repute_buf, newsize);
		if (newbuf == NULL)
		{
			return 0;
		}
		else
		{
			memset(newbuf + io->repute_offset, '\0',
			       newsize - io->repute_offset);
		}
		io->repute_buf = newbuf;
		io->repute_alloc = newsize;
	}

	memcpy(io->repute_buf + io->repute_offset, ptr, need);

	io->repute_offset += need;

	return need;
}

/*
**  REPUTE_NAME_TO_CODE -- look up a name in a table
**
**  Parameters:
**  	tbl -- table to search
**  	name -- name to find
**
**  Return value:
**  	Matching code.
*/

static int
repute_name_to_code(struct repute_lookup *tbl, const char *name)
{
	int c;

	assert(tbl != NULL);
	assert(name != NULL);

	for (c = 0; ; c++)
	{
		if (tbl[c].rt_name == NULL ||
		    strcasecmp(name, tbl[c].rt_name) == 0)
			return tbl[c].rt_code;
	}

	return -1;
}

/*
**  REPUTE_PARSE -- parse a REPUTE message
**
**  Parameters:
**  	buf -- buffer containing a REPUTE reply
**  	rep -- returned reputation
**  	conf -- confidence
**  	sample -- sample size
**
**  Return value:
**  	A REPUTE_STAT_* constant.
*/

static REPUTE_STAT
repute_parse(const char *buf, size_t buflen, float *rep, float *conf,
             unsigned long *sample, time_t *when)
{
	_Bool found_dkim = FALSE;
	_Bool found_spam = FALSE;
	int code;
	float conftmp;
	float reptmp;
	unsigned long sampletmp;
	time_t whentmp;
	char *p;
	const char *start;
	xmlDocPtr doc = NULL;
	xmlNode *node = NULL;
	xmlNode *reputon = NULL;

	assert(buf != NULL);
	assert(rep != NULL);

	xmlSetGenericErrorFunc(NULL, repute_libxml2_errhandler);

	/* skip any header found */
	/* XXX -- this should verify a desirable Content-Type */
	for (start = buf; *start != '\0'; start++)
	{
		if (*start == '\n' && *(start + 1) == '\n')
		{
			buflen = buflen - (start - buf + 2);
			buf = start + 2;
			break;
		}
		else if (*start == '\r' &&
		         *(start + 1) == '\n' &&
		         *(start + 2) == '\r' &&
		         *(start + 3) == '\n')
		{
			buflen = buflen - (start - buf + 4);
			buf = start + 4;
			break;
		}
	}

	doc = xmlParseMemory(buf, buflen);
	if (doc == NULL)
		return REPUTE_STAT_PARSE;

	node = xmlDocGetRootElement(doc);
	if (node == NULL)
	{
		xmlFreeDoc(doc);
		return REPUTE_STAT_PARSE;
	}

	/* confirm root's name */
	if (node->name == NULL ||
	    strcasecmp(node->name, REPUTE_NAME_REPUTATION) != 0 ||
	    node->children == NULL)
	{
		xmlFreeDoc(doc);
		return REPUTE_STAT_PARSE;
	}

	/* iterate through nodes looking for a reputon */
	for (node = node->children; node != NULL; node = node->next)
	{
		/* skip unnamed things or things that aren't reputons */
		if (node->name == NULL ||
		    node->type != XML_ELEMENT_NODE ||
		    strcasecmp(node->name, REPUTE_NAME_REPUTON) != 0 ||
		    node->children == NULL)
			continue;

		found_dkim = FALSE;
		found_spam = FALSE;
		conftmp = 0.;
		reptmp = 0.;
		sampletmp = 0L;
		whentmp = 0;

		for (reputon = node->children;
		     reputon != NULL;
		     reputon = reputon->next)
		{
			/* look for the reputon */
			if (reputon->name == NULL ||
			    reputon->type != XML_ELEMENT_NODE ||
			    reputon->children == NULL ||
			    reputon->children->content == NULL)
				continue;

			/* skip unknown names */
			code = repute_name_to_code(repute_lookup_elements,
			                           reputon->name);
			if (code == -1)
				continue;

			switch (code)
			{
			  case REPUTE_XML_CODE_RATER:
				/*
				**  We assume for now that we got an answer
				**  from the same place we asked.
				*/

				break;

			  case REPUTE_XML_CODE_RATER_AUTH:
				conftmp = strtof(reputon->children->content,
				                 &p);
				if (*p != '\0' || conftmp < 0 || conftmp > 1)
					continue;

			  case REPUTE_XML_CODE_ASSERTION:
				if (strcasecmp(reputon->children->content,
				               REPUTE_ASSERT_SENDS_SPAM) == 0)
					found_spam = TRUE;
				break;

			  case REPUTE_XML_CODE_EXTENSION:
				if (strcasecmp(reputon->children->content,
				               REPUTE_EXT_ID_DKIM) == 0)
					found_dkim = TRUE;
				break;

			  case REPUTE_XML_CODE_RATED:
				/*
				**  We assume for now that we got an answer
				**  to the right question.
				*/

				break;

			  case REPUTE_XML_CODE_RATING:
				reptmp = strtof(reputon->children->content,
				                &p);
				if (*p != '\0' || reptmp < -1 || reptmp > 1)
					continue;
				break;

			  case REPUTE_XML_CODE_SAMPLE_SIZE:
				errno = 0;
				sampletmp = strtoul(reputon->children->content,
				                    &p, 10);
				if (errno != 0)
					continue;
				break;

			  case REPUTE_XML_CODE_UPDATED:
				errno = 0;
				whentmp = strtoul(reputon->children->content,
				                  &p, 10);
				if (errno != 0)
					continue;
				break;

			  default:
				break;
			}
		}

		if (found_dkim && found_spam)
		{
			*rep = reptmp;
			if (conf != NULL)
				*conf = conftmp;
			if (sample != NULL)
				*sample = sampletmp;
			if (when != NULL)
				*when = whentmp;

			break;
		}
	}

	xmlFreeDoc(doc);
	return REPUTE_STAT_OK;
}

/*
**  REPUTE_GET_IO -- get or create an I/O handle
**
**  Parameters:
**  	rep -- REPUTE handle
**
**  Return value:
**  	An I/O handle if one could be either recycled or created, or NULL
**  	on failure.
*/

static struct repute_io *
repute_get_io(REPUTE rep)
{
	assert(rep != NULL);

	struct repute_io *rio = NULL;

	pthread_mutex_lock(&rep->rep_lock);

	if (rep->rep_ios != NULL)
	{
		rio = rep->rep_ios;

		rep->rep_ios = rep->rep_ios->repute_next;

		rio->repute_offset = 0;
	}
	else
	{
		rio = malloc(sizeof *rio);
		if (rio != NULL)
		{
			rio->repute_alloc = 0;
			rio->repute_offset = 0;
			rio->repute_buf = NULL;
			rio->repute_next = NULL;

			rio->repute_curl = curl_easy_init();
			if (rio->repute_curl == NULL)
			{
				free(rio);
				rio = NULL;
			}
			else
			{
				int status;

				status = curl_easy_setopt(rio->repute_curl,
				                          CURLOPT_WRITEFUNCTION,
		                                          repute_curl_writedata);
				if (status != CURLE_OK)
				{
					free(rio);
					rio = NULL;
				}
			}
		}
	}

	pthread_mutex_unlock(&rep->rep_lock);

	return rio;
}

/*
**  REPUTE_PUT_IO -- recycle an I/O handle
**
**  Parameters:
**  	rep -- REPUTE handle
**  	rio -- REPUTE I/O handle to be recycled
**
**  Return value:
**  	None.
*/

static void
repute_put_io(REPUTE rep, struct repute_io *rio)
{
	assert(rep != NULL);
	assert(rio != NULL);

	pthread_mutex_lock(&rep->rep_lock);

	rio->repute_next = rep->rep_ios;
	rep->rep_ios = rio;

	pthread_mutex_unlock(&rep->rep_lock);
}

/*
**  REPUTE_DOQUERY -- execute a query
**
**  Parameters:
**
**  Return value:
**  	A REPUTE_STAT_* constant.
*/

static REPUTE_STAT
repute_doquery(struct repute_io *rio, const char *url)
{
	CURLcode cstatus;
	long rcode;

	assert(rio != NULL);
	assert(url != NULL);

	cstatus = curl_easy_setopt(rio->repute_curl, CURLOPT_WRITEDATA, rio);
	if (cstatus != CURLE_OK)
		return REPUTE_STAT_INTERNAL;

	cstatus = curl_easy_setopt(rio->repute_curl, CURLOPT_URL, url);
	if (cstatus != CURLE_OK)
		return REPUTE_STAT_INTERNAL;

	cstatus = curl_easy_perform(rio->repute_curl);
	if (cstatus != CURLE_OK)
		return REPUTE_STAT_QUERY;

	cstatus = curl_easy_getinfo(rio->repute_curl, CURLINFO_RESPONSE_CODE,
	                            &rcode);
	if (rcode != 200)
		return REPUTE_STAT_QUERY;

	return REPUTE_STAT_OK;
}

/*
**  REPUTE_GET_TEMPLATE -- retrieve a URI template for a service
**
**  Parameters:
**  	rep -- REPUTE handle
**  	buf -- buffer into which to write the retrieved template
**  	buflen -- bytes available at "buf"
**
**  Return value:
**  	A REPUTE_STAT_* constant.
*/

static int
repute_get_template(REPUTE rep)
{
	int out;
	int cstatus;
	long rcode;
	struct repute_io *rio;
	URITEMP ut;
	char url[REPUTE_BUFBASE + 1];

	assert(rep != NULL);

	ut = ut_init();
	if (ut == NULL)
		return REPUTE_STAT_INTERNAL;

	if (ut_keyvalue(ut, UT_KEYTYPE_STRING,
	                "scheme", REPUTE_URI_SCHEME) != 0 ||
	    ut_keyvalue(ut, UT_KEYTYPE_STRING,
	                "service", (void *) rep->rep_server) != 0 ||
	    ut_keyvalue(ut, UT_KEYTYPE_STRING,
	                "application", REPUTE_URI_APPLICATION) != 0)
	{
		ut_destroy(ut);
		return REPUTE_STAT_INTERNAL;
	}

	if (ut_generate(ut, REPUTE_URI_TEMPLATE, url, sizeof url) <= 0)
	{
		ut_destroy(ut);
		return REPUTE_STAT_INTERNAL;
	}

	ut_destroy(ut);

	rio = repute_get_io(rep);
	if (rio == NULL)
		return REPUTE_STAT_INTERNAL;

	cstatus = curl_easy_setopt(rio->repute_curl, CURLOPT_WRITEDATA, rio);
	if (cstatus != CURLE_OK)
	{
		repute_put_io(rep, rio);
		return REPUTE_STAT_INTERNAL;
	}

	cstatus = curl_easy_setopt(rio->repute_curl, CURLOPT_URL, url);
	if (cstatus != CURLE_OK)
	{
		repute_put_io(rep, rio);
		return REPUTE_STAT_INTERNAL;
	}

	cstatus = curl_easy_perform(rio->repute_curl);
	if (cstatus != CURLE_OK)
	{
		repute_put_io(rep, rio);
		return REPUTE_STAT_QUERY;
	}

	cstatus = curl_easy_getinfo(rio->repute_curl, CURLINFO_RESPONSE_CODE,
	                            &rcode);
	if (rcode != 200)
	{
		repute_put_io(rep, rio);
		return REPUTE_STAT_QUERY;
	}

	(void) snprintf(rep->rep_uritemp, sizeof rep->rep_uritemp, "%s",
	                rio->repute_buf);
	if (rep->rep_uritemp[rio->repute_offset - 1] == '\n')
		rep->rep_uritemp[rio->repute_offset - 1] = '\0';

	repute_put_io(rep, rio);

	return REPUTE_STAT_OK;
}

/*
**  REPUTE_INIT -- initialize REPUTE subsystem
**
**  Parameters:
**  	None.
**
**  Return value:
**  	None.
*/

void
repute_init(void)
{
	xmlInitParser();

	curl_global_init(CURL_GLOBAL_ALL);
}

/*
**  REPUTE_NEW -- make a new REPUTE handle
**
**  Parameters:
**  	None.
**
**  Return value:
**  	A new REPUTE handle on success, NULL on failure.
*/

REPUTE
repute_new(const char *server)
{
	struct repute_handle *new;

	assert(server != NULL);

	new = malloc(sizeof *new);
	if (new == NULL)
		return NULL;

	memset(new, '\0', sizeof *new);

	new->rep_server = strdup(server);
	if (new->rep_server == NULL)
	{
		free(new);
		return NULL;
	}

	pthread_mutex_init(&new->rep_lock, NULL);

	return new;
}

/*
**  REPUTE_CLOSE -- tear down a REPUTE handle
**
**  Paramters:
**  	rep -- REPUTE handle to shut down
**
**  Return value:
**  	None.
*/

void
repute_close(REPUTE rep)
{
	struct repute_io *rio;
	struct repute_io *next;

	assert(rep == NULL);

	rio = rep->rep_ios;
	while (rio != NULL)
	{
		next = rio->repute_next;

		if (rio->repute_buf != NULL)
			free(rio->repute_buf);
		if (rio->repute_curl != NULL)
			curl_easy_cleanup(rio->repute_curl);
		free(rio);

		rio = next;
	}

	pthread_mutex_destroy(&rep->rep_lock);

	free((void *) rep->rep_server);

	free(rep);
}

/*
**  REPUTE_QUERY -- query a REPUTE server for a spam reputation
**
**  Parameters:
**  	rep -- REPUTE handle
**  	domain -- domain of interest
**  	repout -- reputation (returned)
**  	confout -- confidence (returned)
**  	sampout -- sample count (returned)
**  	whenout -- update timestamp (returned)
**
**  Return value:
**  	A REPUTE_STAT_* constant.
*/

REPUTE_STAT
repute_query(REPUTE rep, const char *domain, float *repout,
             float *confout, unsigned long *sampout, time_t *whenout)
{
	REPUTE_STAT status;
	float conf;
	float reputation;
	unsigned long samples;
	time_t when;
	struct repute_io *rio;
	URITEMP ut;
	char genurl[REPUTE_URL];
	char template[REPUTE_URL];

	assert(rep != NULL);
	assert(domain != NULL);
	assert(repout != NULL);

	if (rep->rep_uritemp[0] == '\0')
	{
		if (repute_get_template(rep) != REPUTE_STAT_OK)
			return REPUTE_STAT_QUERY;
	}

	ut = ut_init();
	if (ut == NULL)
		return REPUTE_STAT_INTERNAL;

	if (ut_keyvalue(ut, UT_KEYTYPE_STRING,
	                "subject", (void *) domain) != 0 ||
	    ut_keyvalue(ut, UT_KEYTYPE_STRING,
	                "scheme", REPUTE_URI_SCHEME) != 0 ||
	    ut_keyvalue(ut, UT_KEYTYPE_STRING,
	                "service", (void *) rep->rep_server) != 0 ||
	    ut_keyvalue(ut, UT_KEYTYPE_STRING,
	                "application", REPUTE_URI_APPLICATION) != 0 ||
	    ut_keyvalue(ut, UT_KEYTYPE_STRING,
	                "assertion", REPUTE_ASSERT_SENDS_SPAM) != 0)
	{
		ut_destroy(ut);
		return REPUTE_STAT_INTERNAL;
	}

	if (ut_generate(ut, rep->rep_uritemp, genurl, sizeof genurl) <= 0)
	{
		ut_destroy(ut);
		return REPUTE_STAT_INTERNAL;
	}

	ut_destroy(ut);

	rio = repute_get_io(rep);
	if (rio == NULL)
		return REPUTE_STAT_INTERNAL;

	status = repute_doquery(rio, genurl);
	if (status != REPUTE_STAT_OK)
	{
		repute_put_io(rep, rio);
		return status;
	}

	status = repute_parse(rio->repute_buf, rio->repute_offset,
	                      &reputation, &conf, &samples, &when);
	if (status != REPUTE_STAT_OK)
	{
		repute_put_io(rep, rio);
		return status;
	}

	*repout = reputation;
	if (confout != NULL)
		*confout = conf;
	if (sampout != NULL)
		*sampout = samples;
	if (whenout != NULL)
		*whenout = when;

	repute_put_io(rep, rio);

	return REPUTE_STAT_OK;
}

/*
**  REPUTE_ERROR -- return a pointer to the error buffer
**
**  Parameters:
**  	rep -- REPUTE handle
**
**  Return value:
**  	Pointer to the error buffer inside the REPUTE handle.
*/

const char *
repute_error(REPUTE rep)
{
	assert(rep != NULL);

	return rep->rep_error;
}