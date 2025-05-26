clean::
	$(RM) $(CLEANSUFFIXES:%=fftools/resources/%)

vpath %.res $(SRC_PATH)

RESOURCEOBJS += fftools/resources/graph.css.min.res.o
RESOURCEOBJS += fftools/resources/graph.html.res.o

OBJS-resman +=                     \
    fftools/resources/resman.o     \
    $(RESOURCEOBJS)

%.css.min.res: TAG = SED
%.css.min.res: %.css.res
	$(M)sed 's!/\\*.*\\*/!!g' $< \
	| tr '\n' ' ' \
	| tr -s ' ' \
	| sed 's/^ //; s/ $$//' \
	> $@

ifdef CONFIG_RESOURCE_COMPRESSION
RES_GZ = .gz
endif

%.res.gz: TAG = GZIP
%.res.gz: %.res
	$(M)gzip -nc9 $< > $@

%.res.c: %.res$(RES_GZ) $(BIN2CEXE)
	$(BIN2C) $< $@ $(subst .,_,$(basename $(notdir $@)))

.SECONDARY: $(RESOURCEOBJS:.o=) $(RESOURCEOBJS:.o=.c) $(RESOURCEOBJS:.o=.gz)
