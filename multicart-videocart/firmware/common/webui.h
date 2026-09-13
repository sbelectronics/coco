#ifndef MULTICART_WEBUI_H
#define MULTICART_WEBUI_H

// Single self-contained page served at "/". No external assets: the
// cart may be on a network with no internet, and there is no room for
// a second request round-trip budget on a 100 kHz-UI device.
//
// MC_BOARD_TITLE (from the board's config.h) is spliced in via string-
// literal concatenation so each board's page identifies itself.

#include "config.h"

static const char webui_html[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content=\"width=device-width,initial-scale=1\">"
"<title>" MC_BOARD_TITLE "</title><style>"
"body{font:15px/1.45 system-ui,sans-serif;margin:0;background:#14161a;color:#e8e8e8}"
"header{background:#1d2027;padding:14px 18px;border-bottom:1px solid #2c313a}"
"h1{margin:0;font-size:17px;letter-spacing:.5px}"
"#st{color:#9aa4b2;font-size:13px;margin-top:4px}"
"main{padding:18px;max-width:820px;margin:0 auto}"
"table{width:100%;border-collapse:collapse}"
"td,th{padding:7px 8px;border-bottom:1px solid #262b33;text-align:left;font-size:14px}"
"th{color:#9aa4b2;font-weight:600;font-size:12px;text-transform:uppercase}"
"tr.act{background:#1b2a1e}"
"button{background:#2b6cb0;color:#fff;border:0;border-radius:5px;padding:5px 10px;cursor:pointer;font-size:13px}"
"button.d{background:#7a2c2c}button.s{background:#33383f}"
"button:disabled{opacity:.5;cursor:default}"
"input[type=text]{background:#0f1115;border:1px solid #333a44;color:#e8e8e8;border-radius:4px;padding:4px 6px;width:150px}"
"section{margin-top:26px}h2{font-size:14px;color:#9aa4b2;text-transform:uppercase;letter-spacing:.5px}"
"#msg{margin-top:10px;color:#7dd3a0;min-height:18px;font-size:13px}"
".drop{border:2px dashed #39404b;border-radius:8px;padding:20px;text-align:center;color:#9aa4b2}"
".drop.over{border-color:#2b6cb0;color:#e8e8e8}"
"</style></head><body>"
"<header><h1>" MC_BOARD_TITLE "</h1><div id=st>loading...</div></header><main>"
"<table id=t><thead><tr><th>Title<th>File<th>Size<th>Auto<th title='Wait for BASIC to finish booting, then start the cart like a physical insertion. ~2s slower; needed only by some cassette conversions.'>Ins<th></tr></thead><tbody id=b></tbody></table>"
"<div id=msg></div>"
"<section><h2>Machine</h2>"
"<button onclick=reset()>Reset (reload current cart)</button> "
"<button class=s onclick=basic()>Boot to Extended BASIC (no cart)</button></section>"
"<section><h2>Settings</h2>"
"<label><input type=checkbox id=wifi> WiFi radio enabled</label>"
"<div style=margin-top:8px><label>Strobe delay "
"<input type=text id=strobe style=width:70px> ns</label></div>"
// Applied the moment it is saved, unlike WiFi/strobe. The correct phase
// is whichever one makes a given game look right, and that is a judgement
// made by eye -- so trying the other one must not cost a reboot.
"<div style=margin-top:8px><label>PMODE 4 artifact colour "
"<select id=artifact style=width:150px>"
"<option value=0>Off (black &amp; white)</option>"
"<option value=1>Phase A</option>"
"<option value=2>Phase B (default)</option>"
"</select></label>"
"<div style=\"color:#888;font-size:12px;margin-top:4px\">"
"Applies immediately. PMODE 4 games (Zaxxon, Donkey King) get their colour "
"from an NTSC artifact whose phase is arbitrary on real hardware &mdash; if "
"the colours look swapped, use the other phase.</div></div>"
"<div style=margin-top:8px><label>HDMI audio "
"<select id=hdmiaudio style=width:150px>"
"<option value=1>On (HDMI + sound)</option>"
"<option value=0>Off (plain DVI)</option>"
"</select></label> "
// Both judged by ear, both applied the moment they are saved, and both
// PERSISTED -- a setting that has to be re-chosen after every reboot is a
// trap.
"<div style=margin-top:8px><label>Analogue output filter "
"<select id=audiofilter style=width:170px>"
"<option value=0>Off (raw ladder)</option>"
"<option value=1>Gentle (default)</option>"
"<option value=2>Medium</option>"
"<option value=3>Heavy</option>"
"</select></label> "
"<label style=margin-left:12px>Single-bit level "
"<select id=audioonebit style=width:90px>"
"<option value=0>Off</option><option value=1>Quarter</option>"
"<option value=2>Half</option><option value=3>Full</option>"
"</select></label>"
"<div style=\"color:#888;font-size:12px;margin-top:4px\">"
"A CoCo playing a six-step waveform emits mostly harmonics, and its own "
"audio path removes them before they reach a speaker &mdash; so the raw "
"ladder sounds harsher than the real machine, not more faithful. "
"Single-bit level sets how loud PIA1 PB1 is against the 6-bit ladder; on "
"real hardware it joins through a resistor and is quieter than a full "
"ladder swing.</div></div>"
"<div style=margin-top:8px><label style=margin-left:0>Gain "

"<select id=audiogain style=width:70px>"
"<option value=0>-3</option><option value=1>-2</option>"
"<option value=2>0</option><option value=3>+1</option>"
"</select></label></div>"
"<div style=\"color:#888;font-size:12px;margin-top:4px\">"
"CoCo sound is reconstructed from the bus ($FF20 DAC, $FF22 single-bit) and "
"sent as HDMI audio. Gain applies immediately; the On/Off switch is staged "
"and needs a reboot, because it changes the scanout descriptor layout. "
"Turn it off if a display refuses the HDMI signalling.</div></div>"
"<div style=margin-top:10px>"
"<button class=s onclick=setsave(0)>Save</button> "
"<button onclick=setsave(1)>Save &amp; Reboot</button> "
// Opens the live display buffer as hex in a new tab. Ctrl-S there saves
// it; the format is what the coco-hdmi NTSC model script reads directly.
"<button onclick=\"window.open('/api/screen','_blank')\">Dump screen</button> "
// The last ~1024 audio-register writes, decoded, with the sound state
// each one produced -- for reading what a program actually wrote.
"<button onclick=\"window.open('/api/audiolog','_blank')\">Dump audio log</button> "
"<button onclick=\"window.open('/api/pcm','_blank')\">Dump PCM</button> "
// Bisects the audio chain in one click: the tone is generated from the
// sample counter alone, so if it is clean the HDMI side is exonerated.
"<button class=s onclick=atest()>Test tone: off / 1 kHz / 500 Hz</button> "
"<button class=s onclick=afilt()>Analogue filter: off / 1 / 2 / 3</button>"
"</div>"
"<div style=\"color:#e0b060;font-size:12px;margin-top:8px\">"
"WiFi and strobe delay changes are staged &mdash; they take effect only after "
"a reboot. Use Save &amp; Reboot to apply now (the cart restarts; wait a few "
"seconds and reload).</div></section>"
"<section><h2>Upload ROM</h2>"
"<div class=drop id=dz>Drop .rom / .bin / .ccc here, or "
"<input type=file id=f multiple></div></section>"
"<section><h2>Firmware</h2>"
"<input type=file id=fw accept=.bin> <button class=s onclick=ota()>Flash firmware</button>"
"<div style=\"color:#9aa4b2;font-size:12px;margin-top:6px\">"
"Upload a .bin built for this board. The cart reboots and copies the image; "
"do not remove power for ~15 s afterwards.</div></section>"
"<section><h2>Bus diagnostics</h2>"
"<div id=bus style=\"font:13px ui-monospace,monospace;color:#9aa4b2;"
"white-space:pre-line\">reading...</div></section>"
"<section><h2>About</h2>"
"<div style=\"color:#9aa4b2;font-size:13px\">"
MC_BOARD_TITLE " &middot; firmware v" MC_FW_VERSION "<br>"
"Dr. Scott M Baker &middot; "
"<a href=\"https://www.smbaker.com\" style=\"color:#7dd3a0\">www.smbaker.com</a>"
"</div></section>"
"</main><script>\n"
// Rows are built with createElement + closures — NO filename is ever
// interpolated into HTML or a JS string, so names with & + ' etc. are
// safe and there are no fragile id lookups.
"function msg(s){document.getElementById('msg').textContent=s}\n"
"async function load(){try{\n"
" let st=await (await fetch('/api/status')).json();\n"
" document.getElementById('st').textContent=\n"
"  'Playing: '+(st.active||'(none)')+'  |  '+Math.round(st.free/1024)+' KB free  |  fw '+st.fw;\n"
// Bus panel. Core 1's lap is paced by the bus, so its lap time IS the bus
// period when everything is healthy -- ~1117 ns means one lap per bus
// cycle. Computed here from two samples rather than by hand at the bench.
" if(st.bus){let u=st.bus,t=Date.now(),L=[];\n"
"  if(window._bp&&u.laps>window._bp.l){\n"
"   let ns=(t-window._bp.t)*1e6/(u.laps-window._bp.l);\n"
// Locked means WITHIN A FEW PERCENT of the 1117 ns bus cycle. Anything
// meaningfully above it means captures are being consumed slower than
// one per bus cycle (drops); the wait/body split below says whether the
// body is the reason.
"   L.push('lap       : '+ns.toFixed(0)+' ns'+((ns>1090&&ns<1150)?'   (locked to the ~1117 ns bus cycle)':(ns<=1090?'   <-- NOT EDGE-LOCKED':'   <-- CYCLES BEING MISSED (see cap drop + lap split)')));\n"
"  } else L.push('lap       : sampling...');\n"
"  window._bp={l:u.laps,t:t};\n"
// The PCF8574 drives !RESET and the CART gate. If it is not answering,
// selecting a cart cannot reset the CoCo and nothing downstream runs --
// so show it before any bus number, or a dead expander looks like a bus
// bug. P5 low = !RESET asserted, P6 low = CART gate on.
"  L.push('expander  : '+(u.xpok?'ok':'NOT RESPONDING')+\n"
"         '   pins 0x'+u.xppins.toString(16).padStart(2,'0')+\n"
"         '  (RESET '+((u.xppins&32)?'released':'ASSERTED')+\n"
"         ', CART gate '+((u.xppins&64)?'off':'ON')+')');\n"
"  L.push('serving   : '+(u.serving?'YES':'no (nothing mounted)'));\n"
// Window reads core 1 observed = serves the hardware chain was asked for.
// There is no CPU in the serve path to count what it delivered, so this
// is the demand side; svstall below is the supply side.
"  L.push('win reads : '+u.drives+(u.drives?'':'   <-- no cart reads seen'));\n"
"  L.push('writes    : '+u.writes);\n"
// ---- the zero-drop counters. Losing a byte on either pipeline is a
// defect, so these are pass/fail, not tuning knobs. Show them together
// and say plainly which pipeline each one indicts.
"  let z=[['svstall',u.svstall,'SERVE word lost (sampler FIFO full)'],\n"
"         ['pairdrop',u.pairdrop,'CAPTURED WRITE lost (core 0 pump stalled)'],\n"
"         ['tagslip',u.tagslip,'follower ring out of slot order']];\n"
// Lap anatomy: where each ~1117 ns bus cycle actually goes on core 1.
// body is the part we pay for; wait is idle blocking on the capture.
// body+wait should equal ~1117; body alone must stay well under it.
"  if(u.lbody)L.push('lap split : wait '+u.lwait+' + body '+u.lbody+\n"
"     ' ns (worst body '+u.lbmax+')'+(u.lbody>900?'   <-- BODY EATS THE CYCLE':''));\n"
// Serve-chain readback: the byte offset of the last table entry the DMA
// actually fetched. Must sit inside the table region; anything else
// means the sampler's pushed word is wrong on real silicon.
"  if(u.serving)L.push('svc look  : +0x'+u.svlook.toString(16)+\n"
"     (u.svlook<131072?'   (in table)':'   <-- OUTSIDE TABLES: sampler word bad'));\n"
"  if(u.serving)L.push('tbl check : '+(u.tblok?'verified on device':'MISMATCH <-- table build broken'));\n"
// Deepest the consumer ever ran into the 256-cycle snoop rings. Lag is
// HARMLESS (that is the whole point of index pairing) until it nears the
// ring depth, where the DMA would overwrite unconsumed history.
"  if(u.lbody)L.push('snoop lag : '+u.lag+' / 256 cycles worst'+(u.lag>192?'   <-- NEAR RING OVERFLOW':''));\n"
// The worst-ever figure above is pinned by any single spike, including
// one at cart start, so on its own it cannot say whether the ring is
// STILL full. These can: if lag now sits near the depth and laghi keeps
// climbing, the ring is saturated and dropping continuously; if lag now
// is small and laghi is static, the high-water mark is old news.
"  if(u.lagnow!==undefined)L.push('  lag now : '+u.lagnow+', '+u.laghi+' samples at >=3/4 depth'+\n"
"    (u.lagnow>192?'   <-- STILL SATURATED':(u.laghi?'   (spiked earlier, clear now)':'   (healthy)')));\n"
"  if(u.pairhi!==undefined)L.push('  write ring: '+u.pairhi+' / 2048 high-water'+(u.pairhi>1536?'   <-- FILLING':''));\n"
// Serve TX FIFO high-water. The serve SM pulls one word per window read;
// if a word ever lands after its cycle ended, the SM drives it in the
// NEXT cycle and stays permanently one behind -- stale bytes forever.
// That shows up as a persistently non-empty FIFO.
"  if(u.serving)L.push('svc fifo  : '+u.svfifo+' worst'+(u.svfifo>1?'   <-- SERVE SM RUNNING BEHIND (stale bytes)':'   (in step)'));\n"
// OTA. An upload that stages, reboots and then quietly declines to apply
// is invisible from the web page -- it has already said "rebooting" by
// then. owhy says what the LAST boot decided, and when it decided the
// image was corrupt it shows both crcs so the halves can be compared.
"  if(u.owhy){var ow=['','staged header length is absurd',\n"
"    'staged image FAILED its crc -- the upload did not land',\n"
"    'already running this image','applied'][u.owhy];\n"
"   L.push('ota       : last boot: '+ow+(u.olen?' ('+u.olen+' B)':''));\n"
"   if(u.owhy==2)L.push('            header crc 0x'+(u.ohcrc>>>0).toString(16)+\n"
"     ', image crc 0x'+(u.oicrc>>>0).toString(16));}\n"
"  if(u.owire&&u.owire!==u.orb)L.push('ota       : last upload wire crc 0x'+\n"
"    (u.owire>>>0).toString(16)+' but flash read back 0x'+(u.orb>>>0).toString(16)+\n"
"    '   <-- FLASH WRITE DID NOT TAKE');\n"
// A 6809 reset fetches its vector from $FFFE/$FFFF and nothing else reads
// that pair, so this counts machine resets exactly. It separates the two
// faults that both present as "it crashed": the machine being reset,
// versus a program deciding to return to its own menu.
"  if(u.rstv!==undefined)L.push('coco reset: '+u.rstv+' vector fetches'+\n"
"    (u.rstv>1?'   (the machine IS being reset)':' (power-on only)'));\n"
// Cold-boot stub. A selection that times out here never ran the stub,
// so the machine warm-started with $71 still set and BASIC's init
// skipped -- which insertion-style carts (Klendathu, Seadragon) cannot
// survive. Any nonzero "timed out" means selections are NOT getting a
// cold boot.
"  if(u.stubok!==undefined&&(u.stubok||u.stubto))L.push('cold boot : '+u.stubok+\n"
"    ' via stub, '+u.stubto+' timed out'+\n"
"    (u.stubto?'   (NO guaranteed cold boot -- BASIC warm-started)':''));\n"
// Split by cause. A guard is a group whose four tags did not read 0,1,2,3
// -- the two snoop streams genuinely slipped, and that cycle's write is
// gone. A retry is only a lap spent waiting to re-anchor afterwards. Many
// retries per guard means the re-anchor is slow, not that the bus is bad.
"  if(u.tguard!==undefined&&u.tagslip)L.push('slip cause: '+u.tguard+\n"
"    ' guard failures, '+u.tretry+' re-anchor laps'+\n"
"    (u.tguard?'   ('+(u.tretry/u.tguard).toFixed(1)+' laps lost per slip)':''));\n"
// Last tag-guard failure, decoded: ring word phase + the tag byte read.
// phase!=0 = the follower ring lost word alignment; phase 0 with rotated
// tags = the anchor mis-aimed. Only shown once a slip has happened.
"  if(u.tagslip)L.push('slip diag : ring phase '+((u.tagdiag>>8)&3)+', tags 0x'+(u.tagdiag&255).toString(16).padStart(2,'0'));\n"
// Anchor re-takes. A tick per cart change / RESET press is the recovery
// working as designed; continuous growth means the streams slip while
// running, which the tag guard alone cannot see.
"  if(u.afix||u.adist)L.push('anchor    : '+u.afix+' re-takes, last distance '+u.adist+' words'+(u.afix>1000?'   <-- CHURNING':''));\n"
// SAM map type, OBSERVED ONLY -- nothing acts on it. In type 1 the SAM
// makes $C000-$FEFF RAM and drops CTS*, so a real cart goes silent while
// our A14&A15 decode cannot tell. Flips here mean the running cart uses
// 64K mode and that gap is real; zero flips rules it out.
"  if(u.tyf)L.push('SAM maptyp: now '+u.ty+', '+u.tyf+' flips seen (observed only, not acted on)');\n"
// THE RENDERER'S OWN DECODE. Not what the cart's ROM says it programs --
// what we actually resolved from the bus and are drawing from right now.
// Read this against the ROM: if a cart programs base $0400 / RG6 and this
// line agrees, the fault is in the renderer's geometry, not in capture.
// If it disagrees, the write that differs names the exact register that
// went missing or landed wrong.
"  if(u.vfrm!==undefined){var f=u.vmode&255,v=u.vmode>>8,gm=(f>>4)&7;\n"
"    L.push('video : $FF22=0x'+f.toString(16)+' -> '+((f&128)?'graphics, gm='+gm+' ('+((gm&1)?'RG':'CG')+' tier '+((gm>>1)&3)+')':'alpha')+', CSS='+((f&8)?1:0));\n"
"    L.push('        SAM V='+v+', display base=$'+u.vbase.toString(16)+' ('+u.vbase+')');\n"
"    L.push('        '+u.vfrm+' frames rendered, '+u.vregw+' register writes applied');\n"
// We render whole frames at 60 Hz; a cart changing $FF22 many times per
// frame is doing a per-scanline effect, and we necessarily sample one
// arbitrary value out of that stream. That is a renderer limitation, not
// a dropped byte, and nothing in the bus path can fix it.
"    L.push('        $FF22 changes/frame: '+u.vrate+\n"
"      (u.vrate>8?'   <-- PER-SCANLINE EFFECT; one sample/frame cannot show it'\n"
"                :'   (stable within a frame)'));}\n"
// HDMI AUDIO. alate is the one that matters: it counts islands filled
// after their line had already been scanned, i.e. the just-in-time
// scheme failing to keep up. It must be 0 in steady state. aud drops
// are expected only during a flash erase, which holds the refill IRQ off
// for tens of ms.
"  if(u.aisl!==undefined){\n"
"    if(!u.aisl){L.push('audio  : OFF (plain DVI signalling)');}else{\n"
"    var d=u.astate>>8,g=(u.astate>>3)&1,mx=(u.astate>>1)&3,ob=u.astate&1;\n"
"    var mn=['DAC','cassette','cart SND','none'][mx];\n"
// Measured rate, not the nominal one. samples/laps is the only thing on
// this board that can say whether audio time really ran at 48 kHz on the
// hardware: laps is core 1's own bus-cycle count and the CoCo's E clock
// is 894886 Hz, so the quotient is the true sample rate. If this does not
// read ~48000 the ACR is lying to the sink and nothing downstream of it
// can sound right.
"    var hz=u.laps?Math.round(u.asmp*894886/u.laps):0;\n"
"    L.push('audio  : '+hz+' Hz measured, islands on '+u.aisl+' lines/frame, '+\n"
"      u.asmp+' samples, '+u.aev+' reg writes'+\n"
"      ((hz<47500||hz>48500)?'   <-- RATE IS WRONG, want 48000':''));\n"
"    L.push('         ring '+u.adep+'/'+u.aring+', '+u.ares+' resyncs, '+\n"
"      u.alate+' late, '+u.adrop+' dropped, '+u.anull+' idle slots'+\n"
"      (u.alate?'   <-- ISLANDS FILLED TOO LATE':''));\n"
// Seed the buttons from the FIRMWARE's state on every poll. The tone is
// deliberately not persisted, so it is off after any reboot -- including
// the one an OTA flash causes; a button that toggled from its own local
// idea of the state would send "off" on the first click after a flash.
"    _at=u.atest|0; _af=u.afilt|0;\n"
"    if(u.atest){L.push('         *** TEST TONE ON: '+(u.atest==2?'500 Hz':'1 kHz')+\n"
"      ' synthetic, CoCo audio bypassed ***');}\n"
"    if(u.ifix)L.push('         '+u.ifix+' island resyncs'+\n"
"      '   <-- REFILL IRQ IS BEING MISSED');\n"
// Two separate things, deliberately not summed. The GATE being off is the
// program turning sound off between notes -- normal, no fault at all. The
// MUX being away from the DAC is the analogue selector swept for a
// joystick read (it is shared with the joystick comparator), which
// genuinely chops the audio at the polling rate.
"    if(u.agate!==undefined)L.push('         sound gate off '+\n"
"      (u.agate/10).toFixed(1)+'% (program silencing between notes: normal)');\n"
"    if(u.amuxoff!==undefined)L.push('         mux off the DAC '+\n"
"      (u.amuxoff/10).toFixed(1)+'%, '+u.amux+' select edges'+\n"
"      (u.amuxoff>5?'   <-- joystick polling is chopping the audio':''));\n"
"    if(u.agedge!==undefined)L.push('         '+u.agedge+' gate edges, '+\n"
"      u.amux+' mux edges  (each one is a step in the output)');\n"
// The answer to "it sounds clipped", measured. If aclip is nonzero the
// arithmetic was wrong and the level must come down. If it is zero and
// the peak is well below 32767, what is being heard is the CoCo's own
// square waves or the ladder/single-bit balance, and no amount of
// turning the volume down will touch it.
"    if(u.apeak!==undefined){var db=u.apeak?(20*Math.log10(u.apeak/32767)):-99;\n"
"     L.push('         peak '+u.apeak+'/32767 ('+db.toFixed(1)+' dBFS), '+\n"
"      u.aclip+' clipped'+(u.aclip?'   <-- REALLY IS CLIPPING':''));}\n"
"    L.push('         analogue filter: '+\n"
"      ['off (raw ladder)','gentle','medium','heavy'][u.afilt||0]);\n"
"    L.push('         dac '+d+'  gate '+(g?'ON':'off')+'  mux '+mn+\n"
"      '  1-bit '+ob+(g&&mx==0?'':'   (silent: not gated to the DAC)'));}}\n"
// SERVE ECHO -- read this more carefully than its name suggests. It
// compares the table entry against the byte the CAPTURE pipeline sampled,
// so it measures serve AND capture together and cannot separate them. A
// mis-paired capture reports here as a wrong served byte that was never
// wrong on the bus. Cross-check against tagslip/lag before blaming the
// serve path: with tagslip non-zero these counts are not evidence of a
// serve fault, and a "wrong" byte that matches no table entry at all
// (neither address- nor permuted-index-adjacent) is a pairing artefact.
"  if(u.eok+u.ebad){let t=u.eok+u.ebad,p=100*u.ebad/t;\n"
"   L.push('svc/cap echo: '+u.ebad+' mismatch / '+t+' = '+p.toFixed(3)+'%'+\n"
"     (p>0.01?(u.tagslip?'   <-- but tagslip>0: likely PAIRING, not serving'\n"
"                       :'   <-- SERVING WRONG BYTES'):'   (bus carries what we owe)'));\n"
"   if(u.ebad)L.push('  last bad: $'+((u.ediag>>>16)&65535).toString(16).toUpperCase()+\n"
"     ' owed 0x'+((u.ediag>>8)&255).toString(16).padStart(2,'0')+\n"
"     ' got 0x'+(u.ediag&255).toString(16).padStart(2,'0'));}\n"
"  let drops=z.filter(x=>x[1]>0);\n"
"  if(drops.length){for(const x of drops)\n"
"     L.push('DROPPED   : '+x[0]+' = '+x[1]+'   <-- '+x[2]);}\n"
"  else L.push('dropped   : 0   (serve + capture both lossless)');\n"
// Stale captures dropped when the ring is resynced. A burst at boot is
// expected (the DMA fills the ring before core 1 starts). What matters is
// whether it is STILL growing, so show the delta, not just the total.
"  {let d=(window._bpc!==undefined)?u.caplate-window._bpc:-1;window._bpc=u.caplate;\n"
// The multicart has no capture ring and reports a constant 0, so annotate
// only once the counter has actually moved.
"   L.push('cap drop  : '+u.caplate+(d>0?'   <-- STILL GROWING (+'+d+')':(d===0&&u.caplate>0?'   (settled)':'')));}\n"
"  L.push('strobes   : '+u.strobes+'   bank '+u.bank);\n"
"  L.push('delay     : '+u.delay+' ns');\n"
"  document.getElementById('bus').textContent=L.join('\\n');\n"
" }\n"
" let b=document.getElementById('b');\n"
// Skip the rebuild only while a TEXT field is being edited (protects an
// in-progress title). A focused BUTTON must not block it, or the active
// highlight never updates after you click Play.
" let ae=document.activeElement;\n"
" if(ae&&ae.tagName==='INPUT'&&ae.type==='text'&&b.contains(ae))return;\n"
" let r=await (await fetch('/api/roms')).json();\n"
" b.innerHTML='';\n"
" for(const x of r.roms){\n"
"  let tr=document.createElement('tr');if(x.file===st.active)tr.className='act';\n"
"  let c1=document.createElement('td');\n"
"  let ti=document.createElement('input');ti.type='text';ti.value=x.title;c1.appendChild(ti);\n"
"  let c2=document.createElement('td');c2.style.color='#9aa4b2';c2.textContent=x.file;\n"
"  let c3=document.createElement('td');c3.textContent=Math.round(x.size/1024)+'K';\n"
"  let c4=document.createElement('td');\n"
"  let ck=document.createElement('input');ck.type='checkbox';ck.checked=!!x.autostart;c4.appendChild(ck);\n"
"  let c4b=document.createElement('td');\n"
"  let ci=document.createElement('input');ci.type='checkbox';ci.checked=!!x.insert;\n"
"  ci.title='Insertion-style start: let BASIC boot fully, then start the cart "
"like a physical insertion (~2s slower). Only some cassette conversions need it.';\n"
"  c4b.appendChild(ci);\n"
"  let c5=document.createElement('td');\n"
"  let bp=document.createElement('button');bp.textContent='Play';bp.onclick=()=>play(x.file);\n"
"  let bs=document.createElement('button');bs.textContent='Save';bs.className='s';bs.onclick=()=>save(x.file,ti.value,ck.checked,ci.checked);\n"
"  let bd=document.createElement('button');bd.textContent='Del';bd.className='d';bd.onclick=()=>del(x.file);\n"
"  c5.append(bp,' ',bs,' ',bd);\n"
"  tr.append(c1,c2,c3,c4,c4b,c5);b.appendChild(tr);\n"
" }\n"
"}catch(e){}}\n"
"async function play(f){msg('Selecting '+f+'...');\n"
" if(document.activeElement)document.activeElement.blur();try{\n"
" let r=await fetch('/api/select/'+encodeURIComponent(f),{method:'POST'});\n"
" msg(r.ok?('CoCo rebooting into '+f):('Select failed: '+await r.text()));\n"
"}catch(e){msg('Select failed: '+e)}setTimeout(load,2500)}\n"
"async function basic(){msg('Booting to BASIC...');\n"
" if(document.activeElement)document.activeElement.blur();try{\n"
" let r=await fetch('/api/basic',{method:'POST'});\n"
" msg(r.ok?'CoCo rebooting to Extended BASIC':('Failed: '+await r.text()));\n"
"}catch(e){msg('Failed: '+e)}setTimeout(load,2500)}\n"
"async function del(f){if(!confirm('Delete '+f+'?'))return;try{\n"
" let r=await fetch('/api/roms/'+encodeURIComponent(f),{method:'DELETE'});\n"
" if(!r.ok)msg('Delete failed: '+await r.text());\n"
"}catch(e){msg('Delete failed: '+e)}load()}\n"
"async function save(f,t,a,ins){try{\n"
" let r=await fetch('/api/meta/'+encodeURIComponent(f)+'?title='+encodeURIComponent(t)+'&autostart='+(a?1:0)+'&insert='+(ins?1:0),{method:'POST'});\n"
" msg(r.ok?('Saved '+f):('Save failed: '+await r.text()));\n"
"}catch(e){msg('Save failed: '+e)}load()}\n"
"async function up(files){for(const file of files){msg('Uploading '+file.name+'...');\n"
"  try{\n"
"   let r=await fetch('/api/roms/'+encodeURIComponent(file.name),{method:'POST',body:file});\n"
"   msg(r.ok?('Uploaded '+file.name):('Failed '+file.name+': '+await r.text()));\n"
"  }catch(e){msg('Failed '+file.name+': '+e)}\n"
" }load()}\n"
"document.getElementById('f').onchange=e=>up(e.target.files);\n"
"let dz=document.getElementById('dz');\n"
"dz.ondragover=e=>{e.preventDefault();dz.classList.add('over')};\n"
"dz.ondragleave=()=>dz.classList.remove('over');\n"
"dz.ondrop=e=>{e.preventDefault();dz.classList.remove('over');up(e.dataTransfer.files)};\n"
"window.addEventListener('dragover',e=>e.preventDefault());\n"  // a drop that
"window.addEventListener('drop',e=>e.preventDefault());\n"      // misses the
                                          // zone must not navigate away
"async function loadset(){try{let s=await (await fetch('/api/settings')).json();\n"
" document.getElementById('wifi').checked=!!s.wifi;\n"
" document.getElementById('strobe').value=s.strobe;\n"
" if(s.artifact!==undefined)document.getElementById('artifact').value=s.artifact;\n"
" if(s.hdmiaudio!==undefined)document.getElementById('hdmiaudio').value=s.hdmiaudio;\n"
" if(s.audiogain!==undefined)document.getElementById('audiogain').value=s.audiogain;\n"
" if(s.audiofilter!==undefined)document.getElementById('audiofilter').value=s.audiofilter;\n"
" if(s.audioonebit!==undefined)document.getElementById('audioonebit').value=s.audioonebit;\n"
"}catch(e){}}\n"
"async function setsave(rb){\n"
" let w=document.getElementById('wifi').checked?1:0;\n"
" let st=encodeURIComponent(document.getElementById('strobe').value);\n"
" let ar=encodeURIComponent(document.getElementById('artifact').value);\n"
" let ha=encodeURIComponent(document.getElementById('hdmiaudio').value);\n"
" let ag=encodeURIComponent(document.getElementById('audiogain').value);\n"
" let af=encodeURIComponent(document.getElementById('audiofilter').value);\n"
" let ob=encodeURIComponent(document.getElementById('audioonebit').value);\n"
" msg(rb?'Saving + rebooting...':'Saving...');try{\n"
"  let r=await fetch('/api/settings?wifi='+w+'&strobe='+st+'&artifact='+ar+'&hdmiaudio='+ha+'&audiogain='+ag+'&audiofilter='+af+'&audioonebit='+ob+'&reboot='+rb,{method:'POST'});\n"
"  msg(r.ok?(rb?'Saved - rebooting; wait a few seconds then reload'\n"
"            :'Saved (artifact colour applied now; WiFi/strobe need a reboot)')\n"
"          :('Save failed: '+await r.text()));\n"
"}catch(e){msg('Save failed: '+e)}}\n"
// Parameters go in the QUERY STRING: the API reads the URL, never the
// request body.
"var _at=0;\n"
"async function atest(){let w=(_at+1)%3;\n"
"  try{let r=await fetch('/api/atest?on='+w,{method:'POST'});\n"
"   let s=(await r.text()).trim();\n"
"   if(!r.ok){msg('Test tone failed: '+s);return;}\n"
"   _at=parseInt(s)||0;\n"          // believe the firmware, not the click
"   msg(_at?('Test tone ON ('+(_at==2?'500 Hz':'1 kHz')+', CoCo audio "
"bypassed)'):'Test tone off');}\n"
"  catch(e){msg('Test tone failed: '+e)}}\n"
"var _af=0;\n"
"async function afilt(){let w=(_af+1)%4;\n"
"  try{let r=await fetch('/api/afilt?n='+w,{method:'POST'});\n"
"   let s=(await r.text()).trim();\n"
"   if(!r.ok){msg('Filter failed: '+s);return;}\n"
"   _af=parseInt(s)||0;\n"
"   msg('Analogue filter: '+['off (raw ladder)','gentle','medium','heavy'][_af]);}\n"
"  catch(e){msg('Filter failed: '+e)}}\n"
"async function ota(){let f=document.getElementById('fw').files[0];if(!f)return;\n"
" if(!confirm('Flash new firmware ('+f.name+')?'))return;\n"
" msg('Uploading firmware...');try{\n"
" let r=await fetch('/api/ota',{method:'POST',body:f});\n"
" msg(r.ok?'Firmware staged - rebooting, wait ~15 s then reload'\n"
"         :('Firmware rejected: '+await r.text()));\n"
"}catch(e){msg('Firmware upload failed: '+e)}}\n"
"load();loadset();setInterval(load,3000);\n"
"</script></body></html>";

#endif
