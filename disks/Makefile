all: roms.dsk bas.dsk

.PHONY: roms.dsk
roms.dsk:
	decb dskini roms.dsk
	./ccctobin.py < "roms/Audio Spectrum Analyzer (1981) (26-3156) (Tandy) (Coco 1-2).ccc" > /tmp/SPECTRUM.CCC
	decb copy -2 -b /tmp/SPECTRUM.CCC roms.dsk,SPECTRUM.BIN
	./ccctobin.py < "roms/Canyon Climber (1982) (26-3089) (Tandy).ccc" > /tmp/CANYON.CCC
	decb copy -2 -b /tmp/CANYON.CCC roms.dsk,CANYON.BIN
	./ccctobin.py < "roms/Demon Attack (1984) (26-3099) (Tandy).ccc" > /tmp/DEMON.CCC
	decb copy -2 -b /tmp/DEMON.CCC roms.dsk,DEMON.BIN
	./ccctobin.py < "roms/Diagnostics (1980) (26-3019) (Tandy).ccc"  > /tmp/DIAG80.CCC
	decb copy -2 -b /tmp/DIAG80.CCC roms.dsk,DIAG80.BIN
	./ccctobin.py < "roms/Diagnostics v2.0 (1982) (26-3019) (Tandy).ccc"  > /tmp/DIAG82.CCC
	decb copy -2 -b /tmp/DIAG82.CCC roms.dsk,DIAG82.BIN
	./ccctobin.py < "roms/Dungeons of Daggorath (1982) (26-3093) (Tandy).ccc"  > /tmp/DAGGOR.CCC
	decb copy -2 -b /tmp/DAGGOR.CCC roms.dsk,DAGGOR.BIN
	./ccctobin.py < "roms/EDTASM+ (1982) (26-3250) (Tandy).ccc"  > /tmp/EDTASM.CCC
	decb copy -2 -b /tmp/EDTASM.CCC roms.dsk,EDTASM.BIN
	./ccctobin.py < "roms/Fahrfall (Pandemic Edition) (John W. Linville).ccc"  > /tmp/FAHR.CCC
	decb copy -2 -b /tmp/FAHR.CCC roms.dsk,FAHR.BIN
	./ccctobin.py < "roms/Mega-Bug (1982) (26-3076) (Tandy).ccc"  > /tmp/MEGABUG.CCC
	decb copy -2 -b /tmp/MEGABUG.CCC roms.dsk,MEGABUG.BIN
	./ccctobin.py < "roms/Monster Maze (1983) (26-3081) (Tandy).ccc"  > /tmp/MMAZE.CCC
	decb copy -2 -b /tmp/MMAZE.CCC roms.dsk,MMAZE.BIN
	./ccctobin.py < "roms/Pinball (1980) (26-3052) (Tandy).ccc"  > /tmp/PINBALL.BIN
	decb copy -2 -b /tmp/PINBALL.BIN roms.dsk,PINBALL.BIN
	./ccctobin.py < "roms/Shooting Gallery (1982) (26-3088) (Tandy).ccc"  > /tmp/SHOOT.BIN
	decb copy -2 -b /tmp/SHOOT.BIN roms.dsk,SHOOT.BIN
	./ccctobin.py < "roms/Spidercide (1983) (26-3049) (Tandy).ccc"  > /tmp/SPIDER.BIN
	decb copy -2 -b /tmp/SPIDER.BIN roms.dsk,SPIDER.BIN
	#decb copy -2 -b "roms/Super Pitfall (1988) (26-3171) (Tandy) (Coco 3).ccc" roms.dsk,SUPER.CCC
	#decb copy -2 -b "roms/Videotex v1.2 (1981) (26-2222) (Tandy).ccc" roms.dsk,VIDEO.CCC
	cp loadroms.bas /tmp && unix2dos /tmp/loadroms.bas && decb copy -0 -a "/tmp/loadroms.bas" roms.dsk,LOADROMS.BAS

.PHONY: bas.dsk
bas.dsk:
	decb dskini bas.dsk
	cp basic/SPEAK.BAS /tmp && unix2dos /tmp/SPEAK.BAS && decb copy -0 -a "/tmp/SPEAK.BAS" bas.dsk,SPEAK.BAS
	cp basic/SPEAKCC.BAS /tmp && unix2dos /tmp/SPEAKCC.BAS && decb copy -0 -a "/tmp/SPEAKCC.BAS" bas.dsk,SPEAKCC.BAS
	cp basic/TONE.BAS /tmp && unix2dos /tmp/TONE.BAS && decb copy -0 -a "/tmp/TONE.BAS" bas.dsk,TONE.BAS
	cp basic/SSCDEMO.BAS /tmp && unix2dos /tmp/SSCDEMO.BAS && decb copy -0 -a "/tmp/SSCDEMO.BAS" bas.dsk,SSCDEMO.BAS
	cp basic/SNDEFF.BAS /tmp && unix2dos /tmp/SNDEFF.BAS && decb copy -0 -a "/tmp/SNDEFF.BAS" bas.dsk,SNDEFF.BAS
	cp basic/ORGAN.BAS /tmp && unix2dos /tmp/ORGAN.BAS && decb copy -0 -a "/tmp/ORGAN.BAS" bas.dsk,ORGAN.BAS

dir-roms.dsk:
	decb dir roms.dsk

dir-bas.dsk:
	decb dir bas.dsk

