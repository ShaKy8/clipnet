# CLIP//NET. The daemon is the only thing that needs building; the UI is QML.
#
#   make            build daemon/clipnetd
#   make test       unit (C and QML), config-block and IPC tests (no session needed)
#   make test-live  round trip through the real clipboard (needs a session)
#   make asan       unit tests under AddressSanitizer + UBSan

all:
	$(MAKE) -C daemon

test:
	$(MAKE) -C daemon test clipnetd
	tests/scripts/test_blocks.sh
	tests/integration/ipc_test.py
	QT_QPA_PLATFORM=offscreen /usr/lib/qt6/bin/qmltestrunner -input tests/qml -silent

test-live:
	$(MAKE) -C daemon test-live

asan:
	$(MAKE) -C daemon asan

clean:
	$(MAKE) -C daemon clean

.PHONY: all test test-live asan clean
