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
NEON-OBJS-$(CONFIG_UNSTABLE) += aarch64/ops_lookup.gen.o
NEON-OBJS-$(CONFIG_UNSTABLE) += aarch64/ops_neon.gen.o

HOSTPROGS = aarch64/ops_asmgen

$(SUBDIR)aarch64/ops_neon.gen.S: $(SUBDIR)aarch64/ops_asmgen$(HOSTEXESUF)
	$(M)$< -ops > $@.tmp
	$(CP) $@.tmp $@
	$(RM) $@.tmp

$(SUBDIR)aarch64/ops_lookup.gen.c: $(SUBDIR)aarch64/ops_asmgen$(HOSTEXESUF)
	$(M)$< -lookup > $@.tmp
	$(CP) $@.tmp $@
	$(RM) $@.tmp

TESTPROGS += ops_enum_aarch64

sws_ops_entries_aarch64: TAG = GEN
sws_ops_entries_aarch64: $(SUBDIR)tests/ops_enum_aarch64$(EXESUF)
	$(M)$< > libswscale/aarch64/ops_entries.c.tmp
	$(CP) libswscale/aarch64/ops_entries.c.tmp $(SRC_PATH)/libswscale/aarch64/ops_entries.c
	$(RM) libswscale/aarch64/ops_entries.c.tmp

clean::
	$(RM) $(CLEANSUFFIXES:%=libswscale/aarch64/%)
