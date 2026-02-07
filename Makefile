ABSDIR := $(shell pwd)

xinit:
	@xbstrap init .

image:
	@xbstrap build --all
	@sudo $(ABSDIR)/scripts/make-efi-img.sh --force

run:
	@sudo $(ABSDIR)/scripts/run-qemu.sh

debug:
	@sudo $(ABSDIR)/scripts/debug-qemu.sh

bdebug: image
	@sudo $(ABSDIR)/scripts/debug-qemu.sh

clean:
	@sudo $(ABSDIR)/scripts/clean-artifacts.sh --mode normal

ktest:
	@make -C src/ ktest

xclean:
	@rm -rf .xbstrap sysroot tools bundled pkg-builds tool-builds packages
	@rm -f bootstrap.link
	@find . -name "*.xbstrap" -type f -delete

distclean: xclean
	@sudo $(ABSDIR)/scripts/clean-artifacts.sh --mode dist

.PHONY: image run debug clean distclean xclean setup