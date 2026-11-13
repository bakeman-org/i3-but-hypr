__sample_config_push:
	cp ./etc/config ~/.config/i3/config

__sync_local_config:
	cp ~/.config/i3/config ./etc/config

__test:
	ninja -C build/
	./build/i3 -c ./etc/config --nest