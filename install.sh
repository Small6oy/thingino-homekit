#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Install rhkd on a Thingino camera over SSH: binary to /usr/bin, init script and defaults from files/.
# The pairing store in /etc/homekit is kept. If a RAM test store exists (/tmp/rhkd/store) and there is no
# flash store yet, it is migrated, so the camera stays paired.
#
#   ./install.sh <camera-ip>        (uses $SSHPASS with sshpass if set, else plain ssh)
set -eu
CAM=${1:?usage: $0 <camera-ip>}
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=$HERE/build/out/rhkd
[ -x "$BIN" ] || { echo "build first: build/docker-build.sh" >&2; exit 1; }

if [ -n "${SSHPASS:-}" ]; then SSH="sshpass -e ssh"; else SSH="ssh"; fi
run() { $SSH "root@$CAM" "$@"; }

echo "Stopping rhkd (init script or test copy)"
run '[ -x /etc/init.d/S98rhkd ] && /etc/init.d/S98rhkd stop >/dev/null 2>&1; killall rhkd 2>/dev/null; true'

echo "Copying files"
run 'cat > /usr/bin/rhkd.new && chmod 755 /usr/bin/rhkd.new && mv /usr/bin/rhkd.new /usr/bin/rhkd' <"$BIN"
run 'cat > /etc/init.d/S98rhkd && chmod 755 /etc/init.d/S98rhkd' <"$HERE/files/etc/init.d/S98rhkd"
run '[ -f /etc/default/rhkd ] || cat > /etc/default/rhkd' <"$HERE/files/etc/default/rhkd"

echo "Setup helper and web UI page"
run 'cat > /usr/bin/hap-setupgen && chmod 755 /usr/bin/hap-setupgen' <"$HERE/build/out/hap-setupgen"
run 'cat > /usr/sbin/rhkd-setup && chmod 755 /usr/sbin/rhkd-setup' <"$HERE/files/usr/sbin/rhkd-setup"
for f in var/www/config-homekit.html var/www/a/config-homekit.js var/www/a/vendor/qrcode.js \
	var/www/a/plugins/homekit.webui.json var/www/x/json-homekit.cgi; do
	run "mkdir -p \$(dirname /$f) && cat > /$f" <"$HERE/files/$f"
done
run 'chmod 755 /var/www/x/json-homekit.cgi'
# Nav entry: plugins.js is generated at firmware build time from a/plugins/*.webui.json; on a running camera
# add the homekit entry once, and refresh the gzip sibling the patched uhttpd serves.
run 'f=/var/www/a/plugins.js
if ! grep -q "\"homekit\": {" $f; then
	sed -i "s|^  cfg.plugins = {|  cfg.plugins = {\n  \"homekit\": {\"label\": \"HomeKit\", \"name\": \"homekit\", \"nav\": [{\"items\": [{\"href\": \"/config-homekit.html\", \"label\": \"HomeKit\"}], \"position\": \"after:Home Assistant\", \"section\": \"ddServices\"}]},|" $f
	sed -i "s|^  cfg.device\[\"gpio\"\] = true;|  cfg.device[\"homekit\"] = true;\n&|" $f
	echo "  nav entry added to plugins.js"
fi
if [ -f $f.gz ]; then gzip -9 -n -k -f $f; fi'

echo "Pairing store"
run 'set -e
mkdir -p /etc/homekit && chmod 700 /etc/homekit
if [ ! -d /etc/homekit/store ] && [ -d /tmp/rhkd/store ]; then
	cp -a /tmp/rhkd/store /etc/homekit/store
	[ -f /tmp/rhkd/setup.txt ] && cp /tmp/rhkd/setup.txt /etc/homekit/setup.txt
	echo "  migrated /tmp/rhkd/store -> /etc/homekit/store"
fi
[ -f /etc/homekit/setup.txt ] && chmod 600 /etc/homekit/setup.txt
ls /etc/homekit/store 2>/dev/null | wc -l | sed "s/^/  store entries: /"'
run 'rhkd-setup provision | sed "s/^/  /"'

echo "Codecs (HomeKit needs an H.264 stream and Opus audio; changed only if needed)"
run 'before=$(rhkd-setup codecs); after=$(rhkd-setup fixcodecs) || echo "  fixcodecs failed"
echo "  before: $before"; [ "$before" = "$after" ] || echo "  after:  $after"'

echo "Starting"
run '/etc/init.d/S98rhkd start; sleep 3; pidof rhkd >/dev/null && echo "  rhkd running (pid $(pidof rhkd))"; logread | grep "rhkd\[" | tail -5'
