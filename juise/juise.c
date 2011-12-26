/*
 * $Id$
 *
 * Copyright (c) 2010-2011, Juniper Networks, Inc.
 * All rights reserved.
 * This SOFTWARE is licensed under the LICENSE provided in the
 * ../Copyright file. By downloading, installing, copying, or otherwise
 * using the SOFTWARE, you agree to be bound by the terms of that
 * LICENSE.
 *
 * juise -- driver for libjuise, allowing remote access for scripting
 */

#include <err.h>
#include <time.h>
#include <sys/time.h>
#include <time.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <pwd.h>
#include <sys/socket.h>

#include <libxml/tree.h>
#include <libxml/dict.h>
#include <libxml/uri.h>
#include <libxml/xpathInternals.h>
#include <libxml/parserInternals.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxslt/transform.h>
#include <libxml/HTMLparser.h>
#include <libxml/xmlsave.h>
#include <libexslt/exslt.h>
#include <libxslt/xsltutils.h>

#include "config.h"
#include <libslax/slax.h>
#include <libslax/slaxconfig.h>
#include <libslax/slaxdata.h>
#include <libslax/xmlsoft.h>

#include <libjuise/string/strextra.h>
#include <libjuise/time/timestr.h>
#include <libjuise/xml/libxml.h>
#include <libjuise/io/trace.h>
#include <libjuise/xml/jsio.h>
#include <libjuise/xml/extensions.h>
#include <libjuise/xml/juisenames.h>
#include <libjuise/juiseconfig.h>

#include "juise.h"

#define JM_NONE		0	/* Normal mode */
#define JM_CGI		1	/* CGI mode */
#define JM_CSCRIPT	2	/* Commit script mode */

static slax_data_list_t plist;
static int nbparams;

int dump_all;

int opt_debugger;
int opt_indent;
static int opt_load;
char *opt_output_format;

static void
juise_trace (void *vfp, lx_node_t *nodep, const char *fmt, ...)
{
    trace_file_t *tp = vfp;
    FILE *fp = tp ? trace_fileptr(tp) : stderr;
    va_list vap;

    va_start(vap, fmt);

    if (nodep) {
	xmlSaveCtxt *handle;

	fprintf(fp, "XML Content (%d)\n", nodep->type);
	fflush(fp);
	handle = xmlSaveToFd(fileno(fp), NULL,
			     XML_SAVE_FORMAT | XML_SAVE_NO_DECL);
	if (handle) {
	    xmlSaveTree(handle, nodep);
	    xmlSaveFlush(handle);
	    xmlSaveClose(handle);
	    fflush(fp);
	}

    } else if (tp) {
	tracev(tp, TRACE_ALL, fmt, vap);
    } else {
	vfprintf(fp, fmt, vap);
    }

    va_end(vap);
}

static void
juise_log (void *vfp, const char *fmt, va_list vap)
{
    trace_file_t *tp = vfp;
    FILE *fp = tp ? trace_fileptr(tp) : stderr;

    if (tp) {
	tracev(tp, TRACE_ALL, fmt, vap);
    } else {
	vfprintf(fp, fmt, vap);
	fprintf(stderr, "\n");
	fflush(stderr);
    }
}

static void
juise_make_param (const char *pname, const char *pvalue)
{
    char *tvalue;
    char quote;
    int plen;
    slax_data_node_t *dnp;
    int isname = 1;

    if (pname == NULL || pvalue == NULL)
	errx(1, "missing parameter value");

    SLAXDATALIST_FOREACH(dnp, &plist) {
	if (isname) {
	    if (streq(pname, dnp->dn_data)) {
		trace(trace_file, TRACE_ALL,
		      "param: ignoring dup: '%s'", pname);
		return;
	    }
	}
	isname ^= 1;
    }

    plen = strlen(pvalue);
    tvalue = xmlMalloc(plen + 3);
    if (tvalue == NULL)
	errx(1, "out of memory");

    quote = strrchr(pvalue, '\"') ? '\'' : '\"';
    tvalue[0] = quote;
    memcpy(tvalue + 1, pvalue, plen);
    tvalue[plen + 1] = quote;
    tvalue[plen + 2] = '\0';

    nbparams += 1;
    slaxDataListAddNul(&plist, pname);
    slaxDataListAddNul(&plist, tvalue);

    trace(trace_file, TRACE_ALL, "param: '%s' -> '%s'", pname, tvalue);
}

static inline int
is_filename_std (const char *filename)
{
    return (filename == NULL || (filename[0] == '-' && filename[1] == '\0'));
}

static lx_node_t *
juise_add_node (lx_node_t *parent, const char *tag, const char *content)
{
    lx_node_t *nodep, *childp;
    
    childp = xmlNewText((const xmlChar *) content);
    if (childp == NULL)
	return NULL;

    nodep = xmlNewNode(NULL, (const xmlChar *) tag);
    if (nodep == NULL)
	return NULL;

    xmlAddChild(nodep, childp);
    xmlAddChild(parent, nodep);

    return nodep;
}

