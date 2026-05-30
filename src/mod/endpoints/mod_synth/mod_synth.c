/*
 * mod_synth.c -- Minimal synthetic endpoint for FreeSWITCH
 *
 * Originating synth/<name> creates a synthetic channel that:
 *   - is instantly answerable,
 *   - feeds 20ms L16/8000 audio frames on read (timer-paced), looping a
 *     short "beep every second" tone as the audio source,
 *   - accepts and discards all write frames,
 *   - works with any dialplan application that needs a real channel.
 *
 * The string after "synth/" is used only as the channel name; the module
 * sets no channel variables of its own from it.
 *
 * Optional channel variables (settable via [key=value] originate prefix):
 *   synth_timeout=<int seconds>   hang up the channel after N seconds,
 *                                 regardless of bridge state.
 *
 *   originate synth/test &park()
 *   originate synth/test &callcenter(my_queue)
 *   originate [synth_timeout=30]synth/test &bridge(user/1000)
 *   originate synth/test 1000 XML default
 */

/* Read path generates the audio in C -- 800 Hz square wave for 200 ms,
   then 800 ms silence, looping every second. Avoids depending on the FS
   file API (which had EOF/seek edge cases for tone_stream://). */
#define SYNTH_SAMPLE_RATE   8000   /* Hz */
#define SYNTH_SAMPLES_20MS  160
#define SYNTH_TONE_HZ       800
#define SYNTH_TONE_AMP      16000
#define SYNTH_CYCLE_SAMPLES (SYNTH_SAMPLE_RATE)             /* 1 second cycle */
#define SYNTH_TONE_SAMPLES  (SYNTH_SAMPLE_RATE / 5)         /* 200 ms tone */
#define SYNTH_HALFCYCLE     (SYNTH_SAMPLE_RATE / SYNTH_TONE_HZ / 2)  /* square */

#include <switch.h>

SWITCH_MODULE_LOAD_FUNCTION(mod_synth_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_synth_shutdown);
SWITCH_MODULE_DEFINITION(mod_synth, mod_synth_load, mod_synth_shutdown, NULL);

static switch_endpoint_interface_t *synth_endpoint_interface = NULL;

typedef enum {
	TFLAG_BREAK = (1 << 0),
	TFLAG_KILL  = (1 << 1)
} TFLAGS;

struct private_object {
	switch_core_session_t   *session;
	switch_channel_t        *channel;
	switch_caller_profile_t *caller_profile;
	switch_codec_t           read_codec;
	switch_codec_t           write_codec;
	switch_timer_t           timer;
	switch_frame_t           read_frame;
	uint8_t                  databuf[SWITCH_RECOMMENDED_BUFFER_SIZE];
	switch_mutex_t          *mutex;
	uint32_t                 flags;

	/* Beep generator state: current position within the 1-second cycle
	   (0..SYNTH_CYCLE_SAMPLES-1). channel_read_frame advances this by
	   SYNTH_SAMPLES_20MS on every tick. */
	uint32_t                 beep_pos;

	/* Optional hard deadline in microseconds (0 = disabled). When the wall
	   clock crosses this point, channel_read_frame hangs up the channel. */
	switch_time_t            deadline_us;
};
typedef struct private_object private_t;


/* --- state handlers ----------------------------------------------------- */

