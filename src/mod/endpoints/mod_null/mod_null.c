/*
 * mod_null.c -- Minimal null endpoint for FreeSWITCH
 *
 * Originating null/<destination> creates a synthetic channel that:
 *   - is instantly answerable,
 *   - feeds 20ms L16/8000 silence frames on read (timer-paced),
 *   - accepts and discards all write frames,
 *   - works with any dialplan application that needs a real channel.
 *
 * The destination string after "null/" is stored in the channel variable
 * "null_destination". If it contains "://" it is also copied to "hold_music"
 * so bridge/park/hold flows pick it up. mod_callcenter has its own MOH
 * mechanism (cc_moh_override / queue config), independent of this.
 *
 *   originate null/local_stream://moh &callcenter(my_queue)
 *   originate null/test                &park()
 *   originate null/test 1000 XML default
 *
 * Patterned after the null sub-endpoint inside mod_loopback.c.
 */

#include <switch.h>

SWITCH_MODULE_LOAD_FUNCTION(mod_null_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_null_shutdown);
SWITCH_MODULE_DEFINITION(mod_null, mod_null_load, mod_null_shutdown, NULL);

static switch_endpoint_interface_t *null_endpoint_interface = NULL;

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
	char                    *destination;
};
typedef struct private_object private_t;


/* --- state handlers ----------------------------------------------------- */

static switch_status_t channel_on_init(switch_core_session_t *session)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);

	switch_channel_set_flag(channel, CF_AUDIO);
	switch_channel_set_flag(channel, CF_ACCEPT_CNG);

	switch_channel_set_state(channel, CS_ROUTING);
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_routing(switch_core_session_t *session)
{
	/* Belt-and-braces: explicitly drive CS_ROUTING -> CS_EXECUTE so that an
	   `originate null/... &app()` always reaches the inline application. */
	switch_channel_set_state(switch_core_session_get_channel(session), CS_EXECUTE);
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_execute(switch_core_session_t *session)
{
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

static switch_status_t channel_on_exchange_media(switch_core_session_t *session)
{
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_soft_execute(switch_core_session_t *session)
{
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_consume_media(switch_core_session_t *session)
{
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

	tech_pvt->read_frame.flags   = SFF_NONE;
	tech_pvt->read_frame.datalen = 320;  /* 160 samples * 2 bytes (L16) */
	tech_pvt->read_frame.samples = 160;
	tech_pvt->read_frame.codec   = &tech_pvt->read_codec;
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
	case SWITCH_MESSAGE_INDICATE_UNBRIDGE:
	case SWITCH_MESSAGE_INDICATE_AUDIO_SYNC:
		/* Resync the timer so the first read after a bridge transition doesn't
		   come back early because the timer thinks ticks were missed. */
		if (tech_pvt->timer.timer_interface) {
			switch_core_timer_sync(&tech_pvt->timer);
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

static switch_status_t null_tech_init(private_t *tech_pvt, switch_core_session_t *session)
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
						  "mod_null: read codec init failed\n");
		return SWITCH_STATUS_FALSE;
	}

	if (switch_core_codec_init(&tech_pvt->write_codec,
							   "L16", NULL, NULL,
							   8000, 20, 1,
							   SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
							   NULL, pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
						  "mod_null: write codec init failed\n");
		switch_core_codec_destroy(&tech_pvt->read_codec);
		return SWITCH_STATUS_FALSE;
	}

	if (switch_core_session_set_read_codec(session, &tech_pvt->read_codec) != SWITCH_STATUS_SUCCESS ||
		switch_core_session_set_write_codec(session, &tech_pvt->write_codec) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
						  "mod_null: set codec on session failed\n");
		switch_core_codec_destroy(&tech_pvt->read_codec);
		switch_core_codec_destroy(&tech_pvt->write_codec);
		return SWITCH_STATUS_FALSE;
	}

	if (switch_core_timer_init(&tech_pvt->timer, "soft", 20, 160, pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
						  "mod_null: timer init failed\n");
		switch_core_codec_destroy(&tech_pvt->read_codec);
		switch_core_codec_destroy(&tech_pvt->write_codec);
		return SWITCH_STATUS_FALSE;
	}

	/* Pre-fill the silence read frame so channel_read_frame is allocation-free. */
	tech_pvt->read_frame.data    = tech_pvt->databuf;
	tech_pvt->read_frame.buflen  = sizeof(tech_pvt->databuf);
	tech_pvt->read_frame.codec   = &tech_pvt->read_codec;
	tech_pvt->read_frame.datalen = 320;
	tech_pvt->read_frame.samples = 160;
	tech_pvt->read_frame.flags   = SFF_NONE;
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
						  "mod_null: missing destination number\n");
		return SWITCH_CAUSE_INVALID_NUMBER_FORMAT;
	}

	use_uuid = var_event ? switch_event_get_header(var_event, "origination_uuid") : NULL;

	nsession = switch_core_session_request_uuid(null_endpoint_interface,
												SWITCH_CALL_DIRECTION_OUTBOUND,
												flags, pool, use_uuid);
	if (!nsession) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT,
						  "mod_null: session request failed\n");
		return SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
	}

	switch_core_session_add_stream(nsession, NULL);

	tech_pvt = switch_core_session_alloc(nsession, sizeof(*tech_pvt));
	if (!tech_pvt) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(nsession), SWITCH_LOG_CRIT,
						  "mod_null: tech_pvt alloc failed\n");
		switch_core_session_destroy(&nsession);
		return SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
	}
	memset(tech_pvt, 0, sizeof(*tech_pvt));

	channel = switch_core_session_get_channel(nsession);
	dest    = outbound_profile->destination_number;

	tech_pvt->destination = switch_core_session_strdup(nsession, dest);
	switch_channel_set_variable(channel, "null_destination", tech_pvt->destination);

	if (strcasecmp(dest, "none") != 0 && strstr(dest, "://") != NULL) {
		switch_channel_set_variable(channel, SWITCH_HOLD_MUSIC_VARIABLE, dest);
	}

	switch_snprintf(name, sizeof(name), "null/%s", dest);
	switch_channel_set_name(channel, name);

	if (null_tech_init(tech_pvt, nsession) != SWITCH_STATUS_SUCCESS) {
		switch_core_session_destroy(&nsession);
		return SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
	}

	caller_profile = switch_caller_profile_clone(nsession, outbound_profile);
	caller_profile->source = switch_core_strdup(caller_profile->pool, "mod_null");
	switch_channel_set_caller_profile(channel, caller_profile);
	tech_pvt->caller_profile = caller_profile;

	switch_channel_set_state(channel, CS_INIT);

	*new_session = nsession;
	return SWITCH_CAUSE_SUCCESS;
}


