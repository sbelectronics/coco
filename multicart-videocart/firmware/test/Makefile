# ======================================================================
# Host-side simulation tests. Run on any Linux/gcc box:
#     cd firmware/test && make && make run
#
# Each test is one binary that #includes the firmware source under test
# directly (so statics are reachable) and links against the shim SDK in
# shim/. ASan+UBSan are on everywhere: an out-of-bounds framebuffer or
# ring write fails the suite even when the pixels happen to look right.
# ======================================================================

CC      ?= gcc
CFLAGS  += -O1 -g -Wall -Wextra -Wno-unused-parameter \
           -fsanitize=address,undefined -fno-sanitize-recover=all

COMMON  = ../common
MCART   = ../multicart
VCART   = ../videocart
LFS     = ../lib/littlefs

# Board config for each test: sweep/vdg/hstx are videocart-specific;
# romfs/select/ui are board-agnostic and use the multicart config.
INC_VC  = -I shim -I $(VCART) -I $(COMMON)
INC_MC  = -I shim -I $(MCART) -I $(COMMON)

TESTS   = test_sweep test_serve_pipeline test_sampler_timing \
          test_stub6809 test_hstx_frame test_vdg test_audio test_ui_vc test_cfg \
          test_romfs test_select test_ui

all: $(TESTS)

test_sweep: test_sweep.c $(VCART)/sweep.h $(VCART)/config.h
	$(CC) $(CFLAGS) $(INC_VC) -o $@ test_sweep.c

test_serve_pipeline: test_serve_pipeline.c $(VCART)/sweep.h $(VCART)/config.h
	$(CC) $(CFLAGS) $(INC_VC) -o $@ test_serve_pipeline.c

# The timing test decodes the REAL assembler output, so pioasm has to run
# first. PIOASM is located by the caller (see `make preflight`); if it is
# not found the test is a hard failure, never a silent skip -- these are
# the numbers that decide whether serving meets the 6809E deadline.
PIOASM ?= $(shell find $(HOME)/.cache/coco-fw /usr/local -name pioasm -type f 2>/dev/null | head -1)

gen_bus_pio.h: $(VCART)/bus_pio.pio
	@test -n "$(PIOASM)" || { echo "pioasm not found; set PIOASM=<path>"; exit 1; }
	$(PIOASM) -o c-sdk $(VCART)/bus_pio.pio $@.full
	@# Keep the instruction arrays and bus_pio_div(): the rest of the
	@# c-sdk output needs the real SDK headers. bus_pio_div is pulled in
	@# on purpose so test_sampler_timing exercises the firmware's OWN
	@# divider arithmetic -- that function is what keeps the sweep at its
	@# proven cycle timing when clk_sys is raised, and a test that
	@# restated the formula instead would pass while the board re-timed.
	@awk '/^static const uint16_t .*_program_instructions\[\]/,/^};/' \
	    $@.full > $@
	@awk '/^static inline float bus_pio_div/,/^}/' $@.full >> $@
	@rm -f $@.full
	@grep -q mux_sampler_program_instructions $@ || \
	    { echo "gen_bus_pio.h: sampler array missing"; exit 1; }
	@grep -q bus_pio_div $@ || \
	    { echo "gen_bus_pio.h: bus_pio_div missing"; exit 1; }

test_sampler_timing: test_sampler_timing.c gen_bus_pio.h
	$(CC) $(CFLAGS) $(INC_VC) -I . -o $@ test_sampler_timing.c

test_stub6809: test_stub6809.c $(COMMON)/stub6809.h
	$(CC) $(CFLAGS) -I $(COMMON) -o $@ test_stub6809.c

test_hstx_frame: test_hstx_frame.c $(VCART)/hstx.c $(VCART)/audio.c $(VCART)/video.h
	$(CC) $(CFLAGS) $(INC_VC) -o $@ test_hstx_frame.c

test_vdg: test_vdg.c $(VCART)/vdg.c $(VCART)/video.h $(VCART)/vdgfont.h
	$(CC) $(CFLAGS) $(INC_VC) -o $@ test_vdg.c

test_romfs: test_romfs.c $(COMMON)/romfs.c shim/fakeflash.c
	$(CC) $(CFLAGS) $(INC_MC) -I $(LFS) -DLFS_NO_MALLOC -DLFS_NO_DEBUG \
	    -DLFS_NO_WARN -DLFS_NO_ASSERT \
	    -o $@ test_romfs.c shim/fakeflash.c $(LFS)/lfs.c $(LFS)/lfs_util.c

test_select: test_select.c $(COMMON)/select.c $(COMMON)/stub6809.h
	$(CC) $(CFLAGS) $(INC_MC) -o $@ test_select.c

test_ui: test_ui.c $(COMMON)/ui.c $(COMMON)/ssd1306.c
	$(CC) $(CFLAGS) $(INC_MC) -o $@ test_ui.c $(COMMON)/ssd1306.c

run: $(TESTS)
	@fail=0; for t in $(TESTS); do ./$$t || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "== ALL TESTS PASSED =="; \
	 else echo "== FAILURES =="; exit 1; fi

clean:
	rm -f $(TESTS) out_*.ppm ui_*.txt

.PHONY: all run clean

test_audio: test_audio.c $(VCART)/audio.c $(VCART)/audio.h $(VCART)/video.h
	$(CC) $(CFLAGS) $(INC_VC) -o $@ test_audio.c

test_cfg: test_cfg.c $(COMMON)/cfg.c shim/fakeflash.c
	$(CC) $(CFLAGS) $(INC_VC) -DMC_BOARD_VIDEOCART=1 -o $@ test_cfg.c \
	    shim/fakeflash.c

test_ui_vc: test_ui_vc.c $(COMMON)/ui.c $(COMMON)/ssd1306.c
	$(CC) $(CFLAGS) $(INC_VC) -DMC_BOARD_VIDEOCART=1 -o $@ test_ui_vc.c $(COMMON)/ssd1306.c
