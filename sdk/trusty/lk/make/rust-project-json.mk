#
# Build a rust-project.json for use by rust-analyzer
#
# Builds a rust-project.json in the current builddir using the contents of
# RUST_ANALYZER_CONTENTS as the body. RUST_ANALYZER_CONTENTS must be a valid
# concatenated list of json objects.
#
# Inputs:
# BUILDDIR
# RUST_ANALYZER_CONTENTS

RUST_PROJECT_JSON := $(BUILDDIR)/rust-project.json
define RUST_PROJECT_JSON_CONTENTS :=
{
	"crates": [
		$(call STRIP_TRAILING_COMMA,$(RUST_ANALYZER_CONTENTS))
	]
}
endef
RUST_PROJECT_JSON_CONTENTS := $(subst $(NEWLINE),\n,$(RUST_PROJECT_JSON_CONTENTS))
RUST_PROJECT_JSON_CONTENTS := $(subst %,%%,$(RUST_PROJECT_JSON_CONTENTS))
.PHONY: $(RUST_PROJECT_JSON)
$(RUST_PROJECT_JSON): CONTENTS :=  $(RUST_PROJECT_JSON_CONTENTS)
$(RUST_PROJECT_JSON):
	@$(MKDIR)
	@echo Creating rust-project.json for rust-analyzer
	$(NOECHO)printf '$(CONTENTS)' > $@

EXTRA_BUILDDEPS += $(RUST_PROJECT_JSON)

RUST_ANALYZER_CONTENTS :=
RUST_PROJECT_JSON :=
RUST_PROJECT_JSON_CONTENTS :=