static switch_status_t channel_on_init(switch_core_session_t *session)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);

	switch_channel_set_flag(channel, CF_AUDIO);
	switch_channel_set_flag(channel, CF_ACCEPT_CNG);

	/* Returning SUCCESS without changing state lets the core's standard INIT
	   handler advance us to CS_ROUTING. The standard ROUTING handler will
	   then attach the inline &app() (queued by originate) as the caller
	   extension before transitioning to CS_EXECUTE -- calling set_state
	   here would skip that, leaving CS_EXECUTE with no extension to run. */
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_consume_media(switch_core_session_t *session)
{
	/* CS_CONSUME_MEDIA is the state outbound channels park in after CS_ROUTING
	   when there is no queued extension -- i.e. the post-answer-app originate
	   forms like `originate synth/x 'app:args' inline` (vs. `&app()` which
	   queues an extension and goes straight to CS_EXECUTE). The originate
	   blocks here waiting for the channel to be answered, so do it now. */
	switch_channel_mark_answered(switch_core_session_get_channel(session));
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_destroy(switch_core_session_t *session)
{
	private_t *tech_pvt = switch_core_session_get_private(session);

	if (!tech_pvt) {
		return SWITCH_STATUS_SUCCESS;
	}

	if (tech_pvt->timer.timer_interface) {
		switch_core_timer_destroy(&tech_pvt->timer);
	}
	if (switch_core_codec_ready(&tech_pvt->read_codec)) {
		switch_core_codec_destroy(&tech_pvt->read_codec);
	}
	if (switch_core_codec_ready(&tech_pvt->write_codec)) {
		switch_core_codec_destroy(&tech_pvt->write_codec);
	}

	return SWITCH_STATUS_SUCCESS;
}


/* --- I/O ---------------------------------------------------------------- */

static switch_status_t channel_kill_channel(switch_core_session_t *session, int sig)
{
	switch_channel_t *channel  = switch_core_session_get_channel(session);
	private_t        *tech_pvt = switch_core_session_get_private(session);

	if (!tech_pvt) {
		return SWITCH_STATUS_SUCCESS;
	}

	switch (sig) {
	case SWITCH_SIG_KILL:
		switch_mutex_lock(tech_pvt->mutex);
		tech_pvt->flags |= TFLAG_KILL;
		switch_mutex_unlock(tech_pvt->mutex);
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
		break;
	case SWITCH_SIG_BREAK:
		switch_mutex_lock(tech_pvt->mutex);
		tech_pvt->flags |= TFLAG_BREAK;
		switch_mutex_unlock(tech_pvt->mutex);
		break;
	default:
		break;
	}
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_send_dtmf(switch_core_session_t *session, const switch_dtmf_t *dtmf)
{
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_read_frame(switch_core_session_t *session, switch_frame_t **frame,
										  switch_io_flag_t flags, int stream_id)
{
	switch_channel_t *channel  = switch_core_session_get_channel(session);
	private_t        *tech_pvt = switch_core_session_get_private(session);

	*frame = NULL;

	if (!tech_pvt) {
		return SWITCH_STATUS_FALSE;
	}

	if (!switch_channel_ready(channel) || (tech_pvt->flags & TFLAG_KILL)) {
		return SWITCH_STATUS_FALSE;
	}

	if (tech_pvt->flags & TFLAG_BREAK) {
		switch_mutex_lock(tech_pvt->mutex);
		tech_pvt->flags &= ~TFLAG_BREAK;
		switch_mutex_unlock(tech_pvt->mutex);

		/* Return one CNG frame so the read loop unblocks cleanly. */
		((uint8_t *)tech_pvt->read_frame.data)[0] = 0;
		((uint8_t *)tech_pvt->read_frame.data)[1] = 0;
		tech_pvt->read_frame.flags   = SFF_CNG;
		tech_pvt->read_frame.datalen = 2;
		tech_pvt->read_frame.samples = 0;
		tech_pvt->read_frame.codec   = &tech_pvt->read_codec;
		*frame = &tech_pvt->read_frame;
		return SWITCH_STATUS_SUCCESS;
	}

	/* 20 ms pacing -- without this the read thread spins at 100% CPU. */
	if (switch_core_timer_next(&tech_pvt->timer) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	if (!switch_channel_ready(channel) || (tech_pvt->flags & TFLAG_KILL)) {
		return SWITCH_STATUS_FALSE;
	}

	/* Hard deadline (synth_timeout). Checked here because read_frame fires
	   every 20 ms whether the channel is parked, bridged, or running an
	   inline app -- so a single check suffices for all of them. */
	if (tech_pvt->deadline_us && switch_micro_time_now() >= tech_pvt->deadline_us) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
						  "mod_synth: synth_timeout reached, hanging up\n");
		switch_channel_hangup(channel, SWITCH_CAUSE_ALLOTTED_TIMEOUT);
		return SWITCH_STATUS_FALSE;
	}

	{
		int16_t *out = (int16_t *)tech_pvt->databuf;
		uint32_t pos = tech_pvt->beep_pos;
		int      i;

		for (i = 0; i < SYNTH_SAMPLES_20MS; i++) {
			if (pos < SYNTH_TONE_SAMPLES) {
				out[i] = ((pos / SYNTH_HALFCYCLE) & 1) ? -SYNTH_TONE_AMP : SYNTH_TONE_AMP;
			} else {
				out[i] = 0;
			}
			pos++;
			if (pos >= SYNTH_CYCLE_SAMPLES) {
				pos = 0;
			}
		}
		tech_pvt->beep_pos = pos;
	}

	tech_pvt->read_frame.flags    = SFF_NONE;
	tech_pvt->read_frame.codec    = &tech_pvt->read_codec;
	tech_pvt->read_frame.datalen  = SYNTH_SAMPLES_20MS * 2;
	tech_pvt->read_frame.samples  = SYNTH_SAMPLES_20MS;
	tech_pvt->read_frame.rate     = SYNTH_SAMPLE_RATE;
	tech_pvt->read_frame.channels = 1;

	*frame = &tech_pvt->read_frame;
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_write_frame(switch_core_session_t *session, switch_frame_t *frame,
										   switch_io_flag_t flags, int stream_id)
{
	private_t *tech_pvt = switch_core_session_get_private(session);

	if (!tech_pvt || (tech_pvt->flags & TFLAG_KILL)) {
		return SWITCH_STATUS_FALSE;
	}
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_receive_message(switch_core_session_t *session, switch_core_session_message_t *msg)
{
	switch_channel_t *channel  = switch_core_session_get_channel(session);
	private_t        *tech_pvt = switch_core_session_get_private(session);

	if (!tech_pvt) {
		return SWITCH_STATUS_SUCCESS;
	}

	switch (msg->message_id) {
	case SWITCH_MESSAGE_INDICATE_ANSWER:
		switch_channel_mark_answered(channel);
		break;
	case SWITCH_MESSAGE_INDICATE_BRIDGE:
	case SWITCH_MESSAGE_INDICATE_AUDIO_SYNC:
		/* Resync the timer so the first read after a bridge transition doesn't
		   come back early because the timer thinks ticks were missed. */
		if (tech_pvt->timer.timer_interface) {
			switch_core_timer_sync(&tech_pvt->timer);
		}
		break;
	case SWITCH_MESSAGE_INDICATE_UNBRIDGE:
		if (tech_pvt->timer.timer_interface) {
			switch_core_timer_sync(&tech_pvt->timer);
		}
		/* Honor the standard hangup_after_bridge convention. FS normally
		   handles this in the `bridge` dialplan app's post-bridge wrapper,
		   so endpoints don't have to — but synth often gets bridged via
		   paths that don't go through that wrapper (e.g. mod_callcenter
		   calls switch_ivr_uuid_bridge directly). Without this, a synth
		   caller set up with hangup_after_bridge=true keeps living after
		   the bridge ends and gets re-dispatched by callcenter, creating
		   phantom calls. */
		if (switch_true(switch_channel_get_variable(channel, "hangup_after_bridge"))) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
							  "mod_synth: hangup_after_bridge set and bridge ended, hanging up\n");
			switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
		}
		break;
	default:
		break;
	}
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_receive_event(switch_core_session_t *session, switch_event_t *event)
{
	return SWITCH_STATUS_SUCCESS;
}


/* --- outgoing channel --------------------------------------------------- */

static switch_status_t synth_tech_init(private_t *tech_pvt, switch_core_session_t *session)
{
	switch_memory_pool_t *pool = switch_core_session_get_pool(session);

	/* Codecs and timer MUST be initialised here, in the originating thread,
	   before the session thread starts and the core requests any frame. */
	if (switch_core_codec_init(&tech_pvt->read_codec,
							   "L16", NULL, NULL,
							   8000, 20, 1,
							   SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
							   NULL, pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
						  "mod_synth: read codec init failed\n");
		return SWITCH_STATUS_FALSE;
	}

	if (switch_core_codec_init(&tech_pvt->write_codec,
							   "L16", NULL, NULL,
							   8000, 20, 1,
							   SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
							   NULL, pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
						  "mod_synth: write codec init failed\n");
		switch_core_codec_destroy(&tech_pvt->read_codec);
		return SWITCH_STATUS_FALSE;
	}

	if (switch_core_session_set_read_codec(session, &tech_pvt->read_codec) != SWITCH_STATUS_SUCCESS ||
		switch_core_session_set_write_codec(session, &tech_pvt->write_codec) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
						  "mod_synth: set codec on session failed\n");
		switch_core_codec_destroy(&tech_pvt->read_codec);
		switch_core_codec_destroy(&tech_pvt->write_codec);
		return SWITCH_STATUS_FALSE;
	}

	if (switch_core_timer_init(&tech_pvt->timer, "soft", 20, 160, pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
						  "mod_synth: timer init failed\n");
		switch_core_codec_destroy(&tech_pvt->read_codec);
		switch_core_codec_destroy(&tech_pvt->write_codec);
		return SWITCH_STATUS_FALSE;
	}

	/* Pre-fill the read frame so channel_read_frame is allocation-free. The
	   actual PCM is generated in channel_read_frame on every tick. */
	tech_pvt->read_frame.data     = tech_pvt->databuf;
	tech_pvt->read_frame.buflen   = sizeof(tech_pvt->databuf);
	tech_pvt->read_frame.codec    = &tech_pvt->read_codec;
	tech_pvt->read_frame.datalen  = SYNTH_SAMPLES_20MS * 2;
	tech_pvt->read_frame.samples  = SYNTH_SAMPLES_20MS;
	tech_pvt->read_frame.rate     = SYNTH_SAMPLE_RATE;
	tech_pvt->read_frame.channels = 1;
	tech_pvt->read_frame.flags    = SFF_NONE;
	tech_pvt->beep_pos            = 0;
	memset(tech_pvt->databuf, 0, sizeof(tech_pvt->databuf));

	switch_mutex_init(&tech_pvt->mutex, SWITCH_MUTEX_NESTED, pool);
	tech_pvt->session = session;
	tech_pvt->channel = switch_core_session_get_channel(session);

	switch_core_session_set_private(session, tech_pvt);
	return SWITCH_STATUS_SUCCESS;
}

static switch_call_cause_t channel_outgoing_channel(switch_core_session_t *session,
													switch_event_t *var_event,
													switch_caller_profile_t *outbound_profile,
													switch_core_session_t **new_session,
													switch_memory_pool_t **pool,
													switch_originate_flag_t flags,
													switch_call_cause_t *cancel_cause)
{
	switch_core_session_t   *nsession = NULL;
	private_t               *tech_pvt = NULL;
	switch_channel_t        *channel  = NULL;
	switch_caller_profile_t *caller_profile;
	const char              *use_uuid;
	const char              *dest;
	char                     name[128];

	if (!outbound_profile || zstr(outbound_profile->destination_number)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "mod_synth: missing destination number\n");
		return SWITCH_CAUSE_INVALID_NUMBER_FORMAT;
	}

	use_uuid = var_event ? switch_event_get_header(var_event, "origination_uuid") : NULL;

	nsession = switch_core_session_request_uuid(synth_endpoint_interface,
												SWITCH_CALL_DIRECTION_OUTBOUND,
												flags, pool, use_uuid);
	if (!nsession) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT,
						  "mod_synth: session request failed\n");
		return SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
	}

	switch_core_session_add_stream(nsession, NULL);

	tech_pvt = switch_core_session_alloc(nsession, sizeof(*tech_pvt));
	if (!tech_pvt) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(nsession), SWITCH_LOG_CRIT,
						  "mod_synth: tech_pvt alloc failed\n");
		switch_core_session_destroy(&nsession);
		return SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
	}
	memset(tech_pvt, 0, sizeof(*tech_pvt));

	channel = switch_core_session_get_channel(nsession);
	dest    = outbound_profile->destination_number;

	switch_snprintf(name, sizeof(name), "synth/%s", dest);
	switch_channel_set_name(channel, name);

	if (synth_tech_init(tech_pvt, nsession) != SWITCH_STATUS_SUCCESS) {
		switch_core_session_destroy(&nsession);
		return SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
	}

	/* Optional synth_timeout=<seconds>. Plain integer -- no variable
	   substitution -- so we don't trip the originate's var_check post-pass. */
	{
		const char *timeout_str = var_event
			? switch_event_get_header(var_event, "synth_timeout") : NULL;

		if (!zstr(timeout_str)) {
			int seconds = atoi(timeout_str);
			if (seconds > 0) {
				tech_pvt->deadline_us = switch_micro_time_now()
					+ ((switch_time_t)seconds * 1000000);
			}
		}
	}

	caller_profile = switch_caller_profile_clone(nsession, outbound_profile);
	caller_profile->source = switch_core_strdup(caller_profile->pool, "mod_synth");
	switch_channel_set_caller_profile(channel, caller_profile);
	tech_pvt->caller_profile = caller_profile;

	switch_channel_set_state(channel, CS_INIT);

	*new_session = nsession;
	return SWITCH_CAUSE_SUCCESS;
}


