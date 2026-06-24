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
NEON-OBJS-$(CONFIG_UNSTABLE) += aarch64/ops_neon.gen.o

# TODO if jit
NEON-OBJS-$(CONFIG_UNSTABLE) += aarch64/ops_jit.o
# NEON-OBJS-$(CONFIG_UNSTABLE) += aarch64/ops_asmgen_jit.o
NEON-OBJS-$(CONFIG_UNSTABLE) += aarch64/rasm.o
NEON-OBJS-$(CONFIG_UNSTABLE) += aarch64/rasm_print.o

$(SUBDIR)aarch64/ops_neon.gen.S: $(SUBDIR)aarch64/ops_asmgen$(HOSTEXESUF)
	$(M)$< -ops > $@.tmp
	$(CP) $@.tmp $@
	$(RM) $@.tmp

clean::
	$(RM) $(CLEANSUFFIXES:%=libswscale/aarch64/%)

HOSTPROGS = aarch64/ops_asmgen
