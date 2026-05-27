# mod_synth — minimal synth endpoint for FreeSWITCH

`mod_synth` provides a synthetic FreeSWITCH endpoint that creates a real,
auto-answered channel with no SIP, no network, and no hardware behind it.
The channel hands back silence on read and discards everything written to
it. Use it as the left side of an `originate` whenever you would otherwise
have reached for `loopback/`.

```
originate synth/<name> &<application>(...)
```

The string after `synth/` is used only as the channel name. It has no
behavioural side effects — for audio or duration control, see the
`synth_playback` / `synth_timeout` variables below.

## Why this exists

`mod_callcenter` is a dialplan application, not an endpoint, so you cannot
`originate` to it — you can only run it on an existing channel. Today the
only synthetic option is `loopback/`, which:

- Creates four legs instead of two.
- Has variable-isolation quirks between the A/B legs.
- `mod_callcenter` explicitly sets `loopback_bowout=false` and
  `loopback_bowout_on_execute=false`, preventing leg collapse and
  producing inaccurate CDRs.

`mod_synth` skips all of that: one channel, one leg, no media negotiation.

## Build

### In-tree (with the FreeSWITCH source build)

`mod_synth` ships with a `Makefile.am` and is registered in
`build/modules.conf.in` and `configure.ac`. To enable it, uncomment the
`endpoints/mod_synth` line in `build/modules.conf.in` (or `sed` it in your
build pipeline) and run the normal FreeSWITCH build.

### Out-of-tree (standalone)

Requires a FreeSWITCH dev install whose `pkg-config` is on your `PATH`.

```sh
cd src/mod/endpoints/mod_synth
make -f Makefile.standalone
sudo make -f Makefile.standalone install   # copies mod_synth.so to moduledir
```

`make install` resolves the module directory via
`pkg-config --variable=moduledir freeswitch`. `Makefile.standalone` is
named that way so it doesn't clash with the `Makefile` that autotools
generates from `Makefile.am` during the in-tree build.

## Load

In `fs_cli`:

```
load mod_synth
```

To autoload at startup, add to `conf/autoload_configs/modules.conf.xml`:

```xml
<load module="mod_synth"/>
```

## Channel variables read by `mod_synth`

Set these via the `[key=value,...]` originate prefix (or `{key=value,...}`,
or `uuid_setvar` before the channel hits CS_INIT). Anything that
populates the outgoing channel's variables works.

| Variable        | Effect                                                              |
|-----------------|---------------------------------------------------------------------|
| `synth_playback` | Path or URI to play continuously from the synth side instead of silence. Loops on EOF. Accepts files (`/tmp/foo.wav`), `local_stream://moh`, `silence_stream://1400`, `tone_stream://...`, or anything else the FS file API can open. |
| `synth_timeout`  | Integer seconds. Once the channel has been alive that long, mod_synth hangs it up with cause `ALLOTTED_TIMEOUT`, even if it's bridged. |

Both values support `${chan_var}`, `$${global_var}`, and `${api(args)}`
substitution — mod_synth runs them through `switch_channel_expand_variables`
before using them. So this works straight from `fs_cli`:

```
bgapi originate {synth_playback=$${hold_music}}synth/test 'callcenter:support@ws-self' inline
```

`mod_synth` does **not** set any channel variables of its own. If you want
`hold_music` for bridge/hold flows, or `cc_moh_override` for mod_callcenter,
set them explicitly on the originate, e.g.
`{hold_music=local_stream://moh,cc_moh_override=local_stream://moh}`.

## Usage

### Basic

```
originate synth/test &park()
originate synth/test &echo()
```

### Audio applications

```
originate synth/test &playback(/path/to/file.wav)
originate synth/test &record(/tmp/recording.wav)
```

### Interactive applications

```
originate synth/test &conference(myconf)
originate synth/test &socket(127.0.0.1:8084 async full)
originate synth/test &lua(myscript.lua)
```

### Callcenter

```
originate synth/test &callcenter(my_queue)
```

If you want MOH played toward the queue member, set `cc_moh_override`
or rely on the queue's `moh` config:

```
originate {cc_moh_override=local_stream://moh}synth/test &callcenter(my_queue)
```

### Dialplan instead of an inline app

```
originate synth/test 1000 XML default
```

### Playing audio from the synth side / capping call duration

```
# Loop a WAV continuously from the synth side while the bridge is up.
originate [synth_playback=/usr/share/sounds/freeswitch/intro.wav]synth/test &park()

# Hang up after 30 seconds even if bridged.
originate [synth_timeout=30]synth/test &bridge(user/1000)

# Combine: play hold music for at most 2 minutes, then drop.
originate [synth_playback=local_stream://moh,synth_timeout=120]synth/test &callcenter(my_queue)
```

### With channel variables

```
originate {origination_uuid=custom-uuid,origination_caller_id_number=5551234}synth/test &callcenter(support@default)
```

### Lifecycle controls

`mod_synth` supports `uuid_kill`, `uuid_break`, and `uuid_setvar` like any
real endpoint:

```
uuid_kill   <uuid>
uuid_break  <uuid>
uuid_setvar <uuid> my_var hello
```

## What the channel actually does

- Acknowledges `INDICATE_ANSWER` by marking the channel answered.
- Reads return 20 ms frames at L16/8000/mono — silence by default, or
  audio from `synth_playback` if set, paced by a `soft` timer so the CPU
  stays idle.
- Writes are accepted and dropped.
- `BRIDGE` / `UNBRIDGE` / `AUDIO_SYNC` messages resync the timer so the
  first read after a bridge transition doesn't come back early.
- `SWITCH_SIG_BREAK` produces a single CNG frame so `uuid_break`
  interrupts blocking apps.
- `SWITCH_SIG_KILL` hangs the channel up with `NORMAL_CLEARING`.

## Known limitations

- No video support.
- No DTMF generation (DTMF send is a no-op).
- No media negotiation; the codec is fixed to L16/8000/20 ms mono.
- Without `synth_playback`, the read side is pure silence — there is
  nothing meaningful to record from a bare `synth/` leg.
