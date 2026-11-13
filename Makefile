__sample_config_push:
	cp ./etc/config ~/.config/i3/config


__sync_local_config:
	cp ~/.config/i3/config ./etc/config


# Switch the running session to ./build/i3 and ./etc/config without logging
# out (run `ninja -C build` first):
#   make __upgrade
__upgrade:
	./i3-but-hypr-upgrade --binary ./build/i3 --config ./etc/config
