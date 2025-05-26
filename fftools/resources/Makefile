RES_CLEANSUFFIXES = *.o *.css.min.c *.html.c *.d *.gz *.min

clean::
	$(RM) $(RES_CLEANSUFFIXES:%=fftools/resources/%)

vpath %.html $(SRC_PATH)
vpath %.css  $(SRC_PATH)

RESOURCEOBJS += fftools/resources/graph.css.min.o
RESOURCEOBJS += fftools/resources/graph.html.o

OBJS-resman +=                     \
    fftools/resources/resman.o     \
    $(RESOURCEOBJS)

%.css.min: TAG = SED
%.css.min: %.css
	$(M)sed 's!/\\*.*\\*/!!g' $< \
	| tr '\n' ' ' \
	| tr -s ' ' \
	| sed 's/^ //; s/ $$//' \
	> $@

ifdef CONFIG_RESOURCE_COMPRESSION
RES_GZ = .gz
endif

$(RESOURCEOBJS:.o=.gz): TAG = GZIP
$(RESOURCEOBJS:.o=.gz): %.gz: %
	$(M)gzip -nc9 $< > $@

$(RESOURCEOBJS:.o=.c): %.c: %$(RES_GZ) $(BIN2CEXE)
	$(BIN2C) $< $@ $(subst .,_,$(basename $(notdir $@)))

.SECONDARY: $(RESOURCEOBJS:.o=) $(RESOURCEOBJS:.o=.c) $(RESOURCEOBJS:.o=.gz)
