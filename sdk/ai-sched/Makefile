HOSTCC ?= cc
HOSTCFLAGS ?= -Wall -Wextra -Werror -std=c99
ADVISOR := out/ai_sched_advisor

.PHONY: advisor clean

advisor: $(ADVISOR)

$(ADVISOR): advisor/ai_sched_advisor.c advisor/untrained_model.h
	@mkdir -p out
	$(HOSTCC) $(HOSTCFLAGS) -Iadvisor $< -o $@

clean:
	rm -rf out