/* --- interface tables --------------------------------------------------- */

static switch_state_handler_table_t synth_state_handlers = {
	/*.on_init           */ channel_on_init,
	/*.on_routing        */ NULL,
	/*.on_execute        */ NULL,
	/*.on_hangup         */ NULL,
	/*.on_exchange_media */ NULL,
	/*.on_soft_execute   */ NULL,
	/*.on_consume_media  */ channel_on_consume_media,
	/*.on_hibernate      */ NULL,
	/*.on_reset          */ NULL,
	/*.on_park           */ NULL,
	/*.on_reporting      */ NULL,
	/*.on_destroy        */ channel_on_destroy
};

static switch_io_routines_t synth_io_routines = {
	/*.outgoing_channel */ channel_outgoing_channel,
	/*.read_frame       */ channel_read_frame,
	/*.write_frame      */ channel_write_frame,
	/*.kill_channel     */ channel_kill_channel,
	/*.send_dtmf        */ channel_send_dtmf,
	/*.receive_message  */ channel_receive_message,
	/*.receive_event    */ channel_receive_event
};


/* --- load / shutdown ---------------------------------------------------- */

SWITCH_MODULE_LOAD_FUNCTION(mod_synth_load)
{
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	synth_endpoint_interface = switch_loadable_module_create_interface(*module_interface,
																	  SWITCH_ENDPOINT_INTERFACE);
	synth_endpoint_interface->interface_name = "synth";
	synth_endpoint_interface->io_routines    = &synth_io_routines;
	synth_endpoint_interface->state_handler  = &synth_state_handlers;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_synth loaded\n");
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_synth_shutdown)
{
	return SWITCH_STATUS_SUCCESS;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */
