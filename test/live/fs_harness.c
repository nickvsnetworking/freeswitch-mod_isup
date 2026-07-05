/* Minimal libfreeswitch harness: init the real FreeSWITCH core, load
 * mod_isup.so, optionally originate an ISUP call, then idle.
 *
 *   fs_harness <seconds> [originate <dest>]
 */
#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

/* switch_core_destroy()'s final teardown (log/session/memory engines) can hang
 * in this minimal SCF_NONE harness — it is not a mod_isup path (module shutdown
 * has already completed by then) and a full FreeSWITCH tears down cleanly. As a
 * test process we just need to exit; a watchdog forces termination if the core
 * teardown stalls. */
static void hard_exit(int sig) { (void)sig; _exit(0); }

static switch_status_t hlog(const switch_log_node_t *node, switch_log_level_t level)
{
	(void)level;
	if (node && node->data) fprintf(stderr, "FSLOG %s", node->data);
	return SWITCH_STATUS_SUCCESS;
}

int main(int argc, char **argv)
{
	const char *err = NULL;
	const char *base = "/tmp/fsrun";
	int secs = (argc > 1) ? atoi(argv[1]) : 15;

	SWITCH_GLOBAL_dirs.base_dir   = strdup(base);
	SWITCH_GLOBAL_dirs.mod_dir    = strdup(getenv("ISUP_MOD_DIR") ? getenv("ISUP_MOD_DIR") : base);
	SWITCH_GLOBAL_dirs.conf_dir   = strdup(base);
	SWITCH_GLOBAL_dirs.log_dir    = strdup(base);
	SWITCH_GLOBAL_dirs.run_dir    = strdup(base);
	SWITCH_GLOBAL_dirs.db_dir     = strdup(base);
	SWITCH_GLOBAL_dirs.script_dir = strdup(base);
	SWITCH_GLOBAL_dirs.temp_dir   = strdup("/tmp");
	SWITCH_GLOBAL_dirs.storage_dir= strdup(base);
	SWITCH_GLOBAL_dirs.cache_dir  = strdup(base);
	SWITCH_GLOBAL_dirs.recordings_dir = strdup(base);
	SWITCH_GLOBAL_dirs.sounds_dir = strdup(base);
	SWITCH_GLOBAL_dirs.lib_dir    = strdup(base);
	SWITCH_GLOBAL_dirs.grammar_dir= strdup(base);
	SWITCH_GLOBAL_dirs.certs_dir  = strdup(base);

	if (switch_core_init(SCF_NONE, SWITCH_FALSE, &err) != SWITCH_STATUS_SUCCESS) {
		fprintf(stderr, "core init failed: %s\n", err ? err : "?");
		return 1;
	}
	fprintf(stderr, "FreeSWITCH core initialised\n");
	/* surface FreeSWITCH warnings/errors (call-flow problems, codec faults) */
	switch_log_bind_logger(hlog, SWITCH_LOG_WARNING, SWITCH_FALSE);

	/* switch_core_init() leaves SCF_NO_NEW_SESSIONS set (normally cleared by
	 * the full startup path); clear it so sessions can be created. */
	{ int paused = 0; switch_core_session_ctl(SCSC_PAUSE_ALL, &paused); }

	switch_loadable_module_init(SWITCH_FALSE);
	/* switch_loadable_module_init(autoload=FALSE) returns before pre-loading
	 * the built-in core modules, so the PCMA codec and the "soft" timer used
	 * by mod_isup's media path are absent. Load them explicitly. (A full
	 * FreeSWITCH loads these for us — this is only needed in the harness.) */
	switch_loadable_module_load_module((char *)"", (char *)"CORE_SOFTTIMER_MODULE", SWITCH_TRUE, &err);
	switch_loadable_module_load_module((char *)"", (char *)"CORE_PCM_MODULE", SWITCH_TRUE, &err);
	if (switch_loadable_module_load_module(
		    (char *)SWITCH_GLOBAL_dirs.mod_dir,
		    (char *)"mod_isup", SWITCH_TRUE, &err) != SWITCH_STATUS_SUCCESS) {
		fprintf(stderr, "load mod_isup failed: %s\n", err ? err : "?");
		return 1;
	}
	fprintf(stderr, "mod_isup loaded\n");

	/* dialplan + applications, so inbound ISUP calls route via the XML
	 * dialplan (answer/ring_ready/playback/...) instead of in-module. */
	switch_loadable_module_load_module((char *)SWITCH_GLOBAL_dirs.mod_dir,
		(char *)"mod_dialplan_xml", SWITCH_TRUE, &err);
	switch_loadable_module_load_module((char *)SWITCH_GLOBAL_dirs.mod_dir,
		(char *)"mod_dptools", SWITCH_TRUE, &err);

	/* Let the M3UA association come up and the start-up group reset
	 * (Q.764 §2.9.1) converge before any call is placed. Configurable via
	 * ISUP_PRESLEEP so a test can also originate *during* the reset window
	 * and observe the origination barrier reject the call. */
	{ const char *ps = getenv("ISUP_PRESLEEP"); sleep(ps ? (unsigned)atoi(ps) : 9); }

	/* OAM snapshot once the association is up */
	{
		switch_stream_handle_t s = { 0 };
		SWITCH_STANDARD_STREAM(s);
		switch_api_execute("isup", "status", NULL, &s);
		fprintf(stderr, "=== isup status ===\n%s\n", s.data ? (char *)s.data : "");
		switch_safe_free(s.data);
	}

	if (argc > 3 && !strcmp(argv[2], "originate")) {
		switch_core_session_t *bleg = NULL;
		switch_call_cause_t cause = SWITCH_CAUSE_NONE;
		switch_status_t s;
		char dest[256];
		const char *vars = getenv("ISUP_ORIG_VARS");   /* {var=val,...} for the IAM */
		if (vars && *vars)
			snprintf(dest, sizeof(dest), "{%s}isup/lab/%s", vars, argv[3]);
		else
			snprintf(dest, sizeof(dest), "isup/lab/%s", argv[3]);
		fprintf(stderr, ">>> ORIGINATE %s\n", dest);
		s = switch_ivr_originate(NULL, &bleg, &cause, dest, 20,
					 NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
		fprintf(stderr, ">>> ORIGINATE result: status=%d cause=%s\n",
			(int)s, switch_channel_cause2str(cause));
		if (s == SWITCH_STATUS_SUCCESS && bleg) {
			/* Pump the answered leg: read frames for ~3s so the originating
			 * side's RTP engages and we receive the audio the far end (fs-b)
			 * is generating, bridged through the osmo-mgw. Count received
			 * media frames as proof of through-connected audio. */
			int loops = 0, media = 0;
			switch_frame_t *rf = NULL;
			fprintf(stderr, ">>> CALL ANSWERED — pumping media ~3s\n");
			while (loops++ < 150 && switch_channel_ready(switch_core_session_get_channel(bleg))) {
				if (switch_core_session_read_frame(bleg, &rf, SWITCH_IO_FLAG_NONE, 0) != SWITCH_STATUS_SUCCESS)
					break;
				if (rf && rf->datalen && !switch_test_flag(rf, SFF_CNG)) media++;
			}
			fprintf(stderr, ">>> MEDIA: received %d non-CNG frames on originating leg\n", media);
			switch_channel_hangup(switch_core_session_get_channel(bleg),
					      SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(bleg);
		}
	}

	sleep(secs);
	signal(SIGALRM, hard_exit);
	alarm(8);                 /* bound the core teardown */
	switch_core_destroy();
	_exit(0);                 /* don't linger even if destroy returns */
}
