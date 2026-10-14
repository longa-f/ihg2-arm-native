CXX ?= g++
CC ?= cc
CXXFLAGS ?= -std=c++20 -Os -Wall -Wextra -Wpedantic -Werror -fno-rtti
CPPFLAGS += -Iinclude -Ithird_party/nlohmann_json/include
OPENSSL_LIBS ?= -lcrypto
BUILD_DIR ?= build

.PHONY: all test clean android package-android

all: $(BUILD_DIR)/state_store_test $(BUILD_DIR)/protocol_test $(BUILD_DIR)/homekit_cover_test $(BUILD_DIR)/homekit_color_temperature_test $(BUILD_DIR)/homekit_thermostat_test $(BUILD_DIR)/panel_config_test $(BUILD_DIR)/log_writer_test

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/state_store_test: src/state_store.cpp src/gateway_socket.cpp tests/state_store_test.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(BUILD_DIR)/protocol_test: src/vendor_protocol/protocol.cpp src/gateway_socket.cpp tests/protocol_test.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ $(OPENSSL_LIBS) -o $@

$(BUILD_DIR)/homekit_cover_test: tests/homekit_cover_test.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(BUILD_DIR)/homekit_color_temperature_test: tests/homekit_color_temperature_test.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(BUILD_DIR)/homekit_thermostat_test: tests/homekit_thermostat_test.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(BUILD_DIR)/panel_config_test: src/panel_config.cpp src/vendor_protocol/protocol.cpp tests/panel_config_test.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ $(OPENSSL_LIBS) -o $@

$(BUILD_DIR)/log_writer.o: src/log_writer.c include/ihg2_arm/log_writer.h | $(BUILD_DIR)
	$(CC) -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra -Wpedantic -Werror -Iinclude -c $< -o $@

$(BUILD_DIR)/log_writer_test: tests/log_writer_test.cpp $(BUILD_DIR)/log_writer.o | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

test: all
	./$(BUILD_DIR)/state_store_test
	./$(BUILD_DIR)/protocol_test
	./$(BUILD_DIR)/homekit_cover_test
	./$(BUILD_DIR)/homekit_color_temperature_test
	./$(BUILD_DIR)/homekit_thermostat_test
	./$(BUILD_DIR)/panel_config_test
	./$(BUILD_DIR)/log_writer_test

android:
	./scripts/build-android.sh

package-android: android
	./scripts/package-android.sh

clean:
	rm -rf $(BUILD_DIR)
