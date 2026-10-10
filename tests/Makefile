# Host-side unit tests for the living-kotor2 bridge modules that contain no
# Windows code (wire codec, mailbox). Run on Linux:  make -C tests
CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O1 -I../src
OUT := ../out/tests
TESTS := $(OUT)/lk2_proto_test $(OUT)/lk2_mailbox_test

all: $(TESTS)
	@set -e; for t in $(TESTS); do $$t; done

proto: $(OUT)/lk2_proto_test
	$<
mailbox: $(OUT)/lk2_mailbox_test
	$<

$(OUT)/lk2_proto_test: lk2_proto_test.cpp ../src/lk2_proto.cpp ../src/lk2_proto.h
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) lk2_proto_test.cpp ../src/lk2_proto.cpp -o $@

$(OUT)/lk2_mailbox_test: lk2_mailbox_test.cpp ../src/lk2_mailbox.cpp ../src/lk2_mailbox.h
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) lk2_mailbox_test.cpp ../src/lk2_mailbox.cpp -o $@

clean:
	rm -rf $(OUT)
.PHONY: all proto mailbox clean