static lx_document_t *
juise_build_op_input (lx_node_t *newp)
{
    xmlParserCtxtPtr ctxt = xmlNewParserCtxt();
    lx_document_t *docp;
    lx_node_t *input, *nodep, *childp;
    char *value;
    struct passwd *pwd;
    char hostname[MAXHOSTNAMELEN];
    time_t now;

    docp = xmlNewDoc((const xmlChar *) XML_DEFAULT_VERSION);
    if (docp == NULL)
	return NULL;

    docp->standalone = 1;

    input = xmlNewNode(NULL, (const xmlChar *) ELT_OP_SCRIPT_INPUT);
    while (input) {		/* Not _really_ a loop, but.... */
	xmlDocSetRootElement(docp, input);

	if (newp)
	    xmlAddChild(input, newp);

	nodep = xmlNewNode(NULL, (const xmlChar *) ELT_JUNOS_CONTEXT);
	if (nodep == NULL)
	    break;
	xmlAddChild(input, nodep);

	/* Hostname */
	if (gethostname(hostname, sizeof(hostname)) == 0)
	    juise_add_node(nodep, ELT_HOST_NAME, hostname);

	juise_add_node(nodep, ELT_PRODUCT, "juise");

	/* Time */
	time(&now);
	value = strndup(ctime(&now), 24);
	if (value) {
	    juise_add_node(nodep, ELT_LOCALTIME, value);
	    free(value);
	}
	juise_add_node(nodep, ELT_LOCALTIME_ISO, time_isostr(&now));

	juise_add_node(nodep, ELT_SCRIPT_TYPE, "op");

	childp = xmlNewNode(NULL, (const xmlChar *) ELT_USER_CONTEXT);
	if (childp == NULL)
	    break;
	xmlAddChild(nodep, childp);

	pwd = getpwuid(getuid());
	if (pwd) {
	    char nbuf[10];
	    juise_add_node(childp, ELT_USER, pwd->pw_name);

#ifdef HAVE_PWD_CLASS
	    juise_add_node(childp, ELT_CLASS_NAME, pwd->pw_class);
#endif

	    snprintf(nbuf, sizeof(nbuf), "%d", (int) pwd->pw_uid);
	    juise_add_node(childp, ELT_UID, nbuf);
	}

	juise_add_node(childp, ELT_OP_CONTEXT, "");

	break;			/* Not really a loop */
    }

    if (ctxt->dict) {
	docp->dict = ctxt->dict;
	xmlDictReference(docp->dict);
    }

    return docp;
}

/*
 * Emit the results for a CGI script.  If the top-level element
 * is <cgi>, then we lift out the attributes and turn them
 * into header fields and use the rest of the document as the result.
 */
static int
do_write_cgi_results (lx_document_t *res, xsltStylesheetPtr script)
{
    lx_node_t *root = lx_document_root(res);

    if (streq(xmlNodeName(root), ELT_CGI)) {
	xmlSaveCtxt *handle;
	lx_node_t *nodep;
	xmlAttrPtr attr;
	char *value;

	for (attr = root->properties; attr; attr = attr->next) {
	    if (attr->name[0] == 'x' && attr->name[0] == 'm'
			&& attr->name[0] == 'l')
		continue;

	    value = (char *) xmlGetProp(root, attr->name);
	    if (value == NULL)
		continue;

	    printf("%s: %s\n", attr->name, value);
	    xmlFree(value);
	}

	printf("\n");

	for (nodep = lx_node_children(root); nodep;
	     nodep = lx_node_next(nodep)) {
	    if (nodep->type == XML_ELEMENT_NODE) {
		fflush(stdout);
		handle = xmlSaveToFd(fileno(stdout), NULL,
				     XML_SAVE_FORMAT | XML_SAVE_NO_DECL);
		if (handle) {
		    xmlSaveTree(handle, nodep);
		    xmlSaveFlush(handle);
		    xmlSaveClose(handle);
		    fflush(stdout);
		    return 0;
		}
	    }
	}
    }

    xsltSaveResultToFile(stdout, res, script);

    return 0;
}

static xsltStylesheetPtr
read_script (const char *scriptname)
{
    lx_document_t *scriptdoc;
    FILE *scriptfile;
    xsltStylesheetPtr script;

    if (scriptname == NULL)
	errx(1, "missing script name");

    scriptfile = fopen(scriptname, "r");
    if (scriptfile == NULL)
	err(1, "file open failed for '%s'", scriptname);

    scriptdoc = slaxLoadFile(scriptname, scriptfile, NULL, 0);
    if (scriptdoc == NULL)
	errx(1, "cannot parse: '%s'", scriptname);

    if (scriptfile != stdin)
	fclose(scriptfile);

    script = xsltParseStylesheetDoc(scriptdoc);
    if (script == NULL || script->errors != 0)
	errx(1, "%d errors parsing script: '%s'",
	     script ? script->errors : 1, scriptname);

    return script;
}

static lx_document_t *
run_script (xsltStylesheetPtr script, const char *scriptname,
	    lx_document_t *indoc, const char **params, int mode)
{
    lx_document_t *res = NULL;

    if (opt_indent)
	script->indent = 1;

    if (opt_debugger) {
	slaxDebugInit();
	slaxDebugSetStylesheet(script);
	res = slaxDebugApplyStylesheet(scriptname, script, "input",
				       indoc, params);
    } else {

	res = xsltApplyStylesheet(script, indoc, params);
	if (res) {
	    if (mode == JM_CGI)
		do_write_cgi_results(res, script);
	    else if (mode == JM_NONE)
		xsltSaveResultToFile(stdout, res, script);

	    if (dump_all)
		xsltSaveResultToFile(stderr, res, script);
	}
    }

    return res;
}

