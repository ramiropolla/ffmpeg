clean::
	$(RM) $(CLEANSUFFIXES:%=fftools/resources/%)
	$(RM) fftools/resources/resources_list.c

RESMAN-OBJS += fftools/resources/resman.o

fftools/resources/resources_list.c: $(RESOBJS)
	$(RM) $@
	$(M)$(foreach R,$(notdir $(RESOBJS:.o=)),echo 'extern const unsigned char ff_$(subst .,_,$(R))_data[];' >> $@;)
	$(Q)$(foreach R,$(notdir $(RESOBJS:.o=)),echo 'extern const unsigned int ff_$(subst .,_,$(R))_len;' >> $@;)
	$(Q)echo 'static const FFResourceDefinition resource_definitions[] = {' >> $@
	$(Q)$(foreach R,$(notdir $(RESOBJS:.o=)),echo '    { "$(R:.res=)", &ff_$(subst .,_,$(R))_data[0], &ff_$(subst .,_,$(R))_len },' >> $@;)
	$(Q)echo '};' >> $@

fftools/resources/resman.o: fftools/resources/resources_list.c

# Disable dependency checking to prevent spurious rebuilds
$(RESOBJS): CCDEP       =
$(RESOBJS): CC_DEPFLAGS =

# Uncomment to prevent deletion during build
#.SECONDARY: $(RESOBJS:.o=.gz) $(RESOBJS:.o=.c) $(RESOBJS:.res.o=)
