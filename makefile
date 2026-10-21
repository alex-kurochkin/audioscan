BUILD_DIR   := build
CMAKE_FLAGS := -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
JOBS        := $(shell nproc)

.PHONY: all clean install

all:
	@mkdir -p $(BUILD_DIR)
	cd $(BUILD_DIR) && cmake $(CMAKE_FLAGS) ..
	cd $(BUILD_DIR) && make -j$(JOBS)

clean:
	rm -rf $(BUILD_DIR)

install: all
	cd $(BUILD_DIR) && make DESTDIR=$(DESTDIR) install