static int
do_run_op_common (const char *scriptname, const char *input,
		  char **argv UNUSED, lx_node_t *nodep, int cgi_mode)
{
    lx_document_t *indoc, *res = NULL;
    xsltStylesheetPtr script;
    slax_data_node_t *dnp;
    int i = 0;
    const char **params;

    params = alloca(nbparams * 2 * sizeof(*params) + 1);
    SLAXDATALIST_FOREACH(dnp, &plist) {
	params[i++] = dnp->dn_data;
    }

    params[i] = NULL;

    script = read_script(scriptname);
    if (script == NULL)
	return -1;

    if (input) {
	FILE *infile;

	infile = streq(input, "-") ? stdin : fopen(input, "r");
	if (infile == NULL)
	    err(1, "file open failed for '%s'", input);

	indoc = slaxLoadFile(input, infile, NULL, FALSE);

	if (infile != stdin)
	    fclose(infile);

    } else {
	indoc = juise_build_op_input(nodep);
	if (indoc == NULL)
	    errx(1, "unable to build input document");
    }

    res = run_script(script, scriptname, indoc, params, cgi_mode);
    if (res)
	xmlFreeDoc(res);

    xmlFreeDoc(indoc);
    xsltFreeStylesheet(script);

    return 0;
}

static int
do_run_op (const char *scriptname, const char *input, char **argv)
{
    return do_run_op_common(scriptname, input, argv, NULL, JM_NONE);
}

static lx_node_t *
juise_build_get_configuration_rpc (lx_document_t **docpp, int post,
				   const char *format)
{
    char rpc_text[BUFSIZ];
    lx_document_t *xmlp;
    lx_node_t *rootp;

    snprintf(rpc_text, sizeof(rpc_text),
	     "<rpc><get-configuration%s%s%s/></rpc>",
	     post ? "" : " commit-scripts=\"view\"",
	     streq(format, "text") ? " format=\"text\"" : "",
	     streq(format, "compare") ? " compare=\"\"" : "");

    *docpp = xmlp = xmlReadMemory(rpc_text, strlen(rpc_text), "rpc_text",
				  NULL, XML_PARSE_NOENT);
    if (xmlp == NULL)
	return NULL;

    rootp = xmlDocGetRootElement(xmlp);
    if (rootp)
	ext_jcs_fix_namespaces(rootp);

    return rootp;
}

static lx_document_t *
juise_build_input_commit (lx_nodeset_t *config_data)
{
    static char doc_text[] = "<commit-script-input \
xmlns:junos=\"http://xml.juniper.net/junos/*/junos\"/>\n";
    lx_document_t *docp;
    lx_node_t *rootp, *nodep;
    int i;

    docp = xmlReadMemory(doc_text, strlen(doc_text), "doc_text",
			 NULL, XML_PARSE_NOENT);
    if (docp == NULL)
	return NULL;

    rootp = xmlDocGetRootElement(docp);

    for (i = 0; i < config_data->nodeNr; i++) {
	nodep = config_data->nodeTab[i];
	if (nodep == NULL)
	    continue;

	xmlNodePtr newp = xmlDocCopyNode(nodep, docp, 1);
	if (newp)
	    xmlAddChild(rootp, newp);
    }

    return docp;
}

static void
output_node (const char *title, lx_node_t *nodep)
{
    lx_output_t *handle = lx_output_open_fd(1);

    if (handle == NULL)
	return;

    printf("\n%s:\n", title);
    fflush(stdout);

    lx_output_node(handle, nodep);
    lx_output_close(handle);

    printf("\n");
    fflush(stdout);
}

static void
output_nodeset (const char *title, lx_nodeset_t *nsp)
{
    lx_output_t *handle = lx_output_open_fd(1);
    int i;
    lx_node_t *nodep;

    if (handle == NULL)
	return;

    printf("\nResults from %s:\n", title);
    fflush(stdout);

    for (i = 0; i < nsp->nodeNr; i++) {
	nodep = nsp->nodeTab[i];
	if (nodep == NULL || nodep->type != XML_ELEMENT_NODE)
	    continue;
	lx_output_node(handle, nodep);
    }

    lx_output_close(handle);
    printf("\n");
    fflush(stdout);
}

static int
result_has_success (lx_nodeset_t *nsp)
{
    int i;
    lx_node_t *nodep;

    for (i = 0; i < nsp->nodeNr; i++) {
	nodep = nsp->nodeTab[i];
	if (nodep == NULL || nodep->type != XML_ELEMENT_NODE)
	    continue;

	if (!streq((const char *) nodep->name,
		   ELT_LOAD_CONFIGURATION_RESULTS))
	    continue;

	for (nodep = nodep->children; nodep; nodep = nodep->next) {
	    if (nodep->type != XML_ELEMENT_NODE)
		continue;
	    if (streq((const char *) nodep->name, ELT_LOAD_SUCCESS))
		return TRUE;
	}
    }

    return FALSE;
}

