#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# HomeKit (rhkd) status and pairing actions for the web UI.
#   GET                         -> rhkd-setup status (JSON)
#   POST {"action":"unpair"}    -> forget all controllers
#   POST {"action":"newcode"}   -> new setup code (also unpairs)
#   POST {"action":"restart"}   -> restart rhkd
#   POST {"action":"fixcodecs"} -> make camera codecs HomeKit-compatible (Opus audio, an H.264 stream)
# shellcheck disable=SC1091

. /var/www/x/auth.sh
require_auth

send_json() {
	printf 'Status: %s\n' "${2:-200 OK}"
	printf 'Content-Type: application/json\nCache-Control: no-store\nPragma: no-cache\nConnection: close\n\n'
	printf '%s\n' "$1"
	exit 0
}

error() { send_json "{\"error\":{\"message\":\"$1\"}}" "${2:-400 Bad Request}"; }

command -v rhkd-setup >/dev/null 2>&1 || error "rhkd is not installed" "404 Not Found"

case "$REQUEST_METHOD" in
	GET)
		send_json "$(rhkd-setup status)"
		;;
	POST)
		len=${CONTENT_LENGTH:-0}
		case "$len" in '' | *[!0-9]*) len=0 ;; esac
		[ "$len" -gt 0 ] && [ "$len" -lt 1024 ] || error "missing request body"
		body=$(head -c "$len")
		action=$(printf '%s' "$body" | sed -n 's/.*"action"[[:space:]]*:[[:space:]]*"\([a-z]*\)".*/\1/p')
		case "$action" in
			unpair | newcode)
				out=$(rhkd-setup "$action" 2>&1) || error "rhkd-setup $action failed: $(printf '%s' "$out" | tr -d '"\\' | tail -n 1)" "500 Internal Server Error"
				send_json "$(printf '%s' "$out" | tail -n 1)"
				;;
			fixcodecs)
				out=$(rhkd-setup fixcodecs 2>&1) || error "rhkd-setup fixcodecs failed: $(printf '%s' "$out" | tr -d '"\\' | tail -n 1)" "500 Internal Server Error"
				send_json "$(rhkd-setup status)"
				;;
			restart)
				/etc/init.d/S98rhkd restart >/dev/null 2>&1
				sleep 2
				send_json "$(rhkd-setup status)"
				;;
			*) error "unknown action" ;;
		esac
		;;
	*)
		error "method not allowed" "405 Method Not Allowed"
		;;
esac