/* --- interface tables --------------------------------------------------- */

static switch_state_handler_table_t null_state_handlers = {
	/*.on_init           */ channel_on_init,
	/*.on_routing        */ channel_on_routing,
	/*.on_execute        */ channel_on_execute,
	/*.on_hangup         */ NULL,
	/*.on_exchange_media */ channel_on_exchange_media,
	/*.on_soft_execute   */ channel_on_soft_execute,
	/*.on_consume_media  */ channel_on_consume_media,
	/*.on_hibernate      */ NULL,
	/*.on_reset          */ NULL,
	/*.on_park           */ NULL,
	/*.on_reporting      */ NULL,
	/*.on_destroy        */ channel_on_destroy
};

static switch_io_routines_t null_io_routines = {
	/*.outgoing_channel */ channel_outgoing_channel,
	/*.read_frame       */ channel_read_frame,
	/*.write_frame      */ channel_write_frame,
	/*.kill_channel     */ channel_kill_channel,
	/*.send_dtmf        */ channel_send_dtmf,
	/*.receive_message  */ channel_receive_message,
	/*.receive_event    */ channel_receive_event
};


/* --- load / shutdown ---------------------------------------------------- */

SWITCH_MODULE_LOAD_FUNCTION(mod_null_load)
{
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	null_endpoint_interface = switch_loadable_module_create_interface(*module_interface,
																	  SWITCH_ENDPOINT_INTERFACE);
	null_endpoint_interface->interface_name = "null";
	null_endpoint_interface->io_routines    = &null_io_routines;
	null_endpoint_interface->state_handler  = &null_state_handlers;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_null loaded\n");
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_null_shutdown)
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
