# CLIP//NET. The daemon is the only thing that needs building; the UI is QML.
#
#   make            build daemon/clipnetd
#   make test       unit tests + config-block tests
#   make test-live  round trip through the real clipboard (needs a session)
#   make asan       unit tests under AddressSanitizer + UBSan

all:
	$(MAKE) -C daemon

test:
	$(MAKE) -C daemon test
	tests/scripts/test_blocks.sh

test-live:
	$(MAKE) -C daemon test-live

asan:
	$(MAKE) -C daemon asan

clean:
	$(MAKE) -C daemon clean

.PHONY: all test test-live asan clean
