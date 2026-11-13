#!/bin/sh
# Runs as root after installing/upgrading the i3-but-hypr package. Restarting a
# user's window manager needs the user's DISPLAY and IPC socket, so this script
# only tells the user how to finish the upgrade without logging out.
set -e

cat <<'EOF'

i3-but-hypr: the new version is installed. To switch the running session to it
(and to the packaged configuration) without logging out, run inside your
session:

    i3-but-hypr-upgrade

EOF

exit 0