static int
load_change (js_session_t *jsp UNUSED, xmlXPathParserContext *pctxt,
	     lx_node_t *changep, int transient)
{
    static char rpc_text[] = "<rpc><load-configuration \
xmlns:junos=\"http://xml.juniper.net/junos/*/junos\">\
<configuration/></load-configuration></rpc>\n";

    lx_document_t *docp;
    lx_node_t *rootp, *nodep, *confp = NULL, *childp;
    lx_nodeset_t *res;
    int rc = FALSE;

    docp = xmlReadMemory(rpc_text, strlen(rpc_text), "rpc_text",
			 NULL, XML_PARSE_NOENT);
    if (docp == NULL)
	return TRUE;

    rootp = xmlDocGetRootElement(docp);

    if (rootp && rootp->children && rootp->children->children)
	confp = rootp->children->children;

    if (confp == NULL || confp->type != XML_ELEMENT_NODE) {
	xmlFreeDoc(docp);
	return TRUE;
    }

    for (childp = changep->children; childp; childp = childp->next) {
	nodep = xmlDocCopyNode(childp, docp, 1);
	if (nodep)
	    xmlAddChild(confp, nodep);
    }

    res = js_session_execute(pctxt, NULL, rootp, NULL, ST_DEFAULT);
    if (res == NULL) {
	fprintf(stderr, "load-configuration failed");
	output_node("Failed to load the following configuration", changep);
	rc = TRUE;

    } else {
	output_nodeset(transient ? "load change-transient" : "load change",
		       res);
	if (!result_has_success(res)) {
	    output_node("Failed to load the following configuration",
			changep);
	    rc = TRUE;
	}
    }

    xmlFreeDoc(docp);
    return rc;
}

static int
invoke_rpc (js_session_t *jsp UNUSED, xmlXPathParserContext *pctxt,
	    const char *title, const char *rpc_text)
{
    lx_document_t *docp;
    lx_node_t *rootp;
    lx_nodeset_t *res;
    int rc = FALSE;

    docp = xmlReadMemory(rpc_text, strlen(rpc_text), "rpc_text",
			 NULL, XML_PARSE_NOENT);
    if (docp == NULL) {
	fprintf(stderr, "rpc failed to parse");
	return TRUE;
    }

    rootp = xmlDocGetRootElement(docp);

    res = js_session_execute(pctxt, NULL, rootp, NULL, ST_DEFAULT);
    if (res == NULL) {
	fprintf(stderr, "rpc execution failed");
	rc = TRUE;
    } else {
	output_nodeset(title, res);
	xmlXPathFreeNodeSet(res);
    }


    xmlFreeDoc(docp);

    return rc;
}

static int
run_edit_private (js_session_t *jsp UNUSED,
		  xmlXPathParserContext *pctxt UNUSED)
{
    static char rpc_text[] = "<rpc><open-configuration \
xmlns:junos=\"http://xml.juniper.net/junos/*/junos\">\
<private/></open-configuration></rpc>\n";

    return invoke_rpc(jsp, pctxt, "edit private", rpc_text);
}

static int
run_commit_check (js_session_t *jsp UNUSED,
		  xmlXPathParserContext *pctxt UNUSED)
{
    static char rpc_text[] = "<rpc><commit-configuration \
xmlns:junos=\"http://xml.juniper.net/junos/*/junos\">\
<check/></commit-configuration></rpc>\n";

    return invoke_rpc(jsp, pctxt, "commit check", rpc_text);
}

static void
report_error (const char *tag, lx_node_t *child)
{
    const char *path = lx_node_child_value(child, ELT_EDIT_PATH);
    const char *stmt = lx_node_child_value(child, ELT_STATEMENT);
    const char *msg = lx_node_child_value(child, ELT_MESSAGE);
    int indent = 0;

    if (path) {
	fprintf(stderr, "%s\n", path);
	indent += 2;
    }

    if (stmt) {
	fprintf(stderr, "%*s'%s'\n", indent, "", stmt);
	indent += 2;
    }
    
    fprintf(stderr, "%*s%s: %s\n", indent, "", tag, msg ?: "unknown");
}

static int
show_post_commit_config (js_session_t *jsp UNUSED,
			 xmlXPathParserContext *pctxt,
			 const char *format)
{
    lx_document_t *rpc = NULL;
    lx_node_t *get_config_rpc;
    lx_nodeset_t *res;

    get_config_rpc = juise_build_get_configuration_rpc(&rpc, TRUE, format);

    res = js_session_execute(pctxt, NULL, get_config_rpc,
				     NULL, ST_DEFAULT);
    if (res == NULL)
	err(0, "get-configuration (post) rpc failed");

    output_nodeset("script", res);
    xmlXPathFreeNodeSet(res);

    if (rpc)
	xmlFreeDoc(rpc);

    return FALSE;
}

