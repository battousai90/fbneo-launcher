#!/bin/bash

# Clear snap-contaminated environment variables
unset SNAP
unset SNAP_ARCH
unset SNAP_LIBRARY_PATH
unset SNAP_COMMON
unset SNAP_DATA
unset SNAP_INSTANCE_KEY
unset SNAP_INSTANCE_NAME
unset SNAP_NAME
unset SNAP_REAL_HOME
unset SNAP_REEXEC
unset SNAP_REVISION
unset SNAP_USER_COMMON
unset SNAP_USER_DATA
unset SNAP_VERSION

# Set clean environment
export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/usr/games:/usr/local/games"
export LD_LIBRARY_PATH="/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu"
export XDG_DATA_DIRS="/usr/share/ubuntu:/usr/share/gnome:/usr/local/share/:/usr/share/"

# Execute the program
exec "$@"
