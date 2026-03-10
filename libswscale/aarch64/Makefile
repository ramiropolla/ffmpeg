OBJS        += aarch64/rgb2rgb.o                \
               aarch64/swscale.o                \
               aarch64/swscale_unscaled.o       \

NEON-OBJS   += aarch64/hscale.o                 \
               aarch64/input.o                  \
               aarch64/output.o                 \
               aarch64/range_convert_neon.o     \
               aarch64/rgb2rgb_neon.o           \
               aarch64/swscale_unscaled_neon.o  \
               aarch64/xyz2rgb_neon.o           \
               aarch64/yuv2rgb_neon.o           \

NEON-OBJS-$(CONFIG_UNSTABLE) += aarch64/ops.o
NEON-OBJS-$(CONFIG_UNSTABLE) += aarch64/ops_impl.o
NEON-OBJS-$(CONFIG_UNSTABLE) += aarch64/ops_lookup.o
NEON-OBJS-$(CONFIG_UNSTABLE) += aarch64/ops_neon.o

HOSTPROGS = aarch64/ops_asmgen$(HOSTEXESUF)

$(SUBDIR)aarch64/ops_neon.S: $(SUBDIR)aarch64/ops_asmgen$(HOSTEXESUF)
	$(M)$< -ops > $@ || $(RM) $@

$(SUBDIR)aarch64/ops_lookup.c: $(SUBDIR)aarch64/ops_asmgen$(HOSTEXESUF)
	$(M)$< -lookup > $@ || $(RM) $@

sws_ops_entries_aarch64: TAG = GEN
sws_ops_entries_aarch64: $(SUBDIR)tests/sws_ops$(EXESUF)
	$(M)$< -backend aarch64 -quiet 1 -print_ops $(SRC_PATH)/libswscale/aarch64/ops_entries.c