static int
do_test_commit_script (const char *scriptname, const char *input UNUSED,
		       char **argv UNUSED)
{
    lx_document_t *rpc = NULL, *docp, *indoc, *res = NULL;
    xmlXPathContextPtr ctxt;
    xmlXPathParserContext *pctxt;
    lx_nodeset_t *config_data;
    lx_node_t *get_config_rpc;
    xsltStylesheetPtr script;
    const char **params;
    slax_data_node_t *dnp;
    int i = 0;
    js_session_t *jsp;
    int rc = FALSE;

    params = alloca(nbparams * 2 * sizeof(*params) + 1);
    SLAXDATALIST_FOREACH(dnp, &plist) {
	params[i++] = dnp->dn_data;
    }
    params[i] = NULL;

    docp = xmlNewDoc((const xmlChar *) XML_DEFAULT_VERSION);
    ctxt = xmlXPathNewContext(docp);
    pctxt = xmlXPathNewParserContext(NULL, ctxt);

    jsp = js_session_open(NULL, NULL, NULL, 0, 0, 0);
    if (jsp == NULL)
	errx(1, "could not open session to target");
    
    get_config_rpc = juise_build_get_configuration_rpc(&rpc, FALSE, "xml");

    config_data = js_session_execute(pctxt, NULL, get_config_rpc,
				     NULL, ST_DEFAULT);
    if (config_data == NULL)
	err(0, "get-configuration rpc failed");

    if (rpc)
	xmlFreeDoc(rpc);

    script = read_script(scriptname);
    if (script == NULL)
	return -1;

    indoc = juise_build_input_commit(config_data);

    res = run_script(script, scriptname, indoc, params, JM_CSCRIPT);
    if (res) {
	/*
	 * We need to look through the output of a commit script,
	 * which can contain:
	 * <error> -- report and stop
	 * <warning> -- report and continue
	 * <change> -- normal configuration change
	 * <change-transient> -- transient configuration change
	 * <syslog> -- report (but don't syslog)
	 * <progress> -- report
	 */
	int seen_error = 0, seen_change = 0, seen_transient = 0;
	lx_node_t *childp;
	lx_node_t *rootp = xmlDocGetRootElement(res);

	if (rootp == NULL)
	    errx(1, "could not find document root");

	for (childp = rootp->children; childp; childp = childp->next) {
	    if (childp->type != XML_ELEMENT_NODE)
		continue;

	    if (streq((const char *) childp->name, ELT_ERROR)) {
		seen_error += 1;
		report_error("error", childp);

	    } else if (streq((const char *) childp->name, ELT_WARNING)) {
		report_error("warning", childp);

	    } else if (streq((const char *) childp->name, ELT_CHANGE)) {
		/* Wait for second pass */
		seen_change += 1;

	    } else if (streq((const char *) childp->name,
			     ELT_CHANGE_TRANSIENT)) {
		/* Wait for second pass */
		seen_transient += 1;

	    } else if (streq((const char *) childp->name, ELT_SYSLOG)) {
		const char *msg = xmlNodeValue(childp);
		fprintf(stdout, "syslog: %s\n", msg);

	    } else if (streq((const char *) childp->name, ELT_PROGRESS)) {
		const char *msg = xmlNodeValue(childp);
		fprintf(stdout, "progress-message: %s\n", msg);

	    } else {
		fprintf(stderr, "unknown tag: '%s' (ignored)\n",
			(const char *) childp->name);
	    }
	}

	if (!seen_error && (seen_change || seen_transient)) {
	    if (run_edit_private(jsp, pctxt))
		goto done;

	    for (childp = rootp->children; childp; childp = childp->next) {
		if (childp->type != XML_ELEMENT_NODE)
		    continue;

		if (streq((const char *) childp->name, ELT_CHANGE))
		    rc = load_change(jsp, pctxt, childp, FALSE);

		else if (streq((const char *) childp->name,
			       ELT_CHANGE_TRANSIENT))
		    rc = load_change(jsp, pctxt, childp, TRUE);

		if (rc)
		    break;
	    }

	    if (!rc && !run_commit_check(jsp, pctxt) && opt_output_format)
		show_post_commit_config(jsp, pctxt, opt_output_format);
	}

    done:
	xmlFreeDoc(res);
    }

    xmlFreeDoc(indoc);
    xsltFreeStylesheet(script);
    js_session_close(NULL, 0);

    return 0;
}

static int
do_run_server_on_stdin (const char *scriptname UNUSED,
			const char *input UNUSED, char **argv UNUSED)
{
    int fd = 0;

    if (input && !streq(input, "-")) {
	fd = open(input, O_RDONLY);
	if (fd < 0)
	    err(1, "could not open file '%s'", input);
    }

    run_server(fd, 1, ST_DEFAULT);
    return 0;
}

static void
parse_query_string (lx_node_t *nodep, char *str)
{
    char *cp, *ap, *ep, *xp;

    trace(trace_file, TRACE_ALL, "querystring: %p, (%u) '%s'",
	  nodep, (unsigned) strlen(str), str);

    for (cp = str; cp; cp = ap) {
	ep = strchr(cp, '=');
	if (ep == NULL)
	    break;
	*ep++ = '\0';

	ap = strchr(ep, '&');
	if (ap != NULL)
	    *ap++ = '\0';

	/* Pluses are really spaces; pluses are URL-escaped */
	for (xp = ep; *xp; xp++)
	    if (*xp == '+')
		*xp = ' ';

	/* At this point, cp is the name and ep is the value */
	cp = xmlURIUnescapeString(cp, 0, NULL);
	ep = xmlURIUnescapeString(ep, 0, NULL);
	
	if (cp && ep) {
	    trace(trace_file, TRACE_ALL, "querystring: param: '%s' -> '%s'",
		  cp, ep);
	    if (strncmp(cp, "junos", 5) != 0 && !streq(cp, ELT_CGI))
		juise_make_param(cp, ep);
	    juise_add_node(nodep, cp, ep);
	}
	xmlFreeAndEasy(ep);	/* xmlURIUnescapeString allocated them */
	xmlFreeAndEasy(cp);
    }
}

