d := $(dir $(lastword $(MAKEFILE_LIST)))

include \
	$(d)vr/Rules.mk \
	$(d)fastpaxos/Rules.mk \
	$(d)unreplicated/Rules.mk \
	$(d)spec/Rules.mk \
	$(d)nopaxos/Rules.mk \
	$(d)signedunrep/Rules.mk \
	$(d)tombft/Rules.mk \
	$(d)hotstuff/Rules.mk \
	$(d)pbft/Rules.mk
