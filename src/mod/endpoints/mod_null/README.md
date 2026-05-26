# mod_null — minimal null endpoint for FreeSWITCH

`mod_null` provides a synthetic FreeSWITCH endpoint that creates a real,
auto-answered channel with no SIP, no network, and no hardware behind it.
The channel hands back silence on read and discards everything written to
it. Use it as the left side of an `originate` whenever you would otherwise
have reached for `loopback/`.

```
originate null/<destination> &<application>(...)
```

## Why this exists

`mod_callcenter` is a dialplan application, not an endpoint, so you cannot
`originate` to it — you can only run it on an existing channel. Today the
only synthetic option is `loopback/`, which:

- Creates four legs instead of two.
- Has variable-isolation quirks between the A/B legs.
- `mod_callcenter` explicitly sets `loopback_bowout=false` and
  `loopback_bowout_on_execute=false`, preventing leg collapse and
  producing inaccurate CDRs.

`mod_null` skips all of that: one channel, one leg, no media negotiation.

## Build

### In-tree (with the FreeSWITCH source build)

`mod_null` ships with a `Makefile.am` and is registered in
`build/modules.conf.in` and `configure.ac`. To enable it, uncomment the
`endpoints/mod_null` line in `build/modules.conf.in` (or `sed` it in your
build pipeline) and run the normal FreeSWITCH build.

### Out-of-tree (standalone)

Requires a FreeSWITCH dev install whose `pkg-config` is on your `PATH`.

```sh
cd src/mod/endpoints/mod_null
make -f Makefile.standalone
sudo make -f Makefile.standalone install   # copies mod_null.so to moduledir
```

`make install` resolves the module directory via
`pkg-config --variable=moduledir freeswitch`. `Makefile.standalone` is
named that way so it doesn't clash with the `Makefile` that autotools
generates from `Makefile.am` during the in-tree build.

## Load

In `fs_cli`:

```
load mod_null
```

To autoload at startup, add to `conf/autoload_configs/modules.conf.xml`:

```xml
<load module="mod_null"/>
```

## Channel variables set by `mod_null`

| Variable           | Value                                                                 |
|--------------------|-----------------------------------------------------------------------|
| `null_destination` | The string after `null/` (e.g. `local_stream://moh`, `test`).         |
| `hold_music`       | Same as `null_destination`, but only when it contains `://` and is not empty / not `none`. Lets bridge/park/hold flows pick MOH up automatically. |

`mod_callcenter` uses its own `cc_moh_override` channel variable (or the
queue's `moh` config). To make a queue play the same MOH that the null leg
advertises, set it explicitly in the originate, e.g.
`{cc_moh_override=local_stream://moh}`.

## Usage

### Basic

```
originate null/test &park()
originate null/test &echo()
```

### Audio applications

```
originate null/test &playback(/path/to/file.wav)
originate null/test &record(/tmp/recording.wav)
```

### Interactive applications

```
originate null/test &conference(myconf)
originate null/test &socket(127.0.0.1:8084 async full)
originate null/test &lua(myscript.lua)
```

### Callcenter

```
originate null/local_stream://moh        &callcenter(my_queue)
originate null/silence_stream://1400     &callcenter(my_queue)
```

The first form auto-sets `hold_music=local_stream://moh` on the null leg.

### Dialplan instead of an inline app

```
originate null/test 1000 XML default
```

### With channel variables

```
originate {origination_uuid=custom-uuid,origination_caller_id_number=5551234}null/local_stream://moh &callcenter(support@default)
```

### Lifecycle controls

`mod_null` supports `uuid_kill`, `uuid_break`, and `uuid_setvar` like any
real endpoint:

```
uuid_kill   <uuid>
uuid_break  <uuid>
uuid_setvar <uuid> my_var hello
```

## What the channel actually does

- Auto-acknowledges `INDICATE_ANSWER` and `INDICATE_PROGRESS` by marking
  the channel answered.
- Reads return 20 ms frames of L16 silence (8 kHz, mono, 320 bytes),
  paced by a `soft` timer so the CPU stays idle.
- Writes are accepted and dropped.
- `BRIDGE` / `UNBRIDGE` messages are no-ops.
- `SWITCH_SIG_BREAK` produces a single CNG frame so `uuid_break`
  interrupts blocking apps.
- `SWITCH_SIG_KILL` and `INDICATE_KILL` tear the timer down so any
  parked reader exits immediately.

## Known limitations

- No video support.
- No DTMF generation (DTMF send is a no-op).
- No media negotiation; the codec is fixed to L16/8000/20 ms mono.
- The audio source is synthetic silence — there is nothing meaningful to
  record from the `null/` side.