static int
do_run_as_cgi (const char *scriptname, const char *input UNUSED, char **argv)
{
    const char *cgi_params[] = {
	"CONTENT_LENGTH",	"content-length",
	"DOCUMENT_ROOT",	"document-root",
	"GATEWAY_INTERFACE",	"gateway-interface",
	"HTTPS",		"https",
	"LD_LIBRARY_PATH",	"ld-library-path",
	"LD_PRELOAD",		"ld-preload",
	"PATH_INFO",		"path-info",
	"QUERY_STRING",		"query-string",
	"REDIRECT_STATUS",	"redirect-status",
	"REMOTE_ADDR",		"remote-addr",
	"REMOTE_PORT",		"remote-port",
	"REMOTE_USER",		"remote-user",
	"REQUEST_METHOD",	"request-method",
	"REQUEST_URI",		"request-uri",
	"SCRIPT_FILENAME",	"script-filename",
	"SCRIPT_NAME",		"script-name",
	"SERVER_ADDR",		"server-addr",
	"SERVER_NAME",		"server-name",
	"SERVER_PORT",		"server-port",
	"SERVER_PROTOCOL",	"server-protocol",
	"SERVER_SOFTWARE",	"server-software",
	"SYSTEMROOT",		"systemroot",
	NULL,			NULL,
    };

    int len = 0;
    int i;
    lx_node_t *nodep, *paramp = NULL;
    char *cp, *bp;
    char buf[BUFSIZ];
    struct line_s {		/* XXX: should be a slax_data_list_t */
	struct line_s *li_next;
	unsigned li_len;
	char li_data[0];
    } *lines, **lastp = &lines, *lp;
    const char *method = NULL;

    nodep = xmlNewNode(NULL, (const xmlChar *) ELT_CGI);
    if (nodep == NULL)
	errx(1, "op: out of memory");

    /* Turn all the CGI environment variables into XSLT parameters */
    for (i = 0; cgi_params[i]; i += 2) {
	cp = getenv(cgi_params[i]);
	if (cp) {
	    juise_make_param(cgi_params[i], cp);
	    juise_add_node(nodep, cgi_params[i + 1], cp);
	    trace(trace_file, TRACE_ALL, "cgi: env: '%s' = '%s'",
		  cgi_params[i], cp);

	    if (streq("request-method", cgi_params[i]))
		method = cp;
	}
    }

    paramp = xmlNewNode(NULL, (const xmlChar *) ELT_PARAMETERS);
    if (paramp == NULL)
	errx(1, "juise: out of memory");
    xmlAddChild(nodep, paramp);

    cp = getenv("QUERY_STRING");
    if (cp)
	parse_query_string(paramp, cp);

    while (fgets(buf, sizeof(buf), stdin) != NULL) {
	trace(trace_file, TRACE_ALL, "cgi: stdin: %s", buf);
	i = strlen(buf);
	len += i;
	lp = malloc(i + sizeof(*lp));
	if (lp == NULL)
	    break;
	lp->li_next = NULL;
	lp->li_len = i;
	memcpy(lp->li_data, buf, i);
	*lastp = lp;
	lastp = &lp->li_next;
    }

    if (len) {
	/* If we have a linked line, combine them into a single buffer */
	if (lines->li_next) {
	    bp = malloc(len);

	    if (bp) {
		cp = bp;
		for (lp = lines; lp; lp = lp->li_next) {
		    memcpy(cp, lp->li_data, lp->li_len);
		    cp += lp->li_len;
		}
		*cp = '\0';
	    }
	} else {
	    bp = lines->li_data;
	}

	if (bp) {
	    if (method && streq(method, "POST")) {
		juise_add_node(nodep, ELT_CONTENTS, bp);
	    } else {
		parse_query_string(paramp, bp);
	    }

	    if (bp != lines->li_data)
		free(bp);
	}

	for (lp = lines; lp; lp = lines) {
	    lines = lp->li_next;
	    free(lp);
	}
    }

    return do_run_op_common(scriptname, input, argv, nodep, JM_CGI);
}

static int
do_run_as_fastcgi (const char *scriptname UNUSED, const char *input UNUSED,
		   char **argv UNUSED)
{
    return 0;
}

static char **
build_argv (const char *argstring)
{
    char *av[1000], **realv;
    int ac = 0;
    const char *cp, *sp;
    char *ap;
    int len;
    char quote = 0;

    if (argstring == NULL || *argstring == '\0')
	return NULL;

    for (sp = cp = argstring; ; cp++) {
	if (*cp == '\'' || *cp == '\"') {
	    if (quote == *cp) {
		quote = 0;
	    } else if (quote == 0) {
		quote = *cp;
	    }

	} else if (*cp == '\\') {
	    if (cp[0])
		cp += 1;

	} else if (quote) {
	    /* do nothing */

	} else if (*cp == ' ' || *cp == '\t' || *cp == '\0') {
	    ap = malloc(cp - sp + 1);
	    if (ap) {
		memcpy(ap, sp, cp - sp);
		ap[cp - sp] = '\0';
		av[ac++] = ap;

		if (*cp != '\0')
		    sp = cp + 1;
	    }
	}

	if (*cp == '\0')
	    break;
    }

    if (ac == 0)
	return NULL;

    av[ac] = NULL;

    len = (ac + 1) * sizeof(realv[0]);
    realv = malloc(len);
    if (realv)
	memcpy(realv, av, len);

    return realv;
}

