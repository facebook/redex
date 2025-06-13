SUBDIRS = . samples integ unit

check_SCRIPTS = check_num_passes.sh
# _DEPENDENCIES does not seem to work for SCRIPTS
check_num_passes.sh: $(top_builddir)/redex-all

TESTS = check_num_passes.sh
