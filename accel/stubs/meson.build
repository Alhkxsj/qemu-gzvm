system_stubs_ss = ss.source_set()
system_stubs_ss.add(when: 'CONFIG_TCG', if_false: files('tcg-stub.c'))
system_stubs_ss.add(when: 'CONFIG_GZVM', if_false: files('gzvm-stub.c'))
if 'CONFIG_GZVM' not in config_all_accel
error('Only GZVM acceleration is kept in this trimmed build')
endif
specific_ss.add_all(when: ['CONFIG_SYSTEM_ONLY'], if_true: system_stubs_ss)
