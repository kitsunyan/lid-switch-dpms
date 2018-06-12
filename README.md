# lid-switch-dpms

lid-switch-dpms is a tool to blank and unblank the screen on lid switch using DPMS. This tool might
be useful on some machines, for instance, on my librebooted Lenovo ThinkPad X200.

## Building and Installing

Run `make && make install` to build and install lid-switch-dpms to your system.
Run `systemctl daemon-reload` to reload unit files.
Enable and start `lid-switch-dpms.service`.

## Troubleshooting

### X Session Gets Locked After Lid Switch

Some programs (e.g. light-locker) can listen for DPMS events and lock the session.
In case of light-locker you can rebuild it with `--without-mit-ext` and `--without-dpms-ext`
configuration flags.