static void
print_version (void)
{
    printf("libjuice version %s\n", LIBJUISE_VERSION);
    printf("libslax version %s\n",  LIBSLAX_VERSION);
    printf("Using libxml %s, libxslt %s and libexslt %s\n",
	   xmlParserVersion, xsltEngineVersion, exsltLibraryVersion);
    printf("slaxproc was compiled against libxml %d, "
	   "libxslt %d and libexslt %d\n",
	   LIBXML_VERSION, LIBXSLT_VERSION, LIBEXSLT_VERSION);
    printf("libxslt %d was compiled against libxml %d\n",
	   xsltLibxsltVersion, xsltLibxmlVersion);
    printf("libexslt %d was compiled against libxml %d\n",
	   exsltLibexsltVersion, exsltLibxmlVersion);
}

static void
print_help (void)
{
    printf("Usage: juise [@target] [options] [script] [param value]*\n");
    printf("\t--agent OR -A: enable ssh-agent forwarding\n");
    printf("\t--commit-script OR -c: test a commit script\n");
    printf("\t--debug OR -d: use the libslax debugger\n");
    printf("\t--directory <dir> OR -D <dir>: set JUISE_DIR (for server scripts)\n");
    printf("\t--include <dir> OR -I <dir>: search directory for includes/imports\n");
    printf("\t--input <file> OR -i <file>: use given file for input\n");
    printf("\t--indent OR -g: indent output ala output-method/indent\n");
    printf("\t--junoscript OR -J: use junoscript API protocol\n");
    printf("\t--load OR -l: load commit script changes in test mode\n");
    printf("\t--lib <dir> OR -L <dir>: search directory for extension libraries\n");
    printf("\t--no-randomize: do not initialize the random number generator\n");
    printf("\t--param <name> <value> OR -a <name> <value>: pass parameters\n");
    printf("\t--protocol <name> OR -P <name>: use the given API protocol\n");
    printf("\t--run-server OR -R: run in juise server mode\n");
    printf("\t--script <name> OR -S <name>: run the given script\n");
    printf("\t--target <name> OR -T <name>: specify the default target device\n");
    printf("\t--trace <file> OR -t <file>: write trace data to a file\n");
    printf("\t--user <name> OR -u <name>: specify the user name for API connections\n");
    printf("\t--verbose OR -v: enable debugging output (slaxLog())\n");
    printf("\t--version OR -V: show version information (and exit)\n");
    printf("\t--wait <seconds>: wait after starting (for gdb to attach)\n");

    printf("\nProject juise home page: http://juise.googlecode.com\n");
}

int
main (int argc UNUSED, char **argv, char **envp)
{
    char **save_argv = argv;
    char *cp;
    const char *script = NULL, *trace_file_name = NULL;
    int (*func)(const char *, const char *, char **) = NULL;
    FILE *trace_fp = NULL;
    int randomize = 1;
    int logger = FALSE;
    char *target = NULL;
    char *user = NULL;
    int ssh_agent_forwarding = FALSE;
    session_type_t stype;
    int skip_args = FALSE;
    int waiting = 0;
    int i;
    char *input = NULL;
    unsigned jsio_flags = 0;
    int opt_commit_script = FALSE;
    char *env_args = getenv("JUISE_OPTIONS");

    slaxDataListInit(&plist);

    cp = *argv;
    if (cp) {
	static char strxmlmode[] = "xml-mode";
	static char strcgi[] = ".cgi";
	static char strfastcgi[] = ".fastcgi";

	char *ep = cp + strlen(cp) + 1;

	if (streq(strxmlmode, ep - sizeof(strxmlmode))) {
	    func = do_run_server_on_stdin;
	    skip_args = TRUE;
	    jsio_set_default_session_type(ST_JUNOS_NETCONF);

	} else if (streq(strcgi, ep - sizeof(strcgi))) {
	    func = do_run_as_cgi;

	} else if (streq(strfastcgi, ep - sizeof(strfastcgi))) {
	    func = do_run_as_fastcgi;
	}
    }

    if (!skip_args) {
	argv += 1;		/* Skip argv[0] */
    parse_args:
	for ( ; *argv; argv++) {
	    cp = *argv;

	    if (*cp != '-') {
		break;

	    } else if (streq(cp, "--agent") || streq(cp, "-A")) {
		ssh_agent_forwarding = TRUE;

	    } else if (streq(cp, "--commit-script") || streq(cp, "-c")) {
		opt_commit_script = TRUE;
		func = do_test_commit_script;

	    } else if (streq(cp, "--debug") || streq(cp, "-d")) {
		opt_debugger = TRUE;

	    } else if (streq(cp, "--debug-io")) {
		jsio_flags |= JSIO_MEMDUMP;

	    } else if (streq(cp, "--directory") || streq(cp, "-D")) {
		srv_set_juise_dir(*++argv);

	    } else if (streq(cp, "--include") || streq(cp, "-I")) {
		slaxIncludeAdd(*++argv);

	    } else if (streq(cp, "--input") || streq(cp, "-i")) {
		input = *++argv;

	    } else if (streq(cp, "--indent") || streq(cp, "-g")) {
		opt_indent = TRUE;

	    } else if (streq(cp, "--junoscript") || streq(cp, "-J")) {
		stype = ST_JUNOSCRIPT;

	    } else if (streq(cp, "--load") || streq(cp, "-l")) {
		opt_load = TRUE;

	    } else if (streq(cp, "--lib") || streq(cp, "-L")) {
		slaxDynAdd(*++argv);

	    } else if (streq(cp, "--no-randomize")) {
		randomize = 0;

	    } else if (streq(cp, "--op") || streq(cp, "-O")) {
		if (func)
		    errx(1, "open one action allowed");
		func = do_run_op;

	    } else if (streq(cp, "--output-format")) {
		opt_output_format = *++argv;
		if (!streq(opt_output_format, "html")
			    && !streq(opt_output_format, "text")
			    && !streq(opt_output_format, "compare"))
		    errx(1, "invalid output format: %s", opt_output_format);
	    
	    } else if (streq(cp, "--param") || streq(cp, "-a")) {
		char *pname = *++argv;
		char *pvalue = *++argv;

		juise_make_param(pname, pvalue);

	    } else if (streq(cp, "--protocol") || streq(cp, "-P")) {
		cp = *++argv;
		stype = jsio_session_type(cp);
		if (stype == ST_MAX) {
		    fprintf(stderr, "invalid protocol: %s\n", cp);
		    return -1;
		}
		jsio_set_default_session_type(stype);

	    } else if (streq(cp, "--run-server") || streq(cp, "-R")) {
		func = do_run_server_on_stdin;

	    } else if (streq(cp, "--script") || streq(cp, "-S")) {
		script = *++argv;

	    } else if (streq(cp, "--target") || streq(cp, "-T")) {
		target = *++argv;

	    } else if (streq(cp, "--trace") || streq(cp, "-t")) {
		trace_file_name = *++argv;

	    } else if (streq(cp, "--user") || streq(cp, "-u")) {
		user = *++argv;

	    } else if (streq(cp, "--verbose") || streq(cp, "-v")) {
		logger = TRUE;

	    } else if (streq(cp, "--version") || streq(cp, "-V")) {
		print_version();
		exit(0);

	    } else if (streq(cp, "--wait")) {
		waiting = atoi(*++argv);

	    } else {
		fprintf(stderr, "invalid option: %s\n", cp);
		print_help();
		return -1;
	    }
	}

	/*
	 * Handle the rest of argv:
	 * - @xxx -> --target xxx
	 * - the first argument is the name of the script
	 * - the rest of the arguments are <name> <value> parameters
	 */
	for ( ; *argv; argv++) {
	    cp = *argv;

	    if (target == NULL && *cp == '@') {
		target = cp + 1;

	    } else if (target == NULL && (target = strchr(cp, '@')) != NULL) {
		user = cp;
		*target++ = '\0';

	    } else if (script == NULL) {
		script = cp;

	    } else {
		char *pname = cp;
		char *pvalue = *++argv;

		juise_make_param(pname, pvalue);
	    }
	}
    }

    if (env_args) {
	argv = build_argv(env_args);
	env_args = NULL;
	if (argv)
	    goto parse_args;
    }

    if (func == NULL)
	func = do_run_op; /* the default action */

    if (trace_file_name == NULL)
	trace_file_name = getenv("JUISE_TRACE_FILE");

    if (trace_file_name) {
	dump_all = 1;

	if (is_filename_std(trace_file_name)) {
	    slaxTraceEnable(juise_trace, NULL);
	    slaxLogEnableCallback(juise_log, NULL);
	} else {
	    trace_file = trace_file_open(NULL, trace_file_name,
					 1000000, 10);
	    if (trace_file == NULL || trace_fileptr(trace_file) == NULL)
		errx(1, "could not open trace file: %s", trace_file_name);
		
	    slaxTraceEnable(juise_trace, trace_file);
	    slaxLogEnableCallback(juise_log, trace_file);
	}

	if (dump_all) {
	    for (i = 0; save_argv[i]; i++)
		trace(trace_file, TRACE_ALL, "argv: '%s'", save_argv[i]);

	    for (i = 0; envp[i]; i++)
		trace(trace_file, TRACE_ALL, "envp: '%s'", envp[i]);
	}
    }

    if (!waiting) {
	cp = getenv("JUISE_WAIT");
	if (cp)
	    waiting = atoi(cp);
    }

    /* Waiting allows 'gdb' to attach to a spawned process */
    if (waiting) {
	trace(trace_file, TRACE_ALL, "waiting %d seconds", waiting);
	sleep(waiting);
    }

    /*
     * Seed the random number generator.  This is optional to allow
     * test jigs to take advantage of the default stream of generated
     * numbers.
     */
    if (randomize)
	slaxInitRandomizer();

    /*
     * Start the XML API
     */
    xmlInitParser();
    xsltInit();
    slaxEnable(SLAX_ENABLE);
    slaxIoUseStdio();

    if (logger)
	slaxLogEnable(TRUE);

    exsltRegisterAll();
    ext_jcs_register_all();

    jsio_init(jsio_flags);

    if (target)
	jsio_set_default_server(target);
    else if (opt_commit_script)
	errx(1, "target must be specified for commit script mode");

    if (user)
	jsio_set_default_user(user);

    if (ssh_agent_forwarding)
	jsio_set_ssh_options("-A");

    func(script, input, argv);

    if (trace_fp && trace_fp != stderr)
	fclose(trace_fp);

    xsltCleanupGlobals();
    xmlCleanupParser();

    jsio_cleanup();

    return 0;
}
